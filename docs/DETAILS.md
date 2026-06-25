# Architecture & Troubleshooting Guide

## Hardware

- **Board**: M5Stack Atom Echo (ESP32-PICO-D4)
- **Microphone**: SPM1423 PDM MEMS (built-in)
- **Audio**: 16-bit PCM, 16kHz mono (configurable 8–48kHz)
- **Streaming**: RTSP/RTP over TCP, port 8554

### Pin Configuration
```cpp
I2S_BCLK_PIN    = 19  // Bit Clock
I2S_LRCLK_PIN   = 33  // Left-Right Clock / Word Select
I2S_DATA_IN_PIN = 23  // Microphone Data Input (PDM)
I2S_DATA_OUT_PIN = 22 // Speaker Data Output (not used)
```

## Architecture

### Dual-Core Design (v2.3.0)

- **Core 1**: Complete audio pipeline (I2S capture → HPF → AGC → gain → RTP → WiFi) + RTSP keepalive/TEARDOWN processing
- **Core 0**: Web UI, RTSP negotiation, diagnostics, client management

Core 1 **exclusively owns** the WiFiClient socket during streaming — Core 0 never touches it:

1. Core 0 handles RTSP negotiation (OPTIONS → DESCRIBE → SETUP → PLAY)
2. On PLAY, Core 0 hands off the socket to Core 1 via `streamClient` pointer with memory barrier
3. Core 1 reads audio, processes, sends RTP packets, and handles RTSP keepalives/TEARDOWN
4. On disconnect, Core 1 closes the socket itself; Core 0 detects the transition and updates LED/logs
5. When Core 0 needs to stop streaming (TEARDOWN, overheat, WiFi loss), it signals via `requestStreamStop()` and waits for Core 1 to confirm cleanup

### Cross-Core Safety
- FreeRTOS semaphore with 2s timeout for confirmed task exit
- `portMUX_TYPE` spinlock for shared log buffer
- Xtensa `memw` memory barriers on critical flag transitions
- `core1OwnsLED` flag prevents concurrent FastLED/RMT driver access

## WireGuard Tunnel

### Overview

An optional WireGuard tunnel is provided by a new `WireGuardManager` module
(`src/WireGuardManager.cpp` / `.h`). The tunnel is **opt-in and client-only**: the device
initiates outbound UDP to a configured server endpoint and never listens for inbound
WireGuard traffic. It is built on a vendored copy of
`ciniml/WireGuard-ESP32-Arduino` v0.1.5 (under `lib/WireGuard-ESP32/`, which also bundles
the `wireguard-lwip` dependency) with the modifications described below.

When the tunnel is up the device has a routable tunnel IP, and `rtsp://<tunnel-ip>:8554/`
is byte-for-byte identical RTSP/RTP to a LAN stream — fully transparent to any RTSP
client, including BirdNet-Go. The only required step is entering the tunnel-IP source URL
on the consuming side.

### Threading Model

All WireGuard activity — setup, teardown, retry, status reads, and web handler
invocations — runs on **Core 0 only**, driven from `wg_tick()` in the main loop and the
web request handlers (which are also served on Core 0). Core 1's audio path is
untouched: the RTSP server still binds `0.0.0.0:8554`, and lwIP routes inbound
connections to the appropriate `netif`, so the tunnel IP transparently delivers RTSP
traffic when the consuming client connects to it. No changes are made to the audio
pipeline or the socket-ownership model described above.

### Time-Sync Gating

WireGuard handshakes require a sane wall clock. The tunnel connect is therefore gated on
`time(&now) > 100000` (a valid NTP-synced clock). If NTP is not yet valid, the tunnel
start is deferred and re-attempted from `wg_tick()` once time is valid.

### State Machine

```
DISABLED → WAIT_TIME → WAIT_WIFI → CONNECTING → UP
```

The module retries indefinitely with backoff (5s → 30s cap) on handshake failure or peer
down. Any configuration transition cancels any in-flight setup and resets the state:

- `wg_setEnabled()`
- `wg_setEndpoint()`
- `wg_setPrivateKey()`
- `wg_setServerPublicKey()`
- `wg_setTunnelAddress()`
- `wg_setKeepalive()`

A successful peer-up transition resets the backoff to 5s. On peer-down, the module waits
up to 30s before tearing the interface down and reconnecting.

### Async DNS Resolution

Endpoint hostname resolution runs in a dedicated FreeRTOS task (`_dnsResolveTask`,
pinned to Core 0) with a 5-attempt backoff schedule:

```
delays[] = {0ms, 500ms, 500ms, 1s, 2s}   // total wait ≤ ~4s + per-attempt DNS timeout
```

This keeps the web UI responsive even when the endpoint host's DNS is slow. A generation
counter (`s_dnsGeneration`) is bumped on every configuration change or cancellation; the
DNS task checks its captured generation on each attempt and aborts early if it no longer
matches, preventing stale DNS results from being applied to a different endpoint.

### NVS Namespace Isolation

All WireGuard settings live in a dedicated `"wg"` `Preferences` namespace, completely
separate from the `"audio"` namespace that holds RTSP/Audio configuration. The two
namespaces are erased independently by the reset actions, giving clean scoping:

| Action | `"wg"` | `"audio"` | WiFi credentials (system NVS) | Confirm dialog |
|--------|:------:|:---------:|:-----------------------------:|:--------------:|
| Reboot | preserved | preserved | preserved | none |
| Reset I2S | preserved | preserved | preserved | none |
| Factory Reset | cleared | cleared | preserved | yes |
| Reset Wi-Fi | preserved | preserved | cleared | yes |

WireGuard keys are never added to the `"audio"` namespace. Reset Wi-Fi clears only
WiFi credentials via `WiFiManager::resetSettings()` (the deferred-reboot pattern, so the
restart does not happen from HTTP context), then reboots into the captive portal. After
the end user joins WiFi, the preserved WireGuard configuration auto-connects.

Factory Reset wipes both `"audio"` and `"wg"` namespaces in a single action — it is the
device's factory reset while still preserving the Wi-Fi
connection.

### Configurable Hostname

The device hostname is user-configurable through the **Device** card in the Web UI.
The hostname drives:

- mDNS `.local` name (e.g. `kitchen-echo.local`)
- DHCP hostname (advertised to the router, visible in DHCP client lists)
- Browser tab title in the Web UI

**Validation**: RFC 1123 single-label rules enforced server-side with auto-normalization:
lowercase letters, digits, and hyphens only; no leading/trailing hyphens; max 63 characters.
Invalid characters are stripped automatically. An empty or all-invalid input falls back to
the default `atomecho-<mac6>` (last 6 hex digits of the device's MAC address, lowercase).

**Storage**: The hostname is persisted in the `"audio"` NVS namespace under key `"hostname"`.

**Reset behavior**:
- **Factory Reset** → hostname reverts to the per-device default `atomecho-<mac6>` (NVS cleared)
- **Reset Wi-Fi** → hostname preserved (intended for provisioning workflow: admin sets
  hostname, resets Wi-Fi, ships device; end user joins WiFi without reconfiguring hostname)

**Apply timing**: Changing the hostname reboots the device (same deferred-reboot pattern as
other settings) so mDNS and DHCP reinitialize cleanly.

**Upgrade note**: Devices flashed before v2.5.0 that have the old default `atomecho`
already stored in NVS (either set explicitly or written by a prior factory-reset path
that used the old `atomecho` string) will retain `atomecho` after an OTA update — NVS is
not cleared on upgrade. To adopt the new per-device default `atomecho-<mac6>`, perform a
Factory Reset from the Web UI (preserves WiFi) or reflash the firmware and run
`pio run -t uploadfs`.

#### Setup/Recovery AP SSID

The captive-portal setup AP uses a per-device SSID: `ESP32-RTSP-Mic-<MAC6>`, where
`<MAC6>` is the last 6 hex digits of the device's MAC address (e.g.
`ESP32-RTSP-Mic-AB12CD`). This keeps the AP recognizable while disambiguating multiple
devices in setup mode at the same location. The setup AP SSID is:

- Derived at runtime from `ESP.getEfuseMac()` (not persisted)
- Independent of the configured hostname (decoupled for recoverability)
- Fixed prefix `ESP32-RTSP-Mic-` ensures the AP is always findable

### Web UI Surface

Four new cards are added to the single-page app, following the existing dark-theme card
pattern:

1. **Device** — hostname input with live preview, RFC 1123 help text, hostname note
   explaining reboot-on-save behavior. The hostname, mDNS `.local` name, DHCP hostname,
   and browser tab title are configured here.
2. **WireGuard** — configuration form: enable toggle, private key, server public key,
   endpoint (`host:port`), tunnel IP (CIDR), keepalive (seconds).
3. **WireGuard Status** — tunnel state, last handshake age, rx/tx bytes (human-readable),
   and a color-coded state badge matching the existing RTSP server toggle. Includes the
    Reset Wi-Fi button. Note: the admin can reach this web UI over the tunnel at
   `http://<tunnel-ip>/` from any peer on the same WireGuard network.
4. **RTSP URLs** — LAN URL is always shown; the WireGuard URL
   (`rtsp://<tunnel-ip>:8554/`) is shown only when the tunnel is up. Both URLs are
   rendered as clickable hyperlinks (opening in VLC when clicked) and each has a Copy
   button (reusing the existing copy-to-clipboard JS pattern).

The new `/api/wg_status` JSON endpoint is polled at the same 3-second interval as the
other status endpoints. Logs are polled at a separate 9-second interval (`/api/logs` is
the heaviest payload, so decoupling it from the status poll reduces web server contention
during active UI use).

#### WireGuard `.conf` Import

A browser-side `.conf` file importer is integrated into the WireGuard card. It parses
standard `wg-quick(8)` format files entirely in JavaScript with no new C++ endpoints:

1. User clicks **Import Config**, which triggers the OS file picker via a hidden
   `<input type="file">`.
2. The file is read with `FileReader.readAsText()`, then parsed by `parseWgConf()`.
3. Validation runs atomically before any `/api/set` calls:
   - Exactly one `[Peer]` section required; multi-peer configs rejected.
   - `Address` field must contain an IPv4 CIDR; IPv6-only configs rejected.
   - `PrivateKey` and `PublicKey` must be 44-character base64 strings (trailing `=`).
   - `Endpoint` must match `host:port` format.
   - `PresharedKey` presence triggers a warning but does not block import (PSK is not
     supported by the firmware).
4. On success, a preview panel shows parsed values (private key masked as
   `XXXX…XXXX`) with **Apply** and **Cancel** buttons.
5. On apply, `setv()` fires individual `/api/set?key=wg_*&value=…` calls for each
   imported field, reusing all existing input validation and NVS persistence paths.

#### UI Responsiveness Optimizations

Several client-side patterns reduce perceived latency:

- **Optimistic toggle:** The Server ON/OFF button and its `.active` class flip
  synchronously on click before the network request fires. The next 3-second
  `loadStatus()` poll reconciles the button to the actual server state.
- **Slim post-action refresh:** The `act()` helper refreshes only `loadStatus()` instead
  of the full 6-endpoint `loadAll()`, cutting post-click requests from 6 to 1. Reset
  and reboot actions use their own `rebootSequence()` and are unaffected.
- **Decoupled log polling:** `loadLogs()` runs on a separate 9-second interval,
  independent of the 3-second status poll, reducing HTTP request contention at the
  ESP32 web server.

#### `/api/wg_status` Response

```json
{
  "enabled": true,
  "state": "up",
  "last_handshake": "12s ago",
  "rx_bytes": 1048576,
  "tx_bytes": 524288,
  "rx_pretty": "1.0 MB",
  "tx_pretty": "512 KB",
  "tunnel_ip": "10.99.0.2",
  "tunnel_addr": "10.99.0.2/24",
  "endpoint": "wg.example.com:51820",
  "keepalive": 25,
  "private_key": "********",
  "server_public_key": "<base64>",
  "lan_url": "rtsp://192.168.1.42:8554/",
  "wg_url": "rtsp://10.99.0.2:8554/"
}
```

Strings are localized to English and Czech.

### Security Model

The private key is stored in NVS in plaintext, matching the same convention used for
WiFiManager's WiFi password storage. The web API never returns the private key in full:

- `/api/wg_status` masks it as `"********"` (empty string when unset).
- `httpSet()` masks the logged value as `"********"` for the `wg_priv` key.

The server public key is non-sensitive and is returned in full so the UI can re-populate
the input field.

### Vendored Library Modifications

`lib/WireGuard-ESP32/` is a pinned copy of v0.1.5 (which bundles the `wireguard-lwip`
dependency) with three modifications:

1. **`PersistentKeepalive` support** — `WireGuard::begin()` gains a
   `persistentKeepalive` parameter (default 25s) that sets `peer.keep_alive` in the
   `wireguardif_peer` struct. This is required because the consuming RTSP client
   initiates the inbound TCP connection to the device's tunnel IP, so the device's NAT
   mapping must stay open even when idle.

   ```cpp
   bool WireGuard::begin(const IPAddress& localIP, const IPAddress& Subnet,
                         const IPAddress& Gateway, const char* privateKey,
                         const char* remotePeerAddress, const char* remotePeerPublicKey,
                         uint16_t remotePeerPort, uint16_t persistentKeepalive);
   ```

2. **rx/tx byte counters** — `uint64_t rx_bytes` and `uint64_t tx_bytes` are added to
   `struct wireguard_peer` and incremented in `wireguardif_process_data_message()` and
   `wireguardif_output_to_peer()` respectively.

3. **Public accessors** — added to the `WireGuard` class:

   | Accessor | Returns |
   |----------|---------|
   | `isPeerUp()` | `true` if `wireguardif_peer_is_up()` succeeds |
   | `lastHandshakeMs()` | `millis()` timestamp of the current/previous keypair |
   | `rxBytes()` | bytes received over the tunnel (from the peer struct) |
   | `txBytes()` | bytes transmitted over the tunnel (from the peer struct) |

### Configuration Storage

The `"wg"` namespace keys:

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `en` | bool | false | WireGuard enabled |
| `priv` | string | `""` | Device private key (base64) |
| `srvpub` | string | `""` | Server public key (base64) |
| `endhost` | string | `""` | Server endpoint hostname or IP |
| `endport` | uint16 | 51820 | Server endpoint UDP port |
| `tunaddr` | string | `""` | Device tunnel address (CIDR, e.g. `10.99.0.2/24`) |
| `keepalive` | uint16 | 25 | PersistentKeepalive interval (seconds) |

WireGuard key pairs can be generated on any machine with the `wg` tools:

```sh
wg genkey | tee private.key | wg pubkey > public.key
```

The private key is entered into the device's web UI; the corresponding public key is
added as a peer in the WireGuard server configuration.

## Audio Tuning

### Signal Levels
- **Target**: 30–70% (about -10 to -3 dBFS)
- **LED green**: Good level
- **LED orange**: Getting hot — consider reducing gain
- **LED red**: Clipping — reduce gain immediately
- **LED dim purple**: Very quiet — increase gain or enable AGC

### AGC vs Manual Gain
- **AGC OFF**: Consistent, predictable levels (e.g., close-range recording)
- **AGC ON**: Outdoor BirdNET-Go deployment where bird distance varies. AGC multiplies on top of your manual gain setting.

### Buffer Size Profiles

| Size | Latency | Stability | Use Case |
|------|---------|-----------|----------|
| 256 | 16ms | Low | Ultra-low latency, may drop packets |
| 512 | 32ms | Medium | Balanced for good WiFi |
| **1024** | **64ms** | **High** | **Recommended — stable streaming** |
| 2048+ | 128ms+ | Very High | Poor WiFi, maximum stability |

## Web UI Features

- IP address and WiFi signal strength
- WiFi TX power control (-1.0 to 19.5 dBm)
- Free heap memory and system uptime
- RTSP connection status and packet rate
- Real-time signal level and clipping detection
- Audio settings (sample rate displayed in kHz, gain, buffer, HPF, AGC)
- CPU frequency selection (80, 120, 160, 240 MHz)
- Thermal protection config (30–95°C limit)
- Auto recovery and scheduled resets
- Timestamped log viewer with copy button
- Reset controls: `Reboot`, `Reset I2S`, `Factory Reset` (wipes audio + WireGuard, preserves WiFi; confirmation dialog), `Reset Wi-Fi` (wipes only WiFi; confirmation dialog)

## Troubleshooting

### LED is Yellow (Stuck in Startup)
- Check Serial Monitor for errors
- WiFi credentials may be incorrect
- Reset WiFi: connect to `ESP32-RTSP-Mic-<MAC-suffix>` (each device has a unique suffix) and reconfigure

### LED is Red (Not Streaming)
- Thermal protection triggered
- Allow to cool down, then clear latch via Web UI
- Consider lowering CPU frequency or improving ventilation

### No Audio / Low Volume
1. Check signal level in Web UI
2. Enable AGC (auto-adjusts gain)
3. Increase gain (try 5.0–10.0x)
4. Disable high-pass filter temporarily to test

### Audio Clipping / Distortion
1. Decrease gain
2. Enable AGC (fast attack prevents clipping)
3. Check signal level — aim for 30–70%

### First Connection Fails
Some RTSP clients (VLC) probe the server on first connect. The second connection works immediately.

### Stream Drops / Connection Issues
1. Check WiFi signal strength (RSSI > -70 dBm)
2. Increase buffer size to 2048 or 4096
3. Enable auto recovery
4. Reduce WiFi TX power if causing interference

## Version History

### v2.5.0 (Configurable Hostname)
- Configurable device hostname via Web UI (default `atomecho-<mac6>`, per-device unique)
  - Drives mDNS `.local` name, DHCP hostname, and browser tab title
  - RFC 1123 validation with auto-normalization (lowercase, alphanumeric + hyphens)
  - Persisted in `"audio"` NVS namespace; survives Reset Wi-Fi, cleared by Factory Reset
  - Reboot-on-save to reinitialize mDNS/DHCP cleanly
- Setup AP SSID now uses per-device suffix: `ESP32-RTSP-Mic-<MAC6>` (e.g. `ESP32-RTSP-Mic-AB12CD`)
  - Fixed prefix `ESP32-RTSP-Mic-` for discoverability
  - Last 6 hex digits of MAC disambiguate multiple devices in setup mode
- New Device card in Web UI with hostname input, live preview, and RFC 1123 help text
- Captive portal notice: after saving WiFi credentials in the setup portal, users see a prominent warning on the save-success page ("After saving WiFi, this portal closes") with the device's future URL
- Web UI script extracted from inline C++ string to SPIFFS file (`data/gui.js`) served via `web.streamFile()` with backpressure — fixes HTTP response truncation bug on ESP32 Arduino WebServer where `web.send()` silently drops data when lwIP memory pool is exhausted
- DHCP hostname fix: after WiFiManager connects, hostname is set directly on the STA `esp_netif` via `esp_netif_set_hostname()` followed by a DHCP renew — bypasses the Arduino layer which doesn't survive re-init, and fixes stale `esp32-<chipid>` entries in router DHCP tables
- Platform build now requires `board_build.filesystem = spiffs` and `pio run -t uploadfs`

### v2.4.0
- Optional WireGuard tunnel (client-only) with web UI configuration and status
- Reset Wi-Fi action — clears only WiFi credentials, preserves WireGuard and audio config (renamed from Ship-Ready Reset; same behavior)
- Factory Reset now clears both `"audio"` and `"wg"` namespaces (previously preserved WireGuard config); effectively a full reset except for WiFi
- Both Factory Reset and Reset Wi-Fi prompt with a confirmation dialog describing their exact scope before executing
- RTSP URL card with Copy buttons (LAN URL always shown, WireGuard URL shown when tunnel is up)
- Async DNS resolution for endpoint hostname (does not block the web UI)
- Vendored `ciniml/WireGuard-ESP32-Arduino` library with PersistentKeepalive support and rx/tx byte counters
- All WireGuard settings stored in a dedicated `"wg"` NVS namespace (isolated from `"audio"`)
- Platform pinned to `espressif32 @ 6.11.0`
- Private key never logged in full or returned by the API in full

### v2.3.0
- Socket ownership model — Core 1 exclusively owns WiFiClient during streaming
- Confirmed task exit via FreeRTOS semaphore (prevents double-task creation)
- In-stream RTSP processing (TEARDOWN + keepalive on Core 1)
- LED ownership guards, log buffer spinlock, memory barriers
- Proactive WiFi disconnect handling
- Default CPU frequency lowered to 160MHz
- Copy logs button in Web UI

### v2.2.0
- RTSP receive buffer drain on disconnect
- Large RTSP session timeout (86400s)
- Write failure tolerance (100 consecutive failures before disconnect)
- Auto-recovery disabled by default (false positive prevention)
- NTP time sync (EST timestamps)
- Configurable LED mode (Off / Static / Level)
- Blue = ready, green = streaming LED colors
- Disconnect diagnostics (session duration, dropped packets, RSSI)

### v2.1.0
- Lock-free pointer handoff (removed mutex)
- Fixed first-connection race condition (VLC probe)
- Fixed `i2s_read` blocking, Core 1 heap contention
- mDNS discovery (`atomecho.local`)
- Automatic Gain Control (AGC)
- LED audio level indicator
- RTSP idle timeout (60s)
- Periodic heap monitoring
- Removed OTA (serial flash only)
- Default gain 3.0x, HPF cutoff 300Hz

### v2.0.0
- Dual-core architecture

### v1.0.0
- Initial release

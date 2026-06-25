# M5Stack Atom Echo — RTSP Microphone for BirdNET-Go

**Firmware v2.5.0** — A high-quality RTSP audio streaming server for the **M5Stack Atom Echo**, streaming live audio to [BirdNET-Go](https://github.com/tphakala/birdnet-go) or any RTSP-compatible client.

<p align="left">
  <img src="https://shop.m5stack.com/cdn/shop/files/3_e4ea519e-765f-4f30-aad1-7855ff9f8744_1200x1200.jpg" alt="M5Stack Atom Echo" width="300">
</p>

**Buy**: [M5Stack Store](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit) | [Amazon](https://www.amazon.com/M5Stack-Atom-Echo-Smart-Speaker/dp/B0C7QSVPB2)

## Features

- **Dual-core architecture** — Core 1 handles full audio pipeline, Core 0 handles Web UI and RTSP negotiation
- **mDNS discovery** — `<hostname>.local` (default `atomecho-<mac6>`, unique per device), no IP needed
- **Web UI** — configure settings, view signal levels, logs, and diagnostics
- **AGC** — automatic gain control for varying bird distances
- **High-pass filter** — 2nd-order Butterworth (default 300Hz) removes wind/traffic
- **Thermal protection** — configurable auto-shutdown on overheating
- **LED indicator** — Off / Static / Level modes
- **WiFiManager** — captive portal for initial WiFi setup
- **Persistent settings** — saved to flash

## Quick Start

### 1. Flash
```bash
pio run --target upload && pio run --target uploadfs
```

> [!IMPORTANT]
> The Web UI script is served from a SPIFFS partition. Both `upload` and `uploadfs` must run for the Web UI to function. The two commands can run in either order; `uploadfs` is required at least once after a fresh flash or if SPIFFS data has been erased (e.g. via `pio run -t erase` or `esptool.py erase_flash`).

### 2. Connect to WiFi
On first boot, connect to the `ESP32-RTSP-Mic-<MAC-suffix>` access point (each device shows a unique suffix based on its MAC address) and configure your WiFi. The LED turns **blue** when ready.

### 3. Stream

Replace `<hostname>` with your device's hostname (the default is `atomecho-<mac6>`, where `<mac6>` is the last six hex digits of the device's MAC address — e.g. `atomecho-55d990`).

```bash
vlc rtsp://<hostname>.local:8554/
# or
ffplay -rtsp_transport tcp rtsp://<hostname>.local:8554/
```

**BirdNET-Go**: set audio source to `rtsp://<hostname>.local:8554/`

**Web UI**: `http://<hostname>.local/`

You can change the hostname via the Web UI — see [Configurable Hostname](#configurable-hostname) below.

### Configurable Hostname

The hostname is configurable through the Web UI (**Device** card → **Hostname** field). The default is `atomecho-<mac6>` (the last six hex digits of the device's MAC address), so every unit is unique on the network out of the box, but you can set it to any RFC 1123 compliant hostname (lowercase letters, digits, hyphens only; max 63 characters). A clean name like `kitchen-echo` or `garden-echo` is a common choice.

Changing the hostname reboots the device so the new name takes effect for mDNS and DHCP. The hostname:
- Survives **Reset Wi-Fi** (preserved during provisioning)
- Resets to the per-device default `atomecho-<mac6>` on **Factory Reset**

## Optional: WireGuard Tunnel

The firmware includes an **optional, client-only WireGuard tunnel**. When enabled, the Atom Echo establishes an outbound WireGuard connection to your WireGuard server, giving it a routable tunnel IP. This lets a device on a **different network** from BirdNet-Go stream audio over the tunnel with **no port forwarding** required on the remote network.

BirdNet-Go needs **no code change** — once the tunnel is up, the device's tunnel IP behaves like a normal routable address, and BirdNet-Go simply connects to `rtsp://<tunnel-ip>:8554/`.

**Why it's useful:** A device can be shipped to a remote location and handed to a non-technical end user. The end user only joins their home WiFi via the captive portal; the tunnel comes up automatically and BirdNet-Go reconnects on its own.

### Configuration via Web UI

Open the web UI and find the **WireGuard** card. Fields:

- **Enable** — ON/OFF toggle for the tunnel.
- **Private Key** — your WireGuard private key (base64). Rendered as a password field.
- **Server Public Key** — the public key of your WireGuard server (base64).
- **Server Endpoint** — your WireGuard server in `host:port` format.
- **Tunnel IP** — the device's tunnel address in CIDR notation (e.g. `10.6.0.5/24`). Must match the peer entry on your WireGuard server.
- **Keepalive** — persistent keepalive interval in seconds (default **25**). Keeps NAT mappings open so BirdNet-Go can reach the device even when it is idle.

The private key is **never echoed back** in API responses or logs (it is masked as `********`).

### Importing a WireGuard `.conf` File

Instead of entering each field manually, you can import a standard `wg-quick(8)` format `.conf` file. Click the **Import Config** button in the WireGuard card, select your `.conf` file, and review the preview panel (the private key is masked). Click **Apply** to populate all fields in a single step.

The import validates:
- Exactly one `[Peer]` section (multi-peer configs are rejected)
- Required fields: `PrivateKey`, `Address` (IPv4 only), `PublicKey`, `Endpoint`
- Key format: 44-character base64 with trailing `=`

If a `PresharedKey` is present, a warning is shown but import proceeds (PSK is not supported and is ignored).

### Key Generation

Generate a key pair on any computer with the `wg` tools:

```bash
wg genkey | tee private.key | wg pubkey > public.key
```

- Put the **private key** into the Atom Echo's web UI.
- Add the **public key** as a new peer in your WireGuard server's config, assigning the same tunnel IP you entered in the web UI.

### RTSP URLs

The web UI's **RTSP URLs** card shows two addresses, each rendered as a clickable hyperlink (opens in VLC when clicked) with a **Copy** button:

- **LAN URL** — `rtsp://<lan-ip>:8554/` (useful on the same network).
- **WireGuard URL** — `rtsp://<tunnel-ip>:8554/` (shown only when the tunnel is up). Paste this into BirdNet-Go as the source URL for a remote device.

### Diagnostics

The **WireGuard Status** card shows live tunnel state, last handshake time, and bytes received/transmitted. If the tunnel drops, the firmware retries automatically in the background — BirdNet-Go reconnects once it is restored.

### Reset Controls

| Button | Audio settings | Wi-Fi credentials | WireGuard config | Confirm dialog |
|---|---|---|---|---|
| `Reboot` | preserve | preserve | preserve | none |
| `Reset I2S` | preserve | preserve | preserve | none |
| `Factory Reset` | wipe | preserve | wipe | yes |
| `Reset Wi-Fi` | preserve | wipe | preserve | yes |

**Reset Wi-Fi** clears **only** the saved WiFi credentials, then reboots. It preserves the WireGuard configuration and all audio settings. Use this before mailing a device to an end user: on next boot the captive portal appears, they join their home WiFi, and the tunnel comes up automatically with no further action.

**Factory Reset** wipes both audio settings and WireGuard configuration, leaving only the Wi-Fi connection intact. Use this for a full clean slate without having to rejoin WiFi.

### Remote Web UI over the Tunnel

Once the tunnel is up, the Atom Echo's web UI is reachable at `http://<tunnel-ip>/` from any peer on the same WireGuard network (including the WireGuard server itself). No extra firmware feature is needed — the web server already listens on all interfaces.

### Privacy / Security

- The private key is stored in plaintext NVS, in a dedicated `"wg"` namespace. It is never sent in full over the API and never written to logs.
- `Factory Reset` clears both the `"audio"` and `"wg"` namespaces, leaving only WiFi intact. `Reset Wi-Fi` clears only WiFi credentials, preserving audio and WireGuard config.

## Recommended Settings

| Setting | Default | Notes |
|---------|---------|-------|
| Sample Rate | 16.0 kHz (16000 Hz) | Optimal for PDM on Atom Echo; Web UI displays and accepts values in kHz (one decimal) |
| Gain | 3.0x | Good for outdoor use |
| AGC | OFF | Enable for varying bird distances |
| High-Pass | ON, 300 Hz | Removes rumble, keeps bird calls |
| Buffer | 1024 samples | 64ms latency, stable streaming |
| CPU | 160 MHz | Sufficient, reduces heat |
| I2S Shift | 0 bits | Fixed for PDM — do not change |

## LED Status

| Color | Meaning |
|-------|---------|
| Yellow | Starting up |
| Blue | Ready, waiting for connection |
| Green | Streaming |
| Orange | Signal hot, >70% (level mode) |
| Red | Clipping or thermal protection |

## Building

```bash
pio run                         # Build
pio run --target upload         # Flash firmware
pio run --target uploadfs       # Flash SPIFFS (required for Web UI)
pio device monitor -b 115200    # Serial monitor
```

> [!IMPORTANT]
> The Web UI script is served from SPIFFS (`data/gui.js`). After flashing firmware, you must also run `pio run --target uploadfs`. Without it the Web UI loads a blank page. Both commands can run in either order; `uploadfs` is required at least once after a fresh flash or any filesystem erase.

### Dependencies
```ini
lib_deps =
    tzapu/WiFiManager @ ^2.0.17
    m5stack/M5Atom @ ^0.1.3
    fastled/FastLED @ ^3.10.3
```

The Web UI JavaScript is stored in SPIFFS (`data/gui.js`) and served by the firmware. The build requires `board_build.filesystem = spiffs` in `platformio.ini`.

The WireGuard tunnel uses a vendored copy of
[WireGuard-ESP32-Arduino](https://github.com/ciniml/WireGuard-ESP32-Arduino) v0.1.5
(under `lib/WireGuard-ESP32/`), which bundles the `wireguard-lwip` dependency. No
additional PlatformIO `lib_deps` entry is required.

The build is pinned to `platform = espressif32 @ 6.11.0` (Arduino core 2.0.17, ESP-IDF
4.4.x).

## Documentation

- [Architecture & Troubleshooting Guide](docs/DETAILS.md) — dual-core design, audio tuning, troubleshooting, version history

## Acknowledgments

This project is largely based on [birdnetgo-esp32-rtsp-mic](https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic) by [@Sukecz](https://github.com/Sukecz) — thank you for the excellent foundation!

- M5Stack for the Atom Echo hardware
- [BirdNET-Go](https://github.com/tphakala/birdnet-go) community
- [WireGuard-ESP32-Arduino](https://github.com/ciniml/WireGuard-ESP32-Arduino) by Kenta Ida and Daniel Hope (BSD-3-Clause) — vendored WireGuard implementation for ESP32

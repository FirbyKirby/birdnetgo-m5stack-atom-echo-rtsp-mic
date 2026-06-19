#include "WireGuardManager.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WireGuard-ESP32.h>

extern "C" {
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
}

extern void simplePrintln(String message);

static Preferences wgPrefs;

static bool s_enabled = false;
static String s_privateKey = "";
static String s_serverPublicKey = "";
static String s_endpointHost = "";
static uint16_t s_endpointPort = 51820;
static String s_tunnelAddress = "";
static uint16_t s_keepalive = 25;

static WireGuard s_wg;
static WgState s_state = WG_DISABLED;
static IPAddress s_tunnelIP;
static IPAddress s_tunnelMask;
static unsigned long s_lastAttemptMs = 0;
static uint32_t s_backoffMs = 5000;
static bool s_wasUp = false;

// Async DNS resolution state.
// The DNS task runs on Core 0 but independently from the loop,
// so the web UI stays responsive during hostname resolution.
static TaskHandle_t s_dnsTaskHandle = NULL;
static String s_resolvedEndpointIP = "";
static volatile bool s_dnsDone = false;
static volatile bool s_dnsFailed = false;
static volatile uint8_t s_dnsGeneration = 0;

static void _dnsResolveTask(void* param);
static void _startTunnelWithIP(const String& resolvedIP);
static void _cancelDNS(const char* reason);

static bool parseTunnelAddress(const String& cidr, IPAddress& ip, IPAddress& mask) {
    int slash = cidr.indexOf('/');
    if (slash < 0) return false;
    String ipStr = cidr.substring(0, slash);
    int prefix = cidr.substring(slash + 1).toInt();
    if (prefix < 1 || prefix > 32) return false;
    if (!ip.fromString(ipStr)) return false;
    uint32_t m = (prefix == 32) ? 0xFFFFFFFF : (~((1UL << (32 - prefix)) - 1));
    mask = IPAddress(m >> 24, (m >> 16) & 0xFF, (m >> 8) & 0xFF, m & 0xFF);
    return true;
}

static void _dnsResolveTask(void* param) {
    uint8_t myGen = (uint8_t)(uintptr_t)param;

    // Backoff schedule: 5 attempts. Delays between attempts:
    //   0ms, 500ms, 500ms, 1s, 2s  →  total ≤ ~4s of wait + DNS timeout per attempt
    // DNS itself can take a few seconds internally; we do not block the main loop
    // while this task is running.
    const uint32_t delays[] = {0, 500, 500, 1000, 2000};
    bool success = false;

    for (int retry = 0; retry < 5 && !success; retry++) {
        if (myGen != s_dnsGeneration) break;          // endpoint/config changed
        if (s_dnsTaskHandle == NULL) break;           // explicitly cancelled
        if (delays[retry] > 0) {
            vTaskDelay(pdMS_TO_TICKS(delays[retry]));
        }
        if (myGen != s_dnsGeneration) break;
        if (s_dnsTaskHandle == NULL) break;

        struct addrinfo hint;
        struct addrinfo *res = NULL;
        memset(&hint, 0, sizeof(hint));
        ip_addr_t ep_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);

        if (lwip_getaddrinfo(s_endpointHost.c_str(), NULL, &hint, &res) == 0) {
            if (res && res->ai_addr) {
                struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
                inet_addr_to_ip4addr(ip_2_ip4(&ep_ip), &addr4);
                IPAddress ip(ep_ip.u_addr.ip4.addr);
                s_resolvedEndpointIP = ip.toString();
                lwip_freeaddrinfo(res);
                success = true;
            }
        }
    }

    if (myGen == s_dnsGeneration && s_dnsTaskHandle != NULL) {
        s_dnsDone = true;
        s_dnsFailed = !success;
        __asm__ __volatile__("memw" ::: "memory");
    }
    s_dnsTaskHandle = NULL;
    vTaskDelete(NULL);
}

static void _startTunnelWithIP(const String& resolvedIP) {
    if (!parseTunnelAddress(s_tunnelAddress, s_tunnelIP, s_tunnelMask)) {
        simplePrintln("WG: invalid tunnel address: " + s_tunnelAddress);
        return;
    }

    IPAddress gateway(0, 0, 0, 0);
    simplePrintln("WG: starting tunnel to " + s_endpointHost +
                  " (" + resolvedIP + "):" + String(s_endpointPort));
    simplePrintln("WG: tunnel IP " + s_tunnelIP.toString());

    bool ok = s_wg.begin(s_tunnelIP, s_tunnelMask, gateway,
                         s_privateKey.c_str(),
                         resolvedIP.c_str(),
                         s_serverPublicKey.c_str(),
                         s_endpointPort,
                         s_keepalive);
    if (ok) {
        simplePrintln("WG: tunnel interface created, connecting...");
        s_lastAttemptMs = millis();
    } else {
        simplePrintln("WG: failed to create tunnel interface");
        s_lastAttemptMs = millis();
        if (s_backoffMs < 30000) s_backoffMs += 5000;
    }
}

static void _cancelDNS(const char* reason) {
    if (s_dnsTaskHandle != NULL) {
        s_dnsGeneration++;
        __asm__ __volatile__("memw" ::: "memory");
        TaskHandle_t h = s_dnsTaskHandle;
        s_dnsTaskHandle = NULL;
        s_dnsDone = false;
        s_dnsFailed = false;
        s_resolvedEndpointIP = "";
        vTaskDelete(h);
        simplePrintln(String("WG: ") + reason + " — DNS cancelled");
    } else {
        s_dnsDone = false;
        s_dnsFailed = false;
        s_resolvedEndpointIP = "";
    }
}

void wg_load() {
    wgPrefs.begin("wg", false);
    s_enabled = wgPrefs.getBool("en", false);
    s_privateKey = wgPrefs.getString("priv", "");
    s_serverPublicKey = wgPrefs.getString("srvpub", "");
    s_endpointHost = wgPrefs.getString("endhost", "");
    s_endpointPort = wgPrefs.getUShort("endport", 51820);
    s_tunnelAddress = wgPrefs.getString("tunaddr", "");
    s_keepalive = wgPrefs.getUShort("keepalive", 25);
    wgPrefs.end();

    if (s_enabled) {
        s_state = WG_WAIT_TIME;
        simplePrintln("WG: config loaded (enabled)");
    } else {
        s_state = WG_DISABLED;
        simplePrintln("WG: config loaded (disabled)");
    }
}

void wg_save() {
    wgPrefs.begin("wg", false);
    wgPrefs.putBool("en", s_enabled);
    wgPrefs.putString("priv", s_privateKey);
    wgPrefs.putString("srvpub", s_serverPublicKey);
    wgPrefs.putString("endhost", s_endpointHost);
    wgPrefs.putUShort("endport", s_endpointPort);
    wgPrefs.putString("tunaddr", s_tunnelAddress);
    wgPrefs.putUShort("keepalive", s_keepalive);
    wgPrefs.end();
}

void wg_begin() {
    // Preconditions
    if (s_wg.is_initialized()) return;
    if (s_privateKey.length() == 0 || s_serverPublicKey.length() == 0) return;
    if (s_endpointHost.length() == 0) return;
    if (s_tunnelAddress.length() == 0) return;

    // If a DNS task is already running, nothing to do yet
    if (s_dnsTaskHandle != NULL) return;

    // If DNS just finished with fresh results for a successful resolve, start tunnel
    if (s_dnsDone) {
        bool failed = s_dnsFailed;
        String ip = s_resolvedEndpointIP;
        s_dnsDone = false;
        s_dnsFailed = false;
        s_resolvedEndpointIP = "";
        if (failed || ip.length() == 0) {
            s_lastAttemptMs = millis();
            if (s_backoffMs < 30000) s_backoffMs += 5000;
            simplePrintln("WG: endpoint resolution failed, backing off");
            return;
        }
        _startTunnelWithIP(ip);
        return;
    }

    // Otherwise, start async DNS resolution
    simplePrintln("WG: resolving endpoint " + s_endpointHost + " ...");
    s_dnsGeneration++;
    __asm__ __volatile__("memw" ::: "memory");
    xTaskCreatePinnedToCore(_dnsResolveTask, "wg_dns", 4096,
                            (void *)(uintptr_t)s_dnsGeneration,
                            1, &s_dnsTaskHandle, 0);
    if (s_dnsTaskHandle == NULL) {
        simplePrintln("WG: failed to create DNS task");
        s_lastAttemptMs = millis();
    }
    s_state = WG_CONNECTING;
}

void wg_stop() {
    _cancelDNS("stop");
    if (s_wg.is_initialized()) {
        s_wg.end();
        simplePrintln("WG: tunnel stopped");
    }
    s_state = s_enabled ? WG_WAIT_TIME : WG_DISABLED;
    s_wasUp = false;
}

void wg_tick() {
    if (!s_enabled) {
        if (s_state != WG_DISABLED) {
            if (s_dnsTaskHandle != NULL) _cancelDNS("disable");
            if (s_wg.is_initialized()) wg_stop();
        }
        s_state = WG_DISABLED;
        return;
    }

    time_t now;
    time(&now);
    if (now < 100000) {
        if (s_state == WG_WAIT_TIME) return;
        s_state = WG_WAIT_TIME;
        if (s_dnsTaskHandle != NULL) _cancelDNS("waiting_time");
        if (s_wg.is_initialized()) wg_stop();
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (s_dnsTaskHandle != NULL) _cancelDNS("waiting_wifi");
        if (s_wg.is_initialized()) wg_stop();
        s_state = WG_WAIT_WIFI;
        return;
    }

    if (!s_wg.is_initialized()) {
        if (s_dnsDone && !s_dnsFailed) {
            // Resolve just completed with success — pick it up immediately (don't wait for backoff)
            wg_begin();
            return;
        }
        if (s_dnsTaskHandle != NULL) {
            s_state = WG_CONNECTING;
            return;
        }
        if (millis() - s_lastAttemptMs < s_backoffMs) return;
        wg_begin();
        return;
    }

    // Tunnel interface is up — monitor peer state
    bool peerUp = s_wg.isPeerUp();
    if (peerUp) {
        if (!s_wasUp) {
            s_wasUp = true;
            s_backoffMs = 5000;
            simplePrintln("WG: tunnel UP");
        }
        s_state = WG_UP;
    } else {
        if (s_wasUp) {
            s_wasUp = false;
            simplePrintln("WG: tunnel DOWN, will retry");
        }
        s_state = WG_CONNECTING;
        if (millis() - s_lastAttemptMs > 30000) {
            simplePrintln("WG: reconnecting...");
            wg_stop();
            s_lastAttemptMs = millis();
            if (s_backoffMs < 30000) s_backoffMs += 5000;
        }
    }
}

bool wg_isEnabled() { return s_enabled; }
WgState wg_state() { return s_state; }

const char* wg_stateStr() {
    switch (s_state) {
        case WG_DISABLED: return "disabled";
        case WG_WAIT_TIME: return "waiting_time";
        case WG_WAIT_WIFI: return "waiting_wifi";
        case WG_CONNECTING: return "connecting";
        case WG_UP: return "up";
        default: return "unknown";
    }
}

String wg_tunnelIp() {
    if (s_tunnelAddress.length() == 0) return "";
    int slash = s_tunnelAddress.indexOf('/');
    if (slash < 0) return "";
    return s_tunnelAddress.substring(0, slash);
}

String wg_tunnelAddress() {
    return s_tunnelAddress;
}

uint32_t wg_lastHandshakeMs() { return s_wg.lastHandshakeMs(); }
uint64_t wg_rxBytes() { return s_wg.rxBytes(); }
uint64_t wg_txBytes() { return s_wg.txBytes(); }
String wg_endpointStr() { return s_endpointHost + ":" + String(s_endpointPort); }
uint16_t wg_keepalive() { return s_keepalive; }
String wg_privateKey() { return s_privateKey; }
String wg_serverPublicKey() { return s_serverPublicKey; }

void wg_setEnabled(bool en) {
    s_enabled = en;
    wg_save();
    if (!en) {
        if (s_dnsTaskHandle != NULL) _cancelDNS("enabled toggle off");
        wg_stop();
    } else {
        _cancelDNS("enabled toggle on");
        s_state = WG_WAIT_TIME;
        s_lastAttemptMs = 0;
    }
    simplePrintln(String("WG: ") + (en ? "enabled" : "disabled"));
}

void wg_setPrivateKey(const String& key) {
    s_privateKey = key;
    wg_save();
    _cancelDNS("private key changed");
    if (s_wg.is_initialized()) wg_stop();
}

void wg_setServerPublicKey(const String& key) {
    s_serverPublicKey = key;
    wg_save();
    _cancelDNS("server public key changed");
    if (s_wg.is_initialized()) wg_stop();
}

void wg_setEndpoint(const String& host, uint16_t port) {
    s_endpointHost = host;
    s_endpointPort = port;
    wg_save();
    _cancelDNS("endpoint changed");
    if (s_wg.is_initialized()) wg_stop();
}

void wg_setTunnelAddress(const String& addr) {
    s_tunnelAddress = addr;
    wg_save();
    _cancelDNS("tunnel address changed");
    if (s_wg.is_initialized()) wg_stop();
}

void wg_setKeepalive(uint16_t seconds) {
    s_keepalive = seconds;
    wg_save();
    _cancelDNS("keepalive changed");
    if (s_wg.is_initialized()) wg_stop();
}

void wg_clear() {
    if (s_dnsTaskHandle != NULL) _cancelDNS("clearing WG config");
    if (s_wg.is_initialized()) {
        s_wg.end();
        simplePrintln("WG: tunnel stopped");
    }

    s_enabled = false;
    s_privateKey = "";
    s_serverPublicKey = "";
    s_endpointHost = "";
    s_endpointPort = 51820;
    s_tunnelAddress = "";
    s_keepalive = 25;
    s_state = WG_DISABLED;
    s_wasUp = false;
    s_lastAttemptMs = 0;
    s_backoffMs = 5000;

    wgPrefs.begin("wg", false);
    wgPrefs.clear();
    wgPrefs.end();

    simplePrintln("WG: config cleared");
}

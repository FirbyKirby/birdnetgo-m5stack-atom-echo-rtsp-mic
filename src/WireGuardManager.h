#pragma once
#include <Arduino.h>
#include <IPAddress.h>

enum WgState {
    WG_DISABLED,
    WG_WAIT_TIME,
    WG_WAIT_WIFI,
    WG_CONNECTING,
    WG_UP
};

void wg_load();
void wg_save();
void wg_begin();
void wg_stop();
void wg_tick();

bool wg_isEnabled();
WgState wg_state();
const char* wg_stateStr();
String wg_tunnelIp();
String wg_tunnelAddress();
uint32_t wg_lastHandshakeMs();
uint64_t wg_rxBytes();
uint64_t wg_txBytes();
String wg_endpointStr();
uint16_t wg_keepalive();
String wg_privateKey();
String wg_serverPublicKey();

void wg_setEnabled(bool en);
void wg_setPrivateKey(const String& key);
void wg_setServerPublicKey(const String& key);
void wg_setEndpoint(const String& host, uint16_t port);
void wg_setTunnelAddress(const String& addr);
void wg_setKeepalive(uint16_t seconds);

// Clear all WireGuard config — stops the tunnel, cancels any in-flight DNS,
// erases the "wg" NVS namespace, and resets in-memory state to disabled defaults.
// Called from resetToDefaultSettings() so the "Defaults" action also wipes WG.
void wg_clear();

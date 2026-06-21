#include "WireGuard-ESP32.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"

#include "esp32-hal-log.h"

extern "C" {
#include "wireguardif.h"
#include "wireguard-platform.h"
#include "wireguard.h"
}

static struct netif wg_netif_struct = {0};
static struct netif *wg_netif = NULL;
static struct netif *previous_default_netif = NULL;
static uint8_t s_peer_index = WIREGUARDIF_INVALID_INDEX;

#define TAG "[WireGuard] "

bool WireGuard::begin(const IPAddress& localIP, const IPAddress& Subnet, const IPAddress& Gateway, const char* privateKey, const char* remotePeerAddress, const char* remotePeerPublicKey, uint16_t remotePeerPort, uint16_t persistentKeepalive) {
	struct wireguardif_init_data wg;
	struct wireguardif_peer peer;
	ip_addr_t ipaddr = IPADDR4_INIT(static_cast<uint32_t>(localIP));
	ip_addr_t netmask = IPADDR4_INIT(static_cast<uint32_t>(Subnet));
	ip_addr_t gateway = IPADDR4_INIT(static_cast<uint32_t>(Gateway));

	assert(privateKey != NULL);
	assert(remotePeerAddress != NULL);
	assert(remotePeerPublicKey != NULL);
	assert(remotePeerPort != 0);

	wg.private_key = privateKey;
    wg.listen_port = remotePeerPort;
	
	wg.bind_netif = NULL;

	wireguardif_peer_init(&peer);
	bool success_get_endpoint_ip = false;
    for(int retry = 0; retry < 5; retry++) {
        ip_addr_t endpoint_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
        struct addrinfo *res = NULL;
        struct addrinfo hint;
        memset(&hint, 0, sizeof(hint));
        memset(&endpoint_ip, 0, sizeof(endpoint_ip));
        if( lwip_getaddrinfo(remotePeerAddress, NULL, &hint, &res) != 0 ) {
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}
		success_get_endpoint_ip = true;
        struct in_addr addr4 = ((struct sockaddr_in *) (res->ai_addr))->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&endpoint_ip), &addr4);
        lwip_freeaddrinfo(res);

        peer.endpoint_ip = endpoint_ip;
        log_i(TAG "%s is %3d.%3d.%3d.%3d"
			, remotePeerAddress
            , (endpoint_ip.u_addr.ip4.addr >>  0) & 0xff
            , (endpoint_ip.u_addr.ip4.addr >>  8) & 0xff
            , (endpoint_ip.u_addr.ip4.addr >> 16) & 0xff
            , (endpoint_ip.u_addr.ip4.addr >> 24) & 0xff
            );
		break;
    }
	if( !success_get_endpoint_ip  ) {
		log_e(TAG "failed to get endpoint ip.");
		return false;
	}
	wg_netif = netif_add(&wg_netif_struct, ip_2_ip4(&ipaddr), ip_2_ip4(&netmask), ip_2_ip4(&gateway), &wg, &wireguardif_init, &ip_input);
	if( wg_netif == nullptr ) {
		log_e(TAG "failed to initialize WG netif.");
		return false;
	}
	netif_set_up(wg_netif);

	peer.public_key = remotePeerPublicKey;
	peer.preshared_key = NULL;
    {
        ip_addr_t allowed_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
        peer.allowed_ip = allowed_ip;
        ip_addr_t allowed_mask = IPADDR4_INIT_BYTES(0, 0, 0, 0);
        peer.allowed_mask = allowed_mask;
    }
	
	peer.endport_port = remotePeerPort;
	peer.keep_alive = persistentKeepalive;

    wireguard_platform_init();
	wireguardif_add_peer(wg_netif, &peer, &s_peer_index);
	if ((s_peer_index != WIREGUARDIF_INVALID_INDEX) && !ip_addr_isany(&peer.endpoint_ip)) {
        log_i(TAG "connecting wireguard...");
		wireguardif_connect(wg_netif, s_peer_index);
		previous_default_netif = netif_default;
        netif_set_default(wg_netif);
	}

	this->_is_initialized = true;
	return true;
}

bool WireGuard::begin(const IPAddress& localIP, const char* privateKey, const char* remotePeerAddress, const char* remotePeerPublicKey, uint16_t remotePeerPort, uint16_t persistentKeepalive) {
	auto subnet = IPAddress(255,255,255,255);
	auto gateway = IPAddress(0,0,0,0);
	return WireGuard::begin(localIP, subnet, gateway, privateKey, remotePeerAddress, remotePeerPublicKey, remotePeerPort, persistentKeepalive);
}

void WireGuard::end() {
	if( !this->_is_initialized ) return;

	netif_set_default(previous_default_netif);
	previous_default_netif = nullptr;
	wireguardif_disconnect(wg_netif, s_peer_index);
	wireguardif_remove_peer(wg_netif, s_peer_index);
	s_peer_index = WIREGUARDIF_INVALID_INDEX;
	wireguardif_shutdown(wg_netif);
	netif_remove(wg_netif);
	wg_netif = nullptr;

	this->_is_initialized = false;
}

bool WireGuard::isPeerUp() const {
	if (!this->_is_initialized || wg_netif == NULL || s_peer_index == WIREGUARDIF_INVALID_INDEX)
		return false;
	return wireguardif_peer_is_up(wg_netif, s_peer_index, NULL, NULL) == ERR_OK;
}

uint32_t WireGuard::lastHandshakeMs() const {
	if (!this->_is_initialized || wg_netif == NULL || s_peer_index == WIREGUARDIF_INVALID_INDEX)
		return 0;
	struct wireguard_device *device = (struct wireguard_device *)wg_netif->state;
	if (!device || !device->valid) return 0;
	struct wireguard_peer *p = peer_lookup_by_peer_index(device, s_peer_index);
	if (!p) return 0;
	if (p->curr_keypair.valid) return p->curr_keypair.keypair_millis;
	if (p->prev_keypair.valid) return p->prev_keypair.keypair_millis;
	return 0;
}

uint64_t WireGuard::rxBytes() const {
	if (!this->_is_initialized || wg_netif == NULL || s_peer_index == WIREGUARDIF_INVALID_INDEX)
		return 0;
	struct wireguard_device *device = (struct wireguard_device *)wg_netif->state;
	if (!device || !device->valid) return 0;
	struct wireguard_peer *p = peer_lookup_by_peer_index(device, s_peer_index);
	return p ? p->rx_bytes : 0;
}

uint64_t WireGuard::txBytes() const {
	if (!this->_is_initialized || wg_netif == NULL || s_peer_index == WIREGUARDIF_INVALID_INDEX)
		return 0;
	struct wireguard_device *device = (struct wireguard_device *)wg_netif->state;
	if (!device || !device->valid) return 0;
	struct wireguard_peer *p = peer_lookup_by_peer_index(device, s_peer_index);
	return p ? p->tx_bytes : 0;
}

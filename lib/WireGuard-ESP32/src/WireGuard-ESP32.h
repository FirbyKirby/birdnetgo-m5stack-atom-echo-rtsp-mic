#pragma once
#include <IPAddress.h>
#include <stdint.h>

class WireGuard
{
private:
    bool _is_initialized = false;
public:
    bool begin(const IPAddress& localIP, const IPAddress& Subnet, const IPAddress& Gateway, const char* privateKey, const char* remotePeerAddress, const char* remotePeerPublicKey, uint16_t remotePeerPort, uint16_t persistentKeepalive = 25);
    bool begin(const IPAddress& localIP, const char* privateKey, const char* remotePeerAddress, const char* remotePeerPublicKey, uint16_t remotePeerPort, uint16_t persistentKeepalive = 25);
    void end();
    bool is_initialized() const { return this->_is_initialized; }
    bool isPeerUp() const;
    uint32_t lastHandshakeMs() const;
    uint64_t rxBytes() const;
    uint64_t txBytes() const;
};

#pragma once

#include <cstdint>
#include <string>

/* Records the last advertised mDNS state so tests can assert on it. */
namespace mock_mdns {
inline bool& beginCalled() { static bool b = false; return b; }
inline std::string& hostname() { static std::string h; return h; }
}  // namespace mock_mdns

class MDNSClass {
public:
    bool begin(const char* name) {
        mock_mdns::beginCalled() = true;
        mock_mdns::hostname() = name ? name : "";
        return true;
    }
    void addService(const char* service, const char* proto, uint16_t port) {
        (void)service;
        (void)proto;
        (void)port;
    }
    bool addServiceTxt(const char* service, const char* proto, const char* key, const char* value) {
        (void)service;
        (void)proto;
        (void)key;
        (void)value;
        return true;
    }
};

inline MDNSClass MDNS;

#pragma once

#include "WString.h"

#include <cstdint>

class IPAddress {
public:
    uint8_t operator[](int index) const {
        (void)index;
        return 0;
    }
    String toString() const { return String("192.168.4.1"); }
};

class WiFiClass {
public:
    IPAddress localIP() { return IPAddress(); }
};

inline WiFiClass WiFi;

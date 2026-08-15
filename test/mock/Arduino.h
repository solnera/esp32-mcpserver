#pragma once

#include "WString.h"

#include <cstdio>
#include <cstdint>
#include <cstdarg>

class HardwareSerial {
public:
    void printf(const char* fmt, ...) { (void)fmt; }
    void println(const char* s = "") { (void)s; }
    void print(const char* s) { (void)s; }
    void begin(unsigned long baud) { (void)baud; }
};

inline HardwareSerial Serial;

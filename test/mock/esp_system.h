#pragma once

#include <cstdint>
#include <cstdlib>

inline uint32_t esp_random() {
    return (static_cast<uint32_t>(rand()) << 16) ^ static_cast<uint32_t>(rand());
}

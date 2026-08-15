#pragma once

#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;

#ifndef pdTRUE
#define pdTRUE 1
#endif

#ifndef pdFALSE
#define pdFALSE 0
#endif

#ifndef pdPASS
#define pdPASS 1
#endif

#ifndef pdFAIL
#define pdFAIL 0
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY UINT32_MAX
#endif

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
#endif

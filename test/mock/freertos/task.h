#pragma once

#include "FreeRTOS.h"

#include <chrono>
#include <thread>

using TaskFunction_t = void (*)(void*);
using TaskHandle_t = std::thread*;

inline BaseType_t xTaskCreate(TaskFunction_t task_fn,
                              const char* name,
                              uint32_t stack_depth,
                              void* parameters,
                              uint32_t priority,
                              TaskHandle_t* created_task) {
    (void)name;
    (void)stack_depth;
    (void)priority;
    if (!task_fn || !created_task) {
        return pdFAIL;
    }

    try {
        *created_task = new std::thread([task_fn, parameters] { task_fn(parameters); });
    } catch (...) {
        *created_task = nullptr;
        return pdFAIL;
    }
    return pdPASS;
}

inline void vTaskDelay(TickType_t ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() {
    /* The mock cannot map the calling std::thread back to the std::thread*
     * that xTaskCreate returned, so self-join detection is inert in native
     * tests; it is exercised on real FreeRTOS only. */
    return nullptr;
}

inline void vTaskDelete(TaskHandle_t task) {
    if (!task) {
        return;
    }
    if (task->joinable()) {
        task->detach();
    }
    delete task;
}

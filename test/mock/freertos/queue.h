#pragma once

#include "FreeRTOS.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

struct MockQueue {
    explicit MockQueue(size_t max_items) : capacity(max_items) {}

    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable delete_cv;
    std::queue<char*> items;
    size_t capacity;
    size_t waiters = 0;
    bool deleted = false;
};

using QueueHandle_t = MockQueue*;

namespace mock_freertos_detail {
inline void queueFinishWait(MockQueue* queue) {
    if (!queue) {
        return;
    }
    if (queue->waiters > 0) {
        --queue->waiters;
    }
    if (queue->deleted && queue->waiters == 0) {
        queue->delete_cv.notify_all();
    }
}
} // namespace mock_freertos_detail

inline QueueHandle_t xQueueCreate(size_t length, size_t item_size) {
    (void)item_size;
    return new MockQueue(length);
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticks_to_wait) {
    (void)ticks_to_wait;
    if (!queue || !item) {
        return pdFALSE;
    }

    char* value = *static_cast<char* const*>(item);
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->deleted) {
        return pdFALSE;
    }
    if (queue->items.size() >= queue->capacity) {
        return pdFALSE;
    }
    queue->items.push(value);
    queue->cv.notify_one();
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* out, TickType_t ticks_to_wait) {
    if (!queue || !out) {
        return pdFALSE;
    }

    std::unique_lock<std::mutex> lock(queue->mutex);
    if (queue->deleted) {
        return pdFALSE;
    }
    if (ticks_to_wait == 0) {
        if (queue->items.empty()) {
            return pdFALSE;
        }
    } else if (ticks_to_wait == portMAX_DELAY) {
        ++queue->waiters;
        struct WaitGuard {
            MockQueue* queue;
            ~WaitGuard() {
                mock_freertos_detail::queueFinishWait(queue);
            }
        } wait_guard{queue};
        queue->cv.wait(lock, [&] { return queue->deleted || !queue->items.empty(); });
        if (queue->deleted || queue->items.empty()) {
            return pdFALSE;
        }
    } else {
        ++queue->waiters;
        struct WaitGuard {
            MockQueue* queue;
            ~WaitGuard() {
                mock_freertos_detail::queueFinishWait(queue);
            }
        } wait_guard{queue};
        bool ready = queue->cv.wait_for(lock, std::chrono::milliseconds(ticks_to_wait),
                                        [&] { return queue->deleted || !queue->items.empty(); });
        if (!ready || queue->deleted || queue->items.empty()) {
            return pdFALSE;
        }
    }

    *static_cast<char**>(out) = queue->items.front();
    queue->items.pop();
    return pdTRUE;
}

inline void vQueueDelete(QueueHandle_t queue) {
    if (!queue) {
        return;
    }
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->deleted = true;
    queue->cv.notify_all();
    queue->delete_cv.wait(lock, [&] { return queue->waiters == 0; });
    lock.unlock();
    delete queue;
}

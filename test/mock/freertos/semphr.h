#pragma once

#include "FreeRTOS.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>

struct MockSemaphore {
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable delete_cv;
    bool given = false;
    size_t waiters = 0;
    bool deleted = false;
    bool is_mutex = false;
};

using SemaphoreHandle_t = MockSemaphore*;

namespace mock_freertos_detail {
inline void semaphoreFinishWait(MockSemaphore* sem) {
    if (!sem) {
        return;
    }
    if (sem->waiters > 0) {
        --sem->waiters;
    }
    if (sem->deleted && sem->waiters == 0) {
        sem->delete_cv.notify_all();
    }
}
} // namespace mock_freertos_detail

inline SemaphoreHandle_t xSemaphoreCreateBinary() {
    return new MockSemaphore();
}

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    auto* sem = new MockSemaphore();
    sem->given = true;  // a mutex is created in the available (takeable) state
    sem->is_mutex = true;
    return sem;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    if (!sem) {
        return pdFALSE;
    }
    {
        std::lock_guard<std::mutex> lock(sem->mutex);
        if (sem->deleted) {
            return pdFALSE;
        }
        sem->given = true;
    }
    sem->cv.notify_all();
    return pdTRUE;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks_to_wait) {
    if (!sem) {
        return pdFALSE;
    }

    std::unique_lock<std::mutex> lock(sem->mutex);
    if (sem->deleted) {
        return pdFALSE;
    }
    if (ticks_to_wait == 0) {
        if (!sem->given) {
            return pdFALSE;
        }
    } else if (ticks_to_wait == portMAX_DELAY) {
        ++sem->waiters;
        struct WaitGuard {
            MockSemaphore* sem;
            ~WaitGuard() {
                mock_freertos_detail::semaphoreFinishWait(sem);
            }
        } wait_guard{sem};
        sem->cv.wait(lock, [&] { return sem->deleted || sem->given; });
        if (sem->deleted || !sem->given) {
            return pdFALSE;
        }
    } else {
        ++sem->waiters;
        struct WaitGuard {
            MockSemaphore* sem;
            ~WaitGuard() {
                mock_freertos_detail::semaphoreFinishWait(sem);
            }
        } wait_guard{sem};
        bool ready = sem->cv.wait_for(lock, std::chrono::milliseconds(ticks_to_wait),
                                      [&] { return sem->deleted || sem->given; });
        if (!ready || sem->deleted || !sem->given) {
            return pdFALSE;
        }
    }

    sem->given = false;
    return pdTRUE;
}

inline void vSemaphoreDelete(SemaphoreHandle_t sem) {
    if (!sem) {
        return;
    }
    std::unique_lock<std::mutex> lock(sem->mutex);
    if (sem->is_mutex && !sem->given) {
        /* Real FreeRTOS documents deleting a held mutex as undefined behavior.
         * The forgiving mock used to mask exactly the teardown-ordering bug the
         * production code once had, so fail loudly instead. */
        fprintf(stderr, "vSemaphoreDelete: deleting a mutex that is currently held\n");
        abort();
    }
    sem->deleted = true;
    sem->cv.notify_all();
    sem->delete_cv.wait(lock, [&] { return sem->waiters == 0; });
    lock.unlock();
    delete sem;
}

#pragma once

#include <lib/defs.h>
#include <stdatomic.h>

#include "semaphore.h"
#include "spinlock.h"

#define MUTEX_LOCKED        BIT0
#define MUTEX_WOKEN         BIT1
#define MUTEX_STARVING      BIT2
#define MUTEX_WAITER_SHIFT  3

typedef struct mutex {
    // the mutex state
    _Atomic(uint32_t) state;

    // the semaphore, used for waiting and waking
    semaphore_t sema;
} mutex_t;

void mutex_lock_slow(mutex_t* mutex);
void mutex_unlock_slow(mutex_t* mutex, uint32_t new);

static inline void mutex_lock(mutex_t* mutex) {
    uint32_t old = 0;
    if (atomic_compare_exchange_strong(&mutex->state, &old, MUTEX_LOCKED)) {
        return;
    }
    mutex_lock_slow(mutex);
}

static inline void mutex_unlock(mutex_t* mutex) {
    // Fast path, drop lock bit.
    uint32_t new = (mutex->state -= MUTEX_LOCKED);
    if (new != 0) {
        // Outlined slow path to allow inlining the fast path.
        mutex_unlock_slow(mutex, new);
    }
}

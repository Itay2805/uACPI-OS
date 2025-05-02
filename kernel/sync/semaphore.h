#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "spinlock.h"
#include "lib/list.h"

typedef struct semaphore {
    // the semaphore value
    _Atomic(uint32_t) value;

    // the number of waiters
    _Atomic(uint32_t) num_waiters;

    // the queue lock, uses irq spinlock
    // so it can be used from interrupt
    // context as well
    irq_spinlock_t lock;

    // the wait queue
    list_t wait_queue;
} semaphore_t;

void semaphore_acquire(semaphore_t* sema, bool lifo);

void semaphore_release(semaphore_t* sema, bool handoff);

#pragma once

#include "mutex.h"
#include "thread/thread.h"

typedef struct cond {
    // The ticket number of the next waiter. It is atomically
    // incremented outside the lock
    _Atomic(uint32_t) wait;

    // The ticket number of the next waiter to be notified. It can
    // be read outside the lock, but is only written to with lock held
    // Both wait and notify can wrap around, and such cases will be correctly
    // handled as long as their "unwrapped" difference is bounded by 2^31
    // For this not to be the case, we'd need to have 2^31+ threads
    // blocked on the same condvar, which is currently not possible
    _Atomic(uint32_t) notify;

    // List of parked waiters, protected by irq spinlock
    // so we can signal/broadcast from interrupt context
    irq_spinlock_t lock;
    thread_t* head;
    thread_t* tail;
} cond_t;

void cond_wait(cond_t* cond, mutex_t* mutex);
void cond_wait_irq(cond_t* cond, irq_spinlock_t* mutex, bool* irq_status);
void cond_signal(cond_t* cond);
void cond_broadcast(cond_t* cond);

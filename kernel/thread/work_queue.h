#pragma once

#include "thread.h"
#include "lib/except.h"
#include "sync/cond.h"


typedef struct work_queue_item {
    struct work_queue_item* next;
    void (*handler)(void* ctx);
    void* ctx;
} work_queue_item_t;

typedef struct work_queue {
    thread_t* worker;
    cond_t cond;
    irq_spinlock_t lock;
    _Atomic(work_queue_item_t*) head;
    work_queue_item_t* tail;
} work_queue_t;

/**
 * Initialize a new work queue
 */
err_t init_work_queue(work_queue_t* queue, const char* fmt, ...);

/**
 * Add work to the work queue, can be called from interrupt context
 */
err_t work_queue_add(work_queue_t* queue, void (*handler)(void* ctx), void* ctx);

#pragma once

#include "thread.h"
#include "lib/except.h"
#include "lib/list.h"
#include "sync/cond.h"
#include "sync/mutex.h"

typedef struct interrupt_handler {
    // link
    list_entry_t entry;

    // the handler with its context
    void (*handler)(struct interrupt_handler* handler);

    // the vector that we allocated for this handler,
    // set by the allocate code
    uint8_t vector;

    // semaphore to allow a thread to wait
    // for the interrupt
    semaphore_t semaphore;
} interrupt_handler_t;

/**
 * allocate an interrupt handler entry
 */
err_t irq_allocate(interrupt_handler_t* handler);

/**
 * Free the given irq
 */
void irq_free(interrupt_handler_t* handler);

/**
 * Dispatch the irq of the given number
 */
void irq_dispatch(uint8_t vector);

/**
 * Reserve the given irq
 */
err_t irq_reserve(uint8_t index);

/**
 * Wait for the interrupt to jump
 */
void irq_wait(interrupt_handler_t* handler);

/**
 * Wakeup a listening thread
 */
void irq_wakeup(interrupt_handler_t* handler);

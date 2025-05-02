#pragma once

#include "lib/except.h"
#include "lib/list.h"

typedef struct interrupt_handler {
    // link
    list_entry_t entry;

    // the handler with its context
    void (*handler)(struct interrupt_handler* handler);

    // the vector that we allocated for this handler,
    // set by the allocate code
    uint8_t vector;
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
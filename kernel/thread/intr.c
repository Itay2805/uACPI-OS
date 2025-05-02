#include "intr.h"

#include "scheduler.h"
#include "sync/spinlock.h"

#define IRQ_COUNT   (256 - 32)

static uint32_t m_irq_bitmap[IRQ_COUNT / 32];
static spinlock_t m_irq_bitmap_lock = INIT_SPINLOCK();

static list_t m_irq_entries[IRQ_COUNT] = {};

static int allocate_irq(void) {
    for (int w = 0; w < ARRAY_LENGTH(m_irq_bitmap); ++w) {
        if (m_irq_bitmap[w] != UINT32_MAX) {
            // Invert to find the first zero bit
            uint32_t free_mask = ~m_irq_bitmap[w];
            int bit = __builtin_ctz(free_mask);
            if (bit < 32 && w * 32 + bit < IRQ_COUNT) {
                m_irq_bitmap[w] |= (1ULL << bit);
                return w * 32 + bit;
            }
        }
    }

    return -1;
}

err_t irq_allocate(interrupt_handler_t* handler) {
    err_t err = NO_ERROR;
    spinlock_lock(&m_irq_bitmap_lock);

    int irq = allocate_irq();
    CHECK(irq >= 0);

    // initialize the list as required
    if (m_irq_entries[irq].next == NULL) {
        m_irq_entries[irq] = LIST_INIT(&m_irq_entries[irq]);
    }

    // and add it to the list
    list_add(&m_irq_entries[irq], &handler->entry);

    // set the irq
    handler->vector = irq + 0x20;

cleanup:
    spinlock_unlock(&m_irq_bitmap_lock);
    return err;
}

err_t irq_reserve(uint8_t vector) {
    err_t err = NO_ERROR;

    spinlock_lock(&m_irq_bitmap_lock);

    int w = (vector - 0x20) / 32;
    int b = (vector - 0x20) % 32;

    // ensure not already allocated
    CHECK((m_irq_bitmap[w] & (1ULL << b)) == 0);

    // mark as allocated
    m_irq_bitmap[w] |= 1ULL << b;

cleanup:
    spinlock_unlock(&m_irq_bitmap_lock);

    return err;
}

void irq_free(interrupt_handler_t* handler) {
    spinlock_lock(&m_irq_bitmap_lock);
    list_del(&handler->entry);
    int w = (handler->vector - 0x20) / 32;
    int b = (handler->vector - 0x20) % 32;
    m_irq_bitmap[w] &= ~(1ULL << b);
    spinlock_unlock(&m_irq_bitmap_lock);
}

void irq_dispatch(uint8_t vector) {
    vector -= 0x20;

    // ensure we have entries
    ASSERT(m_irq_entries[vector].next != NULL);

    // dispatch all of them
    for (list_entry_t* entry = m_irq_entries[vector].next; entry != &m_irq_entries[vector]; entry = entry->next) {
        interrupt_handler_t* irq = containerof(entry, interrupt_handler_t, entry);
        irq->handler(irq);
    }
}

void irq_wait(interrupt_handler_t* handler) {
    semaphore_acquire(&handler->semaphore, false);
}

void irq_wakeup(interrupt_handler_t* handler) {
    semaphore_release(&handler->semaphore, false);
}

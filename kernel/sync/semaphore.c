#include "semaphore.h"

#include <stdatomic.h>

#include "thread/scheduler.h"
#include "thread/thread.h"

static bool semaphore_can_acquire(semaphore_t* waiter) {
    uint32_t v = waiter->value;
    for (;;) {
        if (v == 0) {
            return false;
        }
        if (atomic_compare_exchange_strong(&waiter->value, &v, v - 1)) {
            return true;
        }
    }
}

typedef struct semaphore_unlock_ctx {
    semaphore_t* sema;
    bool irq_status;
} semaphore_unlock_ctx_t;

static bool semaphore_before_sleep(thread_t* thread, void* _ctx) {
    semaphore_unlock_ctx_t* ctx = _ctx;
    irq_spinlock_unlock(&ctx->sema->lock, ctx->irq_status);
    return true;
}

void semaphore_acquire(semaphore_t* sema, bool lifo) {
    if (semaphore_can_acquire(sema)) {
        return;
    }

    thread_t* thread = scheduler_get_current_thread();
    thread->ticket = 0;

    for (;;) {
        bool irq_status = irq_spinlock_lock(&sema->lock);

        // initialize the wait queue, if not initialized yet
        if (sema->wait_queue.next == NULL) {
            sema->wait_queue = LIST_INIT(&sema->wait_queue);
        }

        // Add ourselves to num_waiters to disable "easy case" in semaphore_release
        sema->num_waiters += 1;

        // Check can acquire to avoid missed wakeups
        if (semaphore_can_acquire(sema)) {
            sema->num_waiters -= 1;
            irq_spinlock_unlock(&sema->lock, irq_status);
            break;
        }

        // Any semaphore_release after the semaphore_acquire knows we're waiting
        // (we set num_waiters above), so go to sleep
        if (lifo) {
            list_add(&sema->wait_queue, &thread->scheduler_node);
        } else {
            list_add_tail(&sema->wait_queue, &thread->scheduler_node);
        }

        semaphore_unlock_ctx_t ctx = {
            .sema = sema,
            .irq_status = irq_status,
        };
        scheduler_park(semaphore_before_sleep, &ctx);
        if (thread->ticket != 0 || semaphore_can_acquire(sema)) {
            break;
        }
    }
}

void semaphore_release(semaphore_t* sema, bool handoff) {
    sema->value += 1;

    // Easy case: no waiters?
    // This check must happen after the add to avoid missed wakeup
    // (see loop in semaphore_acquire)
    if (sema->num_waiters == 0) {
        return;
    }

    bool irq_status = irq_spinlock_lock(&sema->lock);
    if (sema->num_waiters == 0) {
        // The count is already consumed by another thread,
        // so no need to wakeup another thread
        irq_spinlock_unlock(&sema->lock, irq_status);
        return;
    }

    list_entry_t* t = list_pop(&sema->wait_queue);

    // May be slow or even yield, so unlock first
    irq_spinlock_unlock(&sema->lock, irq_status);

    if (t != NULL) {
        thread_t* thread = containerof(t, thread_t, scheduler_node);

        ASSERT(thread->ticket == 0);

        if (handoff && semaphore_can_acquire(sema)) {
            thread->ticket = 1;
        }

        scheduler_wakeup_thread(thread);

        if (thread->ticket == 1) {
            // Direct thread handoff
            // TODO: put the thread in a next or something so it will run first
            scheduler_yield();
        }
    }
}

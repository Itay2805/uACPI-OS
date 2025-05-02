#include "cond.h"

#include "thread/scheduler.h"
#include "thread/thread.h"

static bool less(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static uint32_t cond_notify_list_add(cond_t* notify) {
    return (notify->wait += 1) - 1;
}

typedef struct cond_unlock_ctx {
    irq_spinlock_t* lock;
    bool irq_status;
} cond_unlock_ctx_t;

static bool cond_before_sleep(thread_t* thread, void* _ctx) {
    cond_unlock_ctx_t* cond = _ctx;
    irq_spinlock_unlock(cond->lock, cond->irq_status);
    return true;
}

static void cond_notify_list_wait(cond_t* notify, uint32_t ticket) {
    bool irq_status = irq_spinlock_lock(&notify->lock);

    // Return right away if this ticket has already been notified.
    if (less(ticket, notify->notify)) {
        irq_spinlock_unlock(&notify->lock, irq_status);
        return;
    }

    // Enqueue itself
    thread_t* thread = scheduler_get_current_thread();
    thread->ticket = ticket;

    if (notify->head == NULL) {
        notify->head = thread;
    } else {
        notify->tail->notify_next = thread;
    }
    notify->tail = thread;

    cond_unlock_ctx_t ctx = {
        .lock = &notify->lock,
        .irq_status = irq_status,
    };
    scheduler_park(cond_before_sleep, &ctx);
}

static void cond_notify_list_notify_one(cond_t* notify) {
    // Fast-path: if there are no new waiters since the last notification
    // we don't need to acquire the lock at all.
    if (notify->wait == notify->notify) {
        return;
    }

    bool irq_status = irq_spinlock_lock(&notify->lock);

    // Re-check under the lock if we need to do anything.
    uint32_t t = notify->notify;
    if (t == notify->wait) {
        irq_spinlock_unlock(&notify->lock, irq_status);
        return;
    }

    // Update the next notify ticket number
    notify->notify = t + 1;

    // Try to find the thread that needs to be notified.
    // If it hasn't made it to the list yet we won't find it,
    // but it won't park itself once it sees the new notify number.
    //
    // This scan looks linear but essentially always stops quickly.
    // Because thread's queue separately from taking numbers,
    // there may be minor reorderings in the list, but we
    // expect the thread we're looking for to be near the front.
    // The thread has others in front of it on the list only to the
    // extent that it lost the race, so the iteration will not
    // be too long. This applies even when the thread is missing:
    // it hasn't yet gotten to sleep and has lost the race to
    // the (few) other thread's that we find on the list.
    for (thread_t *prev = NULL, *thread = notify->head; thread != NULL; prev = thread, thread = thread->notify_next) {
        if (thread->ticket == t) {
            thread_t* next = thread->notify_next;
            if (prev != NULL) {
                prev->notify_next = next;
            } else {
                notify->head = next;
            }

            if (next == NULL) {
                notify->tail = NULL;
            }

            irq_spinlock_unlock(&notify->lock, irq_status);
            thread->notify_next = NULL;
            scheduler_wakeup_thread(thread);
            return;
        }
    }

    irq_spinlock_unlock(&notify->lock, irq_status);
}

static void cond_notify_list_notify_all(cond_t* notify) {
    // Fast-path: if there are no new waiters since the last notification
    // we don't need to acquire the lock.
    if (notify->wait == atomic_load(&notify->notify)) {
        return;
    }

    // Pull the list out into a local variable, waiters will be readied
    // outside the lock.
    bool irq_status = irq_spinlock_lock(&notify->lock);
    thread_t* thread = notify->head;
    notify->head = NULL;
    notify->tail = NULL;
    notify->notify = notify->wait;
    irq_spinlock_unlock(&notify->lock, irq_status);

    // Go through the local list and ready all waiters.
    while (thread != NULL) {
        thread_t* next = thread->notify_next;
        thread->notify_next = NULL;
        scheduler_wakeup_thread(thread);
        thread = next;
    }
}

void cond_wait(cond_t* cond, mutex_t* mutex) {
    uint32_t ticket = cond_notify_list_add(cond);
    mutex_unlock(mutex);
    cond_notify_list_wait(cond, ticket);
    mutex_lock(mutex);
}

void cond_wait_irq(cond_t* cond, irq_spinlock_t* lock, bool* irq_status) {
    uint32_t ticket = cond_notify_list_add(cond);
    irq_spinlock_unlock(lock, *irq_status);
    cond_notify_list_wait(cond, ticket);
    *irq_status = irq_spinlock_lock(lock);
}

void cond_signal(cond_t* cond) {
    cond_notify_list_notify_one(cond);
}

void cond_broadcast(cond_t* cond) {
    cond_notify_list_notify_all(cond);
}

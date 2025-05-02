#include "mutex.h"

#include "arch/intrin.h"
#include "thread/scheduler.h"
#include "time/tsc.h"

void mutex_lock_slow(mutex_t* mutex) {
    uint32_t old = atomic_load_explicit(&mutex->state, memory_order_relaxed);
    bool awoke = false;
    bool starving = false;
    uint64_t wait_start_time = 0;
    size_t iter = 0;
    for (;;) {
        // Don't spin in starvation mode, ownership is handed off to waiters
        // so we won't be able to acquire the mutex anyway.
        if ((old & (MUTEX_LOCKED | MUTEX_STARVING)) == MUTEX_LOCKED && scheduler_can_spin(iter)) {
            // Active spinning makes sense.
            // Try to set MUTEX_WOKEN flag to inform unlock
            // to not wake other blocked threads.
            if (!awoke && (old & MUTEX_WOKEN) == 0 && (old >> MUTEX_WAITER_SHIFT) != 0) {
                atomic_compare_exchange_strong(&mutex->state, &old, old | MUTEX_WOKEN);
                awoke = true;
            }

            for (int i = 0; i < 30; i++) {
                cpu_relax();
            }

            iter++;
            old = mutex->state;
            continue;
        }

        uint32_t new = old;

        // Don't try to acquire starving mutex, new arriving threads must queue.
        if ((old & MUTEX_STARVING) == 0) {
            new |= MUTEX_LOCKED;
        }

        if ((old & (MUTEX_LOCKED | MUTEX_STARVING)) != 0) {
            new += 1 << MUTEX_WAITER_SHIFT;
        }

        // The current thread switches mutex to starvation mode.
        // But if the mutex is currently unlocked, don't do the switch.
        // unlock expects that starving mutex has waiters, which will not
        // be true in this case.
        if (starving && (old & MUTEX_LOCKED) != 0) {
            new |= MUTEX_STARVING;
        }

        if (awoke) {
            // The thread has been woken from sleep,
            // so we need to reset the flag in either case.
            ASSERT((new & MUTEX_WOKEN) != 0);
            new &= ~MUTEX_WOKEN;
        }

        uint32_t temp_old = old;
        if (atomic_compare_exchange_strong(&mutex->state, &temp_old, new)) {
            if ((old & (MUTEX_LOCKED | MUTEX_STARVING)) == 0) {
                break; // locked the mutex with cas
            }

            // If we were already waiting before, queue at the front of the queue
            bool queue_lifo = wait_start_time != 0;
            if (wait_start_time == 0) {
                wait_start_time = get_tsc();
            }

            // TODO: timeout support
            semaphore_acquire(&mutex->sema, queue_lifo);

            starving = starving || tsc_to_ns(get_tsc() - wait_start_time) >= 1000000;
            old = mutex->state;
            if ((old & MUTEX_STARVING) != 0) {
                // If this thread was woken and mutex is in starvation mode,
                // ownership was handed off to us but mutex is in somewhat
                // inconsistent state: MUTEX_LOCKED is not set and we are still
                // accounted as waiter. Fix that.
                ASSERT((old & (MUTEX_LOCKED | MUTEX_WOKEN)) == 0);
                ASSERT(old >> MUTEX_WAITER_SHIFT != 0);

                uint32_t delta = MUTEX_LOCKED - (1 << MUTEX_WAITER_SHIFT);
                if (!starving || (old >> MUTEX_WAITER_SHIFT) == 1) {
                    // Exit starvation mode.
                    // Critical to do it here and consider wait time.
                    // Starvation mode is so inefficient, that two threads
                    // can go lock-step infinitely once they switch mutex
                    // to starvation mode.
                    delta -= MUTEX_STARVING;
                }
                atomic_fetch_add(&mutex->state, delta);
                break;
            }

            awoke = true;
            iter = 0;
        } else {
            old = temp_old;
        }
    }
}

void mutex_unlock_slow(mutex_t* mutex, uint32_t new) {
    ASSERT(((new + MUTEX_LOCKED) & MUTEX_LOCKED) != 0);

    if ((new & MUTEX_STARVING) == 0) {
        uint32_t old = new;
        for (;;) {
            // If there are no waiters or a thread has already
            // been woken or grabbed the lock, no need to wake anymore.
            // In starvation mode ownership is directly handed off from unlocking
            // thread to the next waiter. We are not part of this chain,
            // since we did not observe MUTEX_STARVING when we unlocked the mutex above.
            // So get off the way
            if ((old >> MUTEX_WAITER_SHIFT) == 0 || (old & (MUTEX_LOCKED | MUTEX_WOKEN | MUTEX_STARVING)) != 0) {
                return;
            }

            // Grab the right to wake someone
            new = (old - (1 << MUTEX_WAITER_SHIFT)) | MUTEX_WOKEN;
            if (atomic_compare_exchange_strong(&mutex->state, &old, new)) {
                semaphore_release(&mutex->sema, false);
                return;
            }
        }
    } else {
        // Starving mode: handoff mutex ownership to the next waiter, and yield
        // our time slice so that the next waiter can start to run immediately
        // NOTE: MUTEX_LOCKED is not set, the waiter will set it after wakeup.
        //but mutex is still considered locked if MUTEX_STARVING is set
        // so new coming threads won't acquire it
        semaphore_release(&mutex->sema, true);
    }
}

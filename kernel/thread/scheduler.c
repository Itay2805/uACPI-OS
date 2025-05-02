#include "scheduler.h"

#include <arch/intrin.h>
#include <mem/alloc.h>
#include <mem/stack.h>
#include <time/tsc.h>

#include "pcpu.h"
#include "thread.h"
#include <stdnoreturn.h>

typedef struct core_scheduler_context {
    // the park location, scheduler will sleep on this
    // location and wakeup on modifications to it
    __attribute__((aligned(128)))
    _Atomic(size_t) park;

    // The scheduler's runnable
    runnable_t scheduler;

    // when set to true preemption should not switch the context
    // but should set the want preemption flag instead
    int64_t preempt_count;

    // we got an preemption request while preempt count was 0
    // next time we enable preemption make sure to preempt
    bool want_preemption;

    // the eevdf queue of the core
    list_t queue;

    // lock to protect the queue
    spinlock_t queue_lock;

    // the current thread
    thread_t* current;
} core_scheduler_context_t;

/**
 * The current cpu's context
 */
static CPU_LOCAL core_scheduler_context_t m_core = {};

/**
 * The scheduler contexts of all cpus
 */
static core_scheduler_context_t** m_all_cores = NULL;

/**
 * Mask of cores that are in idle currently
 */
static _Atomic(uint64_t) m_idle_cores = 0;

/**
 * Sum of all the weights in the system
 */
static _Atomic(uint64_t) m_total_weights = 0;

static size_t m_cpu_count = 0;

err_t scheduler_init(size_t cpu_count) {
    err_t err = NO_ERROR;

    m_cpu_count = cpu_count;

    m_all_cores = mem_alloc(cpu_count * sizeof(core_scheduler_context_t*));
    CHECK_ERROR(m_all_cores != NULL, UACPI_STATUS_OUT_OF_MEMORY);

cleanup:
    return err;
}

err_t scheduler_init_per_core(void) {
    err_t err = NO_ERROR;

    // save the pointer of the current process
    m_all_cores[get_cpu_id()] = &m_core;

    // setup the task for the scheduler
    void* stack = stack_alloc();
    CHECK_ERROR(stack != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    runnable_set_rsp(&m_core.scheduler, stack);

    // start with a preempt count of 1, because we go to the scheduler right away
    m_core.preempt_count = 1;

    // and init the queue
    list_init(&m_core.queue);

cleanup:
    return err;
}

thread_t* scheduler_get_current_thread(void) {
    return m_core.current;
}

bool scheduler_can_spin(size_t i) {
    if (i >= 4 || m_cpu_count <= 1) {
        return false;
    }

    // TODO: check if the local run queue is empty, if not then don't spin

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Scheduler invocation
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef void (*scheduler_func_t)(void);

/**
 * Performs a scheduler call, must be done with preemption disabled
 */
static void scheduler_do_call(scheduler_func_t callback) {
    runnable_set_rip(&m_core.scheduler, callback);
    runnable_switch(&scheduler_get_current_thread()->runnable, &m_core.scheduler);
}

/**
 * Perform a scheduler call, this will disable preemption between
 * the calls to make sure we won't get any weird double switch
 */
static void scheduler_call(scheduler_func_t callback) {
    m_core.preempt_count++;
    scheduler_do_call(callback);
    m_core.preempt_count--;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Core sleeping and waking up
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void core_prepare_park(void) {
    atomic_store_explicit(&m_core.park, 1, memory_order_relaxed);
}

static void core_park(void) {
    // start by disabling the deadline so we
    // won't have a spurious wakeup
    pcpu_timer_clear();

    // and now wait until someone tells us to wakeup
    while (atomic_load_explicit(&m_core.park, memory_order_acquire) != 0) {
        asm("hlt");
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual scheduler
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Update the status of the thread properly
 */
static void scheduler_update_thread_status(thread_t* thread, thread_status_t old_val, thread_status_t new_val) {
    size_t yield_delay = 5 * 1000;

    uint64_t next_yield = 0;
    thread_status_t temp = old_val;
    for (int i = 0; !atomic_compare_exchange_strong(&thread->status, &temp, new_val); temp = old_val, i++) {
        ASSERT(old_val != THREAD_STATUS_WAITING || thread->status != THREAD_STATUS_RUNNABLE);

        if (i == 0) {
            next_yield = tsc_ns_deadline(yield_delay);
        }

        if (tsc_check_deadline(next_yield)) {
            scheduler_yield();
            next_yield = tsc_ns_deadline(yield_delay / 2);

        } else {
            for (int x = 0; x < 10 && thread->status != old_val; x++) {
                cpu_relax();
            }
        }
    }
}

/**
 * Execute the given thread
 */
noreturn static void scheduler_execute(thread_t* thread, bool inherit_time) {
    // set the current thread
    m_core.current = thread;

    // switch the thread to be running
    scheduler_update_thread_status(thread, THREAD_STATUS_RUNNABLE, THREAD_STATUS_RUNNING);

    // set the timer slice if we don't inherit the current time
    if (!inherit_time) {
        pcpu_timer_set_timeout(10);
    }

    // and actually resume it
    runnable_resume(&thread->runnable);
}

/**
 * Perform the scheduling, this function must be called on the scheduler stack
 * and must be called with preemption disabled and interrupts enabled
 */
static void scheduler_schedule(void) {
    ASSERT(m_core.preempt_count != 0);

    for (;;) {
        // choose the next thread to run, we need to disable interrupts to make sure
        // that interrupts don't attempt to wake up any thread
        spinlock_lock(&m_core.queue_lock);
        list_entry_t* choosen = list_pop(&m_core.queue);
        spinlock_unlock(&m_core.queue_lock);

        // if we did not find anything, attempt to steal
        if (choosen == NULL) {
            // TODO: this
        }

        // found something to run, so run it
        if (choosen != NULL) {
            thread_t* thread = containerof(choosen, thread_t, scheduler_node);
            scheduler_execute(thread, false);
        }

        // prepare the park
        core_prepare_park();

        // mark as idle
        atomic_fetch_or_explicit(&m_idle_cores, 1ull << get_cpu_id(), memory_order_relaxed);

        // could not find one, put the core to sleep
        // and wait until there is something available
        core_park();

        // mark as not idle
        atomic_fetch_and_explicit(&m_idle_cores, ~(1ull << get_cpu_id()), memory_order_relaxed);
    }
}

/**
 * Runs on the scheduler stack with preemption disabled,
 * will requeue the current thread and schedule a new one
 */
static void scheduler_yield_internal(void) {
    thread_t* thread = m_core.current;

    // set as runnable instead of running
    scheduler_update_thread_status(thread, THREAD_STATUS_RUNNING, THREAD_STATUS_RUNNABLE);

    // no longer the current
    m_core.current = NULL;

    // TODO: attempt to push to another core

    // put in the run queue for the core
    spinlock_lock(&m_core.queue_lock);
    list_add_tail(&m_core.queue, &thread->scheduler_node);
    spinlock_unlock(&m_core.queue_lock);

    // and schedule it
    scheduler_schedule();
}

/**
 * Runs on the scheduler stack with preemption disabled,
 * will park the current thread and schedule a new one
 */
static void scheduler_park_internal(void) {
    thread_t* thread = m_core.current;

    // switch to a waiting status
    scheduler_update_thread_status(thread, THREAD_STATUS_RUNNING, THREAD_STATUS_WAITING);

    // don't have it in the current anyore
    m_core.current = NULL;

    scheduler_park_callback_t callback = thread->park_callback;
    if (callback != NULL) {
        bool ok = callback(thread, thread->park_arg);
        thread->park_callback = NULL;
        thread->park_arg = NULL;

        // if returned not ok, then the check wanted to abort the parking
        if (!ok) {
            scheduler_update_thread_status(thread, THREAD_STATUS_WAITING, THREAD_STATUS_RUNNABLE);

            // schedule it back
            scheduler_execute(thread, true);
        }
    }

    scheduler_schedule();
}

/**
 * Runs on the scheduler stack with preemption disabled,
 * will drop the current thread and schedule a new one
 */
static void scheduler_exit_internal(void) {
    // mark the thread as dead
    thread_t* thread = m_core.current;
    scheduler_update_thread_status(thread, THREAD_STATUS_RUNNING, THREAD_STATUS_DEAD);

    // free the thread
    m_core.current = NULL;
    thread_free(thread);

    // and schedule something else
    scheduler_schedule();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Scheduler API
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void scheduler_start_thread(thread_t* thread) {
    scheduler_preempt_disable();

    // mark as runnable
    scheduler_update_thread_status(thread, THREAD_STATUS_IDLE, THREAD_STATUS_RUNNABLE);

    // and add to the queue
    spinlock_lock(&m_core.queue_lock);
    list_add(&m_core.queue, &thread->scheduler_node);
    spinlock_unlock(&m_core.queue_lock);

    // TODO: attempt to wakeup cores if needed

    scheduler_preempt_enable();
}

void scheduler_wakeup_thread(thread_t* thread) {
    // disable preemption so the scheduler won't run while
    // we are doing stuff on this core
    scheduler_preempt_disable();

    // switch to be runnable
    scheduler_update_thread_status(thread, THREAD_STATUS_WAITING, THREAD_STATUS_RUNNABLE);

    // put on the local run queue
    spinlock_lock(&m_core.queue_lock);
    list_add(&m_core.queue, &thread->scheduler_node);
    spinlock_unlock(&m_core.queue_lock);

    // wakeup the core, this ensures that
    // we will not go back to sleep
    m_core.park = 0;

    // enable preemption again
    scheduler_preempt_enable();
}

void scheduler_yield(void) {
    // only call the scheduler yield if we have preemption enabled
    // and interrupts are turned on, meaning we are in a context
    // where we are allowed to yield
    // otherwise this is a nop
    if (m_core.preempt_count == 0 && (__builtin_ia32_readeflags_u64() & BIT9)) {
        scheduler_call(scheduler_yield_internal);
    }
}

void scheduler_park(scheduler_park_callback_t callback, void* ctx) {
    // prepare for parking and call the scheduler
    thread_t* thread = scheduler_get_current_thread();
    ASSERT(thread->status == THREAD_STATUS_RUNNING);
    thread->park_callback = callback;
    thread->park_arg = ctx;
    scheduler_call(scheduler_park_internal);
}

void scheduler_exit(void) {
    // call the scheduler exit
    scheduler_call(scheduler_exit_internal);
}

void scheduler_preempt(void) {
    // if we have a preempt count then don't call
    // the yield
    if (m_core.preempt_count != 0) {
        m_core.want_preemption = true;
        return;
    }

    ASSERT(scheduler_get_current_thread() != NULL);

    // we can safely call the yield, this will ensure
    // interrupts are enabled
    m_core.preempt_count++;
    runnable_set_rip(&m_core.scheduler, scheduler_yield_internal);
    runnable_switch_enable_interrupts(&scheduler_get_current_thread()->runnable, &m_core.scheduler);
    m_core.preempt_count--;
}

void scheduler_start_per_core(void) {
    // make sure the preempt count is non-zero
    m_core.preempt_count++;

    // enable interrupts at this point
    asm("sti");

    // set the scheduler_schedule as target
    runnable_set_rip(&m_core.scheduler, scheduler_schedule);

    // use the jump since we don't have a valid thread right now
    runnable_resume(&m_core.scheduler);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Preemption handling
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void scheduler_preempt_disable(void) {
    m_core.preempt_count++;
}

void scheduler_preempt_enable(void) {
    if (m_core.preempt_count == 1 && m_core.want_preemption) {
        scheduler_do_call(scheduler_yield_internal);
    }

    --m_core.preempt_count;
    ASSERT(m_core.preempt_count >= 0);
}

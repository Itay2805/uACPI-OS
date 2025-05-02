#pragma once

#include <arch/idt.h>
#include <mem/memory.h>

#include "lib/defs.h"

#include <stdatomic.h>
#include <lib/list.h>
#include <sync/spinlock.h>

#include "runnable.h"

typedef void (*thread_entry_t)(void *arg);

typedef enum thread_status {
    THREAD_STATUS_IDLE,
    THREAD_STATUS_RUNNABLE,
    THREAD_STATUS_RUNNING,
    THREAD_STATUS_WAITING,
    THREAD_STATUS_DEAD,
} thread_status_t;

typedef struct thread thread_t;

typedef bool (*scheduler_park_callback_t)(thread_t* thread, void* ctx);

struct thread {
    // The thread name, not null terminated
    char name[256];

    // link in a list of active threads
    list_entry_t link;

    // the ref-count on the thread
    atomic_size_t ref_count;

    // the runnable of this thread, to queue on the scheduler
    runnable_t runnable;

    // The actual stack of the thread
    void* stack_top;

    // the entry point to actually run
    void* arg;
    thread_entry_t entry;

    // The node for the scheduler
    // when waiting used by the wait structure
    list_entry_t scheduler_node;

    // the notify list link
    struct thread* notify_next;

    // the ticket passed to the
    // semaphore we are waiting on
    uint32_t ticket;

    // the status of the thread
    _Atomic(thread_status_t) status;

    // for parking
    scheduler_park_callback_t park_callback;
    void* park_arg;
};

/**
 * Dump the current threads
 */
void thread_dump(void);

/**
 * Increment the ref count of the thread
 */
static inline thread_t* thread_ref(thread_t* thread) { thread->ref_count++; return thread; }

/**
* Create a new thread, you need to schedule it yourself
*/
thread_t* thread_create(thread_entry_t callback, void *arg, const char* name_fmt, ...);

/**
 * Free the given thread, returning it to the freelist
 */
void thread_free(thread_t* thread);

/**
* Exit from the thread right now
*/
void thread_exit();

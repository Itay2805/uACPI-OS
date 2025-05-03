#include "thread.h"

#include <arch/gdt.h>
#include <arch/intrin.h>
#include <lib/list.h>
#include <lib/string.h>
#include <sync/spinlock.h>

#include "scheduler.h"
#include "mem/alloc.h"
#include "mem/stack.h"

static list_t m_threads = LIST_INIT(&m_threads);
static irq_spinlock_t m_threads_lock = INIT_IRQ_SPINLOCK();

static thread_t* thread_alloc() {
    thread_t* thread = mem_alloc(sizeof(*thread));
    if (thread == NULL) {
        return NULL;
    }
    memset(thread, 0, sizeof(*thread));

    // initialize anything that it needs, we multiply after the ++ because we want to get
    // the top of the stack, not the bottom of it
    thread->stack_top = stack_alloc();
    thread->ref_count = 1;

    bool irq_status = irq_spinlock_lock(&m_threads_lock);
    list_add(&m_threads, &thread->link);
    irq_spinlock_unlock(&m_threads_lock, irq_status);

    return thread;
}

static const char* m_thread_status_str[] = {
    [THREAD_STATUS_IDLE] = "idle",
    [THREAD_STATUS_RUNNABLE] = "runnable",
    [THREAD_STATUS_RUNNING] = "running",
    [THREAD_STATUS_WAITING] = "waiting",
    [THREAD_STATUS_DEAD] = "dead",
};

void thread_dump(void) {
    bool irq_status = irq_spinlock_lock(&m_threads_lock);

    TRACE("Current threads:");
    for (list_entry_t* entry = m_threads.next; entry != &m_threads; entry = entry->next) {
        thread_t* thread = containerof(entry, thread_t, link);
        TRACE("\t`%s`: %s", thread->name, m_thread_status_str[thread->status]);
    }

    irq_spinlock_unlock(&m_threads_lock, irq_status);
}

static void thread_entry() {
    // we need to enable preemption manually since we
    // are not coming from a scheduler_call stub
    scheduler_preempt_enable();

    // and now run the
    thread_t* thread = scheduler_get_current_thread();
    thread->entry(thread->arg);
    thread_exit();
}

thread_t* thread_vcreate(thread_entry_t callback, void* arg, const char* name_fmt, va_list va) {
    thread_t* thread = thread_alloc();
    if (thread == NULL) {
        return NULL;
    }

    // set the name
    uacpi_vsnprintf(thread->name, sizeof(thread->name), name_fmt, va);

    // initialize the callback, this will be used by the thread_entry to
    // call the real entry point
    thread->entry = callback;
    thread->arg = arg;

    // set the thread entry as the first function to run
    // it will call the callback properly with its argument
    runnable_set_rsp(&thread->runnable, thread->stack_top);
    runnable_set_rip(&thread->runnable, thread_entry);

    // TODO: fpu context

    return thread;
}

void thread_free(thread_t* thread) {
    if (--thread->ref_count == 0) {
        bool irq_status = irq_spinlock_lock(&m_threads_lock);
        list_del(&thread->link);
        irq_spinlock_unlock(&m_threads_lock, irq_status);

        stack_free(thread->stack_top);
        mem_free(thread);
    }
}

void thread_exit() {
    scheduler_exit();
}

#include "thread.h"

#include <arch/gdt.h>
#include <arch/intrin.h>
#include <lib/list.h>
#include <lib/string.h>
#include <sync/spinlock.h>

#include "scheduler.h"
#include "mem/alloc.h"
#include "mem/stack.h"

static thread_t* thread_alloc() {
    thread_t* thread = mem_alloc(sizeof(*thread));
    if (thread == NULL) {
        return NULL;
    }
    memset(thread, 0, sizeof(*thread));

    // initialize anything that it needs, we multiply after the ++ because we want to get
    // the top of the stack, not the bottom of it
    thread->stack_top = stack_alloc();

    return thread;
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

thread_t* thread_create(thread_entry_t callback, void* arg, const char* name_fmt, ...) {
    thread_t* thread = thread_alloc();
    if (thread == NULL) {
        return NULL;
    }

    // set the name
    va_list va;
    va_start(va, name_fmt);
    uacpi_snprintf(thread->name, sizeof(thread->name) - 1, name_fmt, va);
    va_end(va);

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
    stack_free(thread->stack_top);
    mem_free(thread);
}

void thread_exit() {
    scheduler_exit();
}

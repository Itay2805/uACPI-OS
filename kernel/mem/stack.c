#include "stack.h"

#include "memory.h"
#include "lib/defs.h"
#include "lib/list.h"
#include "sync/spinlock.h"

static list_t m_stack_freelist = LIST_INIT(&m_stack_freelist);

static spinlock_t m_stack_freelist_lock = INIT_SPINLOCK();

static _Atomic(uintptr_t) m_stack_watermark = STACKS_ADDR;

void* stack_alloc(void) {
    spinlock_lock(&m_stack_freelist_lock);
    void* stack = list_pop(&m_stack_freelist);
    spinlock_unlock(&m_stack_freelist_lock);

    if (stack == NULL) {
        stack = (void*)atomic_fetch_add(&m_stack_watermark, SIZE_8MB);
    } else {
        stack += sizeof(list_entry_t);
    }

    // turn into the top of stack pointer
    return stack + SIZE_8MB;
}

void stack_free(void* stack_top) {
    list_entry_t* stack = stack_top - sizeof(list_entry_t);

    spinlock_lock(&m_stack_freelist_lock);
    list_add(&m_stack_freelist, stack);
    spinlock_unlock(&m_stack_freelist_lock);
}

#include "spinlock.h"

uacpi_handle uacpi_kernel_create_spinlock(void) {
    spinlock_t* lock = uacpi_kernel_alloc(sizeof(spinlock_t));
    if (lock != NULL) {
        atomic_flag_clear_explicit(&lock->flag, memory_order_relaxed);
    }
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle lock) {
    uacpi_kernel_free(lock);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    // disable interrupts
    uacpi_cpu_flags flags = __builtin_ia32_readeflags_u64();
    if (flags & 0x0200) asm("cli");

    // lock the lock
    spinlock_t* lock = handle;
    while (!atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire));

    return flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    // unlock the lock
    spinlock_t* lock = handle;
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);

    // restore the interrupt state
    __builtin_ia32_writeeflags_u64(flags);
}


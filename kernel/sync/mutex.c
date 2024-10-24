#include "mutex.h"

uacpi_handle uacpi_kernel_create_mutex(void) {
    mutex_t* mutex = uacpi_kernel_alloc(sizeof(mutex_t));
    if (mutex != NULL) {
        atomic_flag_clear_explicit(&mutex->flag, memory_order_relaxed);
    }
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle mutex) {
    uacpi_kernel_free(mutex);
}

uacpi_bool uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    uint64_t deadline = UINT64_MAX;
    if (timeout < UINT16_MAX) {
        deadline = uacpi_kernel_get_ticks() + (((uint64_t)timeout) * 10000);
    }

    // TODO: replace with monitor
    mutex_t* mutex = handle;
    while (!atomic_flag_test_and_set_explicit(&mutex->flag, memory_order_acquire)) {
        // wait a little
        __builtin_ia32_pause();

        // check if we got a timeout
        if (uacpi_kernel_get_ticks() >= deadline) {
            return false;
        }
    }

    return true;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    mutex_t* mutex = handle;
    atomic_flag_clear_explicit(&mutex->flag, memory_order_release);
}

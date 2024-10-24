#include <uacpi/kernel_api.h>

#include <stdatomic.h>

typedef struct event {
    atomic_uint_least64_t count;
} event_t;

uacpi_handle uacpi_kernel_create_event(void) {
    event_t* event = uacpi_kernel_alloc(sizeof(event_t));
    if (event != NULL) {
        event->count = 0;
    }
    return event;
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    uacpi_kernel_free(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    uint64_t deadline = UINT64_MAX;
    if (timeout < UINT16_MAX) {
        deadline = uacpi_kernel_get_ticks() + (((uint64_t)timeout) * 10000);
    }

    event_t* event = handle;
    for (;;) {
        // wait until the count is valid
        // TODO: replace with monitor
        size_t count = 0;
        while ((count = atomic_load_explicit(&event->count, memory_order_relaxed)) == 0) {
            // wait a little
            __builtin_ia32_pause();

            // check if we got a timeout
            if (uacpi_kernel_get_ticks() >= deadline) {
                return false;
            }
        }

        // we got a non-zero count, attempt to decrement it,
        while (count != 0) {
            // we will try to decrement it by one, if we fail we will try
            // again until the count is back to zero, at which point we are
            // going to wait again for it to not be zero
            if (atomic_compare_exchange_strong_explicit(
                &event->count,
                &count, count - 1,
                memory_order_acquire, memory_order_relaxed)
            ) {
                return true;
            }
        }
    }
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    event_t* event = handle;
    atomic_fetch_add_explicit(&event->count, 1, memory_order_release);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    event_t* event = handle;
    atomic_store_explicit(&event->count, 0, memory_order_release);
}

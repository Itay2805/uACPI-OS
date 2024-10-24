#include "sleep.h"

#include <uacpi/kernel_api.h>

#include <uacpi/uacpi.h>

static uint64_t m_tsc_frequency = 1;

void init_sleep(void) {

}

uacpi_u64 uacpi_kernel_get_ticks(void) {
    uint64_t tsc = __builtin_ia32_rdtsc();
    uint64_t ticks = (tsc / m_tsc_frequency) * 10000000u;
    return ticks;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t ticks = uacpi_kernel_get_ticks() + (usec * 10);
    while (ticks <= uacpi_kernel_get_ticks());
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    uint64_t ticks = uacpi_kernel_get_ticks() + (msec * 10000);
    while (ticks <= uacpi_kernel_get_ticks()) {
        // TODO: potentially actually sleep? wait for interrupt?
        __builtin_ia32_pause();
    }
}

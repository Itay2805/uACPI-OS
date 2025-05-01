#include "log.h"

#include "arch/intrin.h"
#include "sync/spinlock.h"
#include "lib/defs.h"

#include <stdarg.h>

#include <limine.h>
#include <limine_requests.h>
#include <stdbool.h>
#include <stddef.h>

#include "lib/string.h"
#include "uacpi/internal/stdlib.h"

static irq_spinlock_t m_debug_lock = INIT_IRQ_SPINLOCK();

void init_early_logging() {
    // TODO: framebuffer
}

static void kputchar(char c) {
    __outbyte(0xE9, c);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* s) {
    bool irq_state = irq_spinlock_lock(&m_debug_lock);

    for (; *s != 0; s++) {
        kputchar(*s);
    }

    irq_spinlock_unlock(&m_debug_lock, irq_state);
}

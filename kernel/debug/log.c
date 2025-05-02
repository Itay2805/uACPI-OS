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
#include "flanterm.h"
#include "backends/fb.h"

static irq_spinlock_t m_debug_lock = INIT_IRQ_SPINLOCK();

static struct flanterm_context* m_flanterm_context = NULL;

static bool m_e9_enabled = false;

void init_early_logging() {
    // detect e9 support
    m_e9_enabled = __inbyte(0xE9) == 0xE9;

    // initialize the framebuffer

    // framebuffer
    struct limine_framebuffer_response* response = g_limine_framebuffer_request.response;
    if (response != NULL && response->framebuffer_count >= 1) {
        struct limine_framebuffer* framebuffer = response->framebuffers[0];
        TRACE("Using framebuffer #0 - %p - %ldx%ld (pitch=%ld)", framebuffer->address, framebuffer->width, framebuffer->height, framebuffer->pitch);
        m_flanterm_context = flanterm_fb_init(
            NULL,
            NULL,
            framebuffer->address, framebuffer->width, framebuffer->height, framebuffer->pitch,
            framebuffer->red_mask_size, framebuffer->red_mask_shift,
            framebuffer->green_mask_size, framebuffer->green_mask_shift,
            framebuffer->blue_mask_size, framebuffer->blue_mask_shift,
            NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, NULL,
            NULL, 0, 0, 1,
            0, 0,
            0
        );
    }
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* str) {
    bool irq_state = irq_spinlock_lock(&m_debug_lock);

    if (m_e9_enabled) {
        for (const char* s = str; *s != 0; s++) {
            __outbyte(0xE9, *s);
        }
    }

    if (m_flanterm_context != NULL) {
        flanterm_write(m_flanterm_context, str, strlen(str));
    }

    irq_spinlock_unlock(&m_debug_lock, irq_state);
}

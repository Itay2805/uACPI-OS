#include "acpi.h"

#include <limine_requests.h>
#include <mem/alloc.h>
#include <mem/virt.h>
#include <sync/spinlock.h>
#include <time/tsc.h>
#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/tables.h>

#include "limine.h"
#include "lib/defs.h"

#include "mem/memory.h"
#include "arch/intrin.h"

#include <uacpi/uacpi.h>

#include "lib/cmdline.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"
#include "thread/scheduler.h"

/**
 * The frequency of the acpi timer
 */
#define ACPI_TIMER_FREQUENCY  3579545

/**
 * The timer port
 */
static uint16_t m_acpi_timer_port;

/**
 * The RSDP, saved for uACPI
 */
static uint64_t m_rsdp_phys;

static uacpi_table m_mcfg;
static size_t m_mcfg_entry_count = 0;

err_t init_acpi() {
    err_t err = NO_ERROR;

    uacpi_log_level log_level = UACPI_LOG_INFO;
    const char* cmdline = g_limine_executable_file_request.response->executable_file->cmdline;
    if (cmdline != NULL) {
        char* loglevel = cmdline_get_option_value(cmdline, "uacpi-log-level");
        if (loglevel != NULL) {
            if (strcmp(loglevel, "debug") == 0) log_level = UACPI_LOG_DEBUG;
            else if (strcmp(loglevel, "trace") == 0) log_level = UACPI_LOG_TRACE;
            else if (strcmp(loglevel, "info") == 0) log_level = UACPI_LOG_INFO;
            else if (strcmp(loglevel, "warn") == 0) log_level = UACPI_LOG_WARN;
            else if (strcmp(loglevel, "error") == 0) log_level = UACPI_LOG_ERROR;
            else WARN("Invalid value for uacpi-log-level: %s", loglevel);
            mem_free(loglevel);
        }
    }
    uacpi_context_set_log_level(log_level);

    CHECK(g_limine_rsdp_request.response != NULL);
    m_rsdp_phys = g_limine_rsdp_request.response->address;

    // initialize uACPI
    CHECK_UACPI(uacpi_initialize(0));

    // get the FADT
    struct acpi_fadt* fadt = NULL;
    CHECK_UACPI(uacpi_table_fadt(&fadt));

    // get the ACPI timer configuration
    CHECK(fadt->x_pm_tmr_blk.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO);
    CHECK(fadt->x_pm_tmr_blk.address <= UINT16_MAX, "%lu", fadt->x_pm_tmr_blk.address);
    m_acpi_timer_port = fadt->x_pm_tmr_blk.address;

    // get the pcie mappings and save the count
    CHECK_UACPI(uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &m_mcfg));
    m_mcfg_entry_count = (m_mcfg.hdr->length - sizeof(struct acpi_mcfg)) / sizeof(struct acpi_mcfg_allocation);

cleanup:
    return err;
}

uint32_t acpi_get_timer_tick(void) {
    return __indword(m_acpi_timer_port);
}

void acpi_stall(uint64_t microseconds) {
    uint32_t delay = (microseconds * ACPI_TIMER_FREQUENCY) / 1000000u;
    uint32_t times = delay >> 22;
    delay &= BIT22 - 1;
    do {
        uint32_t ticks = acpi_get_timer_tick() + delay;
        delay = BIT22;
        while (((ticks - acpi_get_timer_tick()) & BIT23) == 0) {
            cpu_relax();
        }
    } while (times-- > 0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// uACPI kernel API
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Misc
//

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    *out_rsdp_address = m_rsdp_phys;
    return UACPI_STATUS_OK;
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return scheduler_get_current_thread();
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    uint64_t tsc = get_tsc();
    uint64_t q = tsc / g_tsc_freq_hz;
    uint64_t r = tsc % g_tsc_freq_hz;
    uint64_t ns = q * 1000000000ULL;
    ns += (r * 1000000000ULL) / g_tsc_freq_hz;
    return ns;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t deadline = tsc_us_deadline(usec);
    while (tsc_check_deadline(deadline));
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    // TODO: turn into a real sleep instead
    uint64_t deadline = tsc_ms_deadline(msec);
    while (tsc_check_deadline(deadline)) {
        cpu_relax();
    }
}


//
// Logging
//

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request) {
    switch (request->type) {
        case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT: {
            ERROR("acpi: Breakpoint()");
        } break;

        case UACPI_FIRMWARE_REQUEST_TYPE_FATAL: {
            ERROR("acpi: Fatal(%d, %d, %ld)", request->fatal.type, request->fatal.code, request->fatal.arg);
        } break;
    }
    return UACPI_STATUS_OK;
}

//
// Spinlock
//

uacpi_handle uacpi_kernel_create_spinlock(void) {
    spinlock_t* spinlock = mem_alloc(sizeof(*spinlock));
    if (spinlock == NULL) return NULL;
    *spinlock = INIT_SPINLOCK();
    return spinlock;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    mem_free(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    return irq_spinlock_lock(handle);
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    return irq_spinlock_unlock(handle, flags);
}

//
// Mutex
//

uacpi_handle uacpi_kernel_create_mutex(void) {
    mutex_t* mutex = mem_alloc(sizeof(*mutex));
    if (mutex == NULL) return NULL;
    *mutex = INIT_MUTEX();
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    mem_free(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    mutex_t* mutex = handle;

    uint64_t deadline = 0;
    if (timeout != 0xFFFF) {
        deadline = tsc_ms_deadline(timeout);
    }

    if (mutex_try_lock_until(mutex, deadline)) {
        return UACPI_STATUS_OK;
    } else {
        return UACPI_STATUS_TIMEOUT;
    }
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    mutex_t* mutex = handle;
    mutex_unlock(mutex);
}

//
// Semaphore
//

uacpi_handle uacpi_kernel_create_event(void) {
    semaphore_t* semaphore = mem_alloc(sizeof(*semaphore));
    if (semaphore == NULL) return NULL;
    *semaphore = INIT_SEMAPHORE();
    return semaphore;
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    mem_free(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    semaphore_t* semaphore = handle;

    // calculate the deadline
    uint64_t deadline = 0;
    if (timeout != 0xFFFF) {
        deadline = tsc_ms_deadline(timeout);
    }
    return semaphore_wait_until(semaphore, deadline);
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    semaphore_t* semaphore = handle;
    semaphore_signal(semaphore);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    semaphore_t* semaphore = handle;
    semaphore_reset(semaphore);
}

//
// IO access
//

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    *out_handle = (uacpi_handle)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {}

uacpi_status uacpi_kernel_io_read8(uacpi_handle base, uacpi_size offset, uacpi_u8 *out_value) { *out_value = __inbyte((uintptr_t)base + offset); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle base, uacpi_size offset, uacpi_u16 *out_value) { *out_value = __inword((uintptr_t)base + offset); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle base, uacpi_size offset, uacpi_u32 *out_value) { *out_value = __indword((uintptr_t)base + offset); return UACPI_STATUS_OK; }

uacpi_status uacpi_kernel_io_write8(uacpi_handle base, uacpi_size offset, uacpi_u8 in_value) { __outbyte((uintptr_t)base + offset, in_value); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle base, uacpi_size offset, uacpi_u16 in_value) { __outword((uintptr_t)base + offset, in_value); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle base, uacpi_size offset, uacpi_u32 in_value) { __outdword((uintptr_t)base + offset, in_value); return UACPI_STATUS_OK; }

//
// Physical memory access
//

void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    return PHYS_TO_DIRECT(addr);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    // nop
}

//
// Pci access
//

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    struct acpi_mcfg* mcfg = m_mcfg.ptr;
    for (int i = 0; i < m_mcfg_entry_count; i++) {
        struct acpi_mcfg_allocation* entry = &mcfg->entries[i];
        if (address.segment != entry->segment) continue;
        if (address.bus < entry->start_bus) continue;
        if (address.bus > entry->end_bus) continue;
        *out_handle = PHYS_TO_DIRECT(entry->address) + (((address.bus - entry->start_bus) * 256) + (address.device * 8) + address.function) * 4096;
        return UACPI_STATUS_OK;
    }
    return UACPI_STATUS_NOT_FOUND;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {

}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value) { *value = *(volatile uint8_t*)(device + offset); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value) { *value = *(volatile uint16_t*)(device + offset); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value) { *value = *(volatile uint32_t*)(device + offset); return UACPI_STATUS_OK; }

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value) { *(volatile uint8_t*)(device + offset) = value; return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value) { *(volatile uint16_t*)(device + offset) = value; return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value) { *(volatile uint32_t*)(device + offset) = value; return UACPI_STATUS_OK; }

//
// Memory allocation
//

void* uacpi_kernel_alloc(uacpi_size size) {
    return mem_alloc(size);
}

void uacpi_kernel_free(void *mem) {
    mem_free(mem);
}

//
// Interrupts
//

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx, uacpi_handle *out_irq_handle) {
    TRACE("TODO: uacpi_kernel_install_interrupt_handler");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    TRACE("TODO: uacpi_kernel_uninstall_interrupt_handler");
    return UACPI_STATUS_OK;
}

//
// Work queues
//

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx) {
    TRACE("TODO: uacpi_kernel_schedule_work");
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    TRACE("TODO: uacpi_kernel_wait_for_work_completion");
    return UACPI_STATUS_UNIMPLEMENTED;
}
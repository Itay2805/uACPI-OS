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

#include "arch/apic.h"
#include "lib/cmdline.h"
#include "sync/mutex.h"
#include "sync/semaphore.h"
#include "thread/intr.h"
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

typedef struct isa_override_entry {
    uint8_t source;
    ioapic_irq_t entry;
} isa_override_entry_t;

static isa_override_entry_t* m_isa_override_entries = NULL;
static size_t m_isa_override_entries_count = 0;

static err_t register_ioapic_redirects(void) {
    err_t err = NO_ERROR;
    static uacpi_table madt_table;
    bool unref_table = false;

    CHECK_UACPI(uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt_table));
    unref_table = true;

    // iterate the entries
    struct acpi_madt* madt = madt_table.ptr;
    void* madt_end = madt_table.ptr + madt_table.hdr->length;

    struct acpi_entry_hdr* hdr = &madt->entries[0];
    while ((void*)(hdr + 1) <= madt_end) {
        void* next_hdr = ((void*)hdr) + hdr->length;
        if (next_hdr >= madt_end) {
            break;
        }

        switch (hdr->type) {
            case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
                struct acpi_madt_interrupt_source_override* override = (void*)hdr;

                m_isa_override_entries_count++;
                m_isa_override_entries = mem_realloc(m_isa_override_entries, m_isa_override_entries_count * sizeof(*m_isa_override_entries));
                CHECK_ERROR(m_isa_override_entries != NULL, UACPI_STATUS_OUT_OF_MEMORY);

                m_isa_override_entries[m_isa_override_entries_count - 1].source = override->source;

                bool assertion_level;
                const char* assertion_level_str;
                if ((override->flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_CONFORMING) { assertion_level = false; assertion_level_str = "dfl"; }
                else if ((override->flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_HIGH) { assertion_level = true; assertion_level_str = "high"; }
                else if ((override->flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW) { assertion_level = false; assertion_level_str = "low"; }
                else CHECK_FAIL();

                bool level_triggered;
                const char* level_trigger_str;
                if ((override->flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_CONFORMING) { level_triggered = true; level_trigger_str = "dfl"; }
                else if ((override->flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_LEVEL) { level_triggered = true; level_trigger_str = "level"; }
                else if ((override->flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_EDGE) { level_triggered = false; level_trigger_str = "edge"; }
                else CHECK_FAIL();

                TRACE("acpi: INT_SRC_OVR (bus %d bus_irq %d global_irq %d %s %s)",
                    override->bus, override->source, override->gsi, assertion_level_str, level_trigger_str);

                if (override->bus == 0) {
                    m_isa_override_entries[m_isa_override_entries_count - 1].entry = (ioapic_irq_t){
                        .irq = override->gsi,
                        .assertion_level = assertion_level,
                        .level_triggered = level_triggered,
                    };
                }
            } break;

            case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
                struct acpi_madt_ioapic* ioapic = (void*)hdr;
                RETHROW(ioapic_add(ioapic->address, ioapic->gsi_base));
            } break;
        }

        hdr = next_hdr;
    }

cleanup:
    if (unref_table) {
        uacpi_table_unref(&madt_table);
    }

    return err;
}

ioapic_irq_t acpi_convert_isa_to_gsi(uint8_t isa_irq) {
    // ISA bus is level-triggered, active-low by default
    ioapic_irq_t entry = (ioapic_irq_t){
        .irq = isa_irq,
        .level_triggered = false,
        .assertion_level = false,
    };

    // search for an override entry for this isa irq
    for (int i = 0; i < m_isa_override_entries_count; i++) {
        if (m_isa_override_entries[i].source == isa_irq) {
            entry = m_isa_override_entries[i].entry;
            break;
        }
    }

    return entry;
}

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

    // register all the ioapic redirects
    RETHROW(register_ioapic_redirects());

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
    while (!tsc_check_deadline(deadline));
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    // TODO: turn into a real sleep instead
    uint64_t deadline = tsc_ms_deadline(msec);
    while (!tsc_check_deadline(deadline)) {
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
    memset(mutex, 0, sizeof(*mutex));
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    mem_free(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    mutex_t* mutex = handle;

    // uint64_t deadline = 0;
    // if (timeout != 0xFFFF) {
    //     deadline = tsc_ms_deadline(timeout);
    // }
    // ASSERT(deadline == 0);

    mutex_lock(mutex);
    return UACPI_STATUS_OK;
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
    memset(semaphore, 0, sizeof(*semaphore));
    return semaphore;
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    mem_free(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    semaphore_t* semaphore = handle;

    // calculate the deadline
    // uint64_t deadline = 0;
    // if (timeout != 0xFFFF) {
    //     deadline = tsc_ms_deadline(timeout);
    // }
    // ASSERT(deadline == 0);

    semaphore_acquire(semaphore, false);
    return true;
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    semaphore_t* semaphore = handle;
    semaphore_release(semaphore, false);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    semaphore_t* semaphore = handle;
    semaphore->value = 0;
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
    uint64_t addr_end;
    if (__builtin_add_overflow(addr, len, &addr_end)) return NULL;
    if (addr_end >= DIRECT_MAP_SIZE) return NULL;
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

typedef struct uacpi_interrupt_handler {
    interrupt_handler_t handler;
    uacpi_interrupt_handler func;
    uacpi_handle ctx;
} uacpi_interrupt_handler_t;

static void uacpi_interrupt_wrapper(interrupt_handler_t* _ctx) {
    uacpi_interrupt_handler_t* handler = containerof(_ctx, uacpi_interrupt_handler_t, handler);
    handler->func(handler->ctx);
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx, uacpi_handle* out_irq_handle) {
    err_t err = NO_ERROR;

    uacpi_interrupt_handler_t* irq_handler = mem_alloc(sizeof(*irq_handler));
    CHECK_ERROR(irq_handler != NULL, UACPI_STATUS_OUT_OF_MEMORY);

    // setup the struct correctly
    irq_handler->handler.handler = uacpi_interrupt_wrapper;
    irq_handler->func = handler;
    irq_handler->ctx = ctx;

    // allocate the irq vector
    RETHROW(irq_allocate(&irq_handler->handler));

    // map the source to the irq
    ioapic_irq_t gsi = acpi_convert_isa_to_gsi(irq);
    RETHROW(ioapic_configure_irq(&gsi, irq_handler->handler.vector, 0));
    RETHROW(ioapic_enable_irq(gsi.irq, true));

cleanup:
    return err.status;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    TRACE("TODO: uacpi_kernel_uninstall_interrupt_handler");
    return UACPI_STATUS_UNIMPLEMENTED;
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
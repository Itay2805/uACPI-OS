#include <limine_requests.h>

#include "limine.h"
#include "debug/log.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "lib/except.h"
#include "acpi/acpi.h"
#include "arch/intrin.h"
#include "sync/spinlock.h"
#include "mem/phys.h"
#include "mem/virt.h"
#include "arch/regs.h"
#include "mem/alloc.h"

#include <stddef.h>
#include <stdatomic.h>
#include <arch/apic.h>
#include <debug/debug.h>
#include <lib/string.h>
#include <thread/pcpu.h>
#include <time/tsc.h>

#include "drivers/acpi-ec.h"
#include "thread/scheduler.h"
#include "uacpi/event.h"
#include "uacpi/utilities.h"

/**
 * The init thread
 */
static thread_t* m_init_thread;

static uacpi_interrupt_ret acpi_on_fixed_event(uacpi_handle ctx) {
    const char* fixed_event = "<unknown>";
    switch ((uintptr_t)ctx) {
        case UACPI_FIXED_EVENT_TIMER_STATUS: fixed_event = "Timer"; break;
        case UACPI_FIXED_EVENT_POWER_BUTTON: fixed_event = "Power button"; break;
        case UACPI_FIXED_EVENT_SLEEP_BUTTON: fixed_event = "Sleep button"; break;
        case UACPI_FIXED_EVENT_RTC: fixed_event = "RTC"; break;
    }
    TRACE("Got fixed event: %s", fixed_event);
    return UACPI_INTERRUPT_HANDLED;
}

static void init_thread_entry(void* arg) {
     err_t err = NO_ERROR;

     TRACE("Init thread started");

    // Load the AML namespace
    CHECK_UACPI(uacpi_namespace_load());

    // we are using IOAPIC interrupt mode
    CHECK_UACPI(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC));

    // Perform early initialization of the EC
    RETHROW(init_early_ec());

    // Initialize the namespace
    CHECK_UACPI(uacpi_namespace_initialize());

    // And finish finalizing the EC now that the namespace is loaded
    RETHROW(init_ec());

    // TODO: setup gpes and stuff

    // Finalize the wakeup sources
    CHECK_UACPI(uacpi_finalize_gpe_initialization());

    // the rest we want
    CHECK_UACPI(uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, acpi_on_fixed_event, (uacpi_handle)UACPI_FIXED_EVENT_POWER_BUTTON));

cleanup:
     if (IS_ERROR(err)) {
         ERROR("Can't continue loading the OS");
     }
     (void)err;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Early startup
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * For waiting until all cpus are finished initializing
 */
static atomic_size_t m_smp_count = 0;

/**
 * If we get any failure then we will mark it
 */
static atomic_bool m_smp_fail = false;

static void set_cpu_features() {
    // PG/PE - required for long mode
    // MP - required for SSE
    __writecr0(CR0_PG | CR0_PE | CR0_MP);

    // PAE - required for long mode
    // OSFXSR/OSXMMEXCPT - required for SSE
    __writecr4(CR4_PAE | CR4_OSFXSR | CR4_OSXSAVE | CR4_OSXMMEXCPT);
}

static void halt() {
    asm("cli");
    for (;;) {
        asm("hlt");
    }
}

static void smp_entry(struct limine_mp_info* info) {
    err_t err = NO_ERROR;

    TRACE("smp: \tCPU#%ld - LAPIC#%d", info->extra_argument, info->lapic_id);

    //
    // Start by setting the proper CPU context
    //
    init_gdt();
    init_idt();
    set_cpu_features();
    switch_page_table();

    //
    // And now setup the per-cpu
    //
    pcpu_init_per_core(info->extra_argument);
    init_phys_per_cpu();
    RETHROW(init_tss());

    // and now we can init
    init_lapic_per_core();
    RETHROW(scheduler_init_per_core());

    // we are done
    m_smp_count++;

    // we can trigger the scheduler,
    scheduler_start_per_core();

cleanup:
    // if we got an error mark it
    if (IS_ERROR(err)) {
        m_smp_fail = true;
        m_smp_count++;
    }
    halt();
}

void _start() {
    err_t err = NO_ERROR;

    // make early logging work
    init_early_logging();

    // Welcome!
    TRACE("------------------------------------------------------------------------------------------------------------");
    TRACE("uACPI-OS");
    TRACE("------------------------------------------------------------------------------------------------------------");
    limine_check_revision();

    //
    // early cpu init, this will take care of having interrupts
    // and a valid GDT already
    //
    init_gdt();
    init_idt();

    //
    // setup the basic memory management
    //
    RETHROW(init_virt_early());
    RETHROW(init_phys());

    //
    // setup the per-cpu data of the current cpu
    //
    size_t cpu_count = 1;
    if (g_limine_mp_request.response != NULL) {
        cpu_count = g_limine_mp_request.response->cpu_count;

        // allocate all the storage needed
        RETHROW(pcpu_init(g_limine_mp_request.response->cpu_count));

        // now find the correct cpu id and set it
        bool found = false;
        for (int i = 0; i < g_limine_mp_request.response->cpu_count; i++) {
            if (g_limine_mp_request.response->cpus[i]->lapic_id == g_limine_mp_request.response->bsp_lapic_id) {
                pcpu_init_per_core(i);
                found = true;
                break;
            }
        }
        CHECK(found);
    } else {
        // no SMP startup available from bootloader,
        // just assume we have a single cpu
        WARN("smp: missing limine SMP support");
        RETHROW(pcpu_init(1));
        pcpu_init_per_core(0);
    }

    //
    // Continue with the rest of the initialization
    // now that we have a working pcpu data
    //
    init_phys_per_cpu();
    RETHROW(init_tss());
    RETHROW(init_virt());
    RETHROW(init_phys_mappings());
    set_cpu_features();
    switch_page_table();

    init_alloc();

    // load the debug symbols now that we have an allocator
    debug_load_symbols();

    // we need acpi for some early sleep primitives
    RETHROW(init_acpi());

    // we need to calibrate the timer now
    init_tsc();

    // setup the scheduler
    // do that before the SMP startup so it can requests
    // tasks right away
    RETHROW(scheduler_init(cpu_count));

    // perform cpu startup
    CHECK(g_limine_mp_request.response != NULL);
    struct limine_mp_response* response = g_limine_mp_request.response;

    RETHROW(init_lapic());

    TRACE("smp: Starting CPUs (%zd)", cpu_count);

    for (size_t i = 0; i < cpu_count; i++) {
        if (response->cpus[i]->lapic_id == response->bsp_lapic_id) {
            TRACE("smp: \tCPU#%ld - LAPIC#%d (BSP)", i, response->cpus[i]->lapic_id);

            // allocate the per-cpu storage now that we know our id
            RETHROW(scheduler_init_per_core());

            m_smp_count++;
        } else {
            // start it up
            response->cpus[i]->extra_argument = i;
            response->cpus[i]->goto_address = smp_entry;

            while (m_smp_count != i + 1) {
                cpu_relax();
            }
        }
    }

    // wait for smp to finish up
    // TODO: timeout?
    while (m_smp_count != cpu_count) {
        cpu_relax();
    }
    TRACE("smp: Finished SMP startup");

    // we are about done, create the init thread and queue it
    m_init_thread = thread_create(init_thread_entry, NULL, "init thread");
    scheduler_start_thread(m_init_thread);

    // and we are ready to start the scheduler
    scheduler_start_per_core();

cleanup:
    halt();
}

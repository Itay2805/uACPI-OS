#include "apic.h"

#include <limine_requests.h>
#include <stdbool.h>
#include <stdint.h>
#include <lib/except.h>
#include <mem/memory.h>
#include <mem/virt.h>
#include <time/tsc.h>

#include "intrin.h"
#include "acpi/acpi.h"
#include "mem/alloc.h"
#include "sync/spinlock.h"
#include "thread/intr.h"
#include "thread/pcpu.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LAPIC driver
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define XAPIC_ID_OFFSET                          0x20
#define XAPIC_VERSION_OFFSET                     0x30
#define XAPIC_EOI_OFFSET                         0x0b0
#define XAPIC_ICR_DFR_OFFSET                     0x0e0
#define XAPIC_SPURIOUS_VECTOR_OFFSET             0x0f0
#define XAPIC_ICR_LOW_OFFSET                     0x300
#define XAPIC_ICR_HIGH_OFFSET                    0x310
#define XAPIC_LVT_TIMER_OFFSET                   0x320
#define XAPIC_LVT_LINT0_OFFSET                   0x350
#define XAPIC_LVT_LINT1_OFFSET                   0x360
#define XAPIC_TIMER_INIT_COUNT_OFFSET            0x380
#define XAPIC_TIMER_CURRENT_COUNT_OFFSET         0x390
#define XAPIC_TIMER_DIVIDE_CONFIGURATION_OFFSET  0x3E0

#define X2APIC_MSR_BASE_ADDRESS  0x800
#define X2APIC_MSR_ICR_ADDRESS   0x830

#define LOCAL_APIC_DELIVERY_MODE_FIXED            0
#define LOCAL_APIC_DELIVERY_MODE_LOWEST_PRIORITY  1
#define LOCAL_APIC_DELIVERY_MODE_SMI              2
#define LOCAL_APIC_DELIVERY_MODE_NMI              4
#define LOCAL_APIC_DELIVERY_MODE_INIT             5
#define LOCAL_APIC_DELIVERY_MODE_STARTUP          6
#define LOCAL_APIC_DELIVERY_MODE_EXTINT           7

#define LOCAL_APIC_DESTINATION_SHORTHAND_NO_SHORTHAND        0
#define LOCAL_APIC_DESTINATION_SHORTHAND_SELF                1
#define LOCAL_APIC_DESTINATION_SHORTHAND_ALL_INCLUDING_SELF  2
#define LOCAL_APIC_DESTINATION_SHORTHAND_ALL_EXCLUDING_SELF  3

typedef union {
    struct {
        uint32_t SpuriousVector          : 8;  ///< Spurious Vector.
        uint32_t SoftwareEnable          : 1;  ///< APIC Software Enable/Disable.
        uint32_t FocusProcessorChecking  : 1;  ///< Focus Processor Checking.
        uint32_t Reserved0               : 2;  ///< Reserved.
        uint32_t EoiBroadcastSuppression : 1;  ///< EOI-Broadcast Suppression.
        uint32_t Reserved1               : 19; ///< Reserved.
    };
    uint32_t packed;
} LOCAL_APIC_SVR;

typedef union {
    struct {
        uint32_t divide_value_1 : 2;
        uint32_t : 1;
        uint32_t divide_value_2 : 1;
        uint32_t : 28;
    };
    uint32_t packed;
} LOCAL_APIC_DCR;

typedef union {
    struct {
        uint32_t vector : 8;
        uint32_t delivery_mode : 3;
        uint32_t : 1;
        uint32_t delivery_status : 1;
        uint32_t input_pin_polarity : 1;
        uint32_t remote_irr : 1;
        uint32_t trigger_mode : 1;
        uint32_t mask : 1;
        uint32_t : 15;
    };
    uint32_t packed;
} LOCAL_APIC_LVT_LINT;

typedef union {
    struct {
        uint32_t vector : 8;
        uint32_t : 4;
        uint32_t delivery_status : 1;
        uint32_t : 3;
        uint32_t mask : 1;
        uint32_t timer_mode : 2;
        uint32_t : 13;
    };
    uint32_t packed;
} LOCAL_APIC_LVT_TIMER;

/**
 * Are we using x2APIC mode
 */
static bool m_x2apic_mode = false;

/**
 * The xAPIC base, when using xAPIC mode
 */
static uint8_t* m_xapic_base = NULL;

/**
 * The frequency of the lapic timer
 */
static uint64_t m_lapic_timer_freq = 0;

static void lapic_write(size_t offset, uint32_t value) {
    if (m_x2apic_mode) {
        asm("" ::: "memory");
        __wrmsr((offset >> 4) + X2APIC_MSR_BASE_ADDRESS, value);
    } else {
        *((uint32_t*)(m_xapic_base + offset)) = value;
    }
}

static uint32_t lapic_read(size_t offset) {
    if (m_x2apic_mode) {
        return __rdmsr((offset >> 4) + X2APIC_MSR_BASE_ADDRESS);
    } else {
        return *((uint32_t*)(m_xapic_base + offset));
    }
}

static uint32_t calculate_lapic_freq() {
    // set the counter to FFs
    lapic_write(XAPIC_TIMER_INIT_COUNT_OFFSET, UINT32_MAX);

    // start the timer
    uint32_t ticks = acpi_get_timer_tick() + 363;
    while (((ticks - acpi_get_timer_tick()) & BIT23) == 0);
    uint32_t end_ticks = lapic_read(XAPIC_TIMER_CURRENT_COUNT_OFFSET);

    // and clear the timer
    lapic_timer_clear();

    return (UINT32_MAX - end_ticks) * 9861;
}

err_t init_lapic(void) {
    err_t err = NO_ERROR;

    // we are going to use 0xFF as the spurious interrupt vector
    ASSERT(!IS_ERROR(irq_reserve(0xFF)));

    // we are going to use 0x20 as the timer handler
    ASSERT(!IS_ERROR(irq_reserve(0x20)));

    // check the apic state
    MSR_IA32_APIC_BASE_REGISTER apic_base = { .packed = __rdmsr(MSR_IA32_APIC_BASE) };
    CHECK(apic_base.en);
    if (apic_base.extd) {
        m_x2apic_mode = true;
        TRACE("apic: using x2apic");

    } else {
        m_x2apic_mode = false;
        TRACE("apic: using xapic");

        // calculate the address
        m_xapic_base = PHYS_TO_DIRECT(apic_base.apic_base << 12);

        // make sure the apic is mapped properly
        RETHROW(virt_map_page(apic_base.apic_base << 12, (uintptr_t)m_xapic_base, MAP_PERM_W));
    }

    // perform the per-core init
    init_lapic_per_core();

    // calculate the frequency of the lapic
    m_lapic_timer_freq = calculate_lapic_freq();

cleanup:
    return err;
}

void init_lapic_per_core(void) {
    // set the spurious vector
    LOCAL_APIC_SVR svr = {
        .SpuriousVector = 0xFF,
        .SoftwareEnable = 1
    };
    lapic_write(XAPIC_SPURIOUS_VECTOR_OFFSET, svr.packed);

    // divide by 1, aka I don't want any division
    LOCAL_APIC_DCR dcr = {
        .divide_value_1 = 0b11,
        .divide_value_2 = 0b01,
    };
    lapic_write(XAPIC_TIMER_DIVIDE_CONFIGURATION_OFFSET, dcr.packed);

    // ensure the timer is clear
    lapic_timer_clear();

    // enable the lapic timer properly
    LOCAL_APIC_LVT_TIMER timer = {
        .vector = 0x20,
        .mask = 0,
        .timer_mode = 0
    };
    lapic_write(XAPIC_LVT_TIMER_OFFSET, timer.packed);
}

void lapic_eoi(void) {
    lapic_write(XAPIC_EOI_OFFSET, 0);
}

void lapic_timer_set_timeout(uint64_t ms_timeout) {
    // calculate the amount of ticks we need to set, if too much then
    // just truncate, its up to the timer subsystem to be able to handle
    // it
    uint64_t timer_count = (ms_timeout * m_lapic_timer_freq) / MS_PER_S;
    if (timer_count > UINT32_MAX) {
        timer_count = 429496730;
    }

    // set the count
    lapic_write(XAPIC_TIMER_INIT_COUNT_OFFSET, timer_count);
}

void lapic_timer_clear(void) {
    lapic_write(XAPIC_TIMER_INIT_COUNT_OFFSET, 0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// IOAPIC driver
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define IOAPIC_INDEX_OFFSET 0x00
#define IOAPIC_DATA_OFFSET  0x10

#define IOAPIC_IDENTIFICATION_REGISTER_INDEX 0x00
#define IOAPIC_VERSION_REGISTER_INDEX 0x01
#define IOAPIC_REDIRECTION_TABLE_ENTRY_INDEX 0x10

typedef union IOAPIC_VERSION_REGISTER {
    struct {
        uint8_t version;
        uint8_t : 8;
        uint8_t maximum_redirection_entry;
        uint8_t : 8;
    };
    uint32_t packed;
} PACKED IOAPIC_VERSION_REGISTER;

typedef union IOAPIC_REDIRECTION_TABLE_ENTRY {
    struct {
        uint64_t vector : 8;
        uint64_t delivery_mode : 3;
        uint64_t destination_mode : 1;
        uint64_t delivery_status : 1;
        uint64_t polarity : 1;
        uint64_t remote_irr : 1;
        uint64_t trigger_mode : 1;
        uint64_t mask : 1;
        uint64_t : 39;
        uint64_t destination_id : 8;
    };
    struct {
        uint32_t packed_low;
        uint32_t packed_high;
    };
} PACKED IOAPIC_REDIRECTION_TABLE_ENTRY;
STATIC_ASSERT(sizeof(IOAPIC_REDIRECTION_TABLE_ENTRY) == sizeof(uint64_t));

typedef struct ioapic {
    uint32_t gsi_start;
    uint32_t gsi_end;
    void* address;
    irq_spinlock_t irq;
} ioapic_t;

static ioapic_t* m_ioapics = NULL;
static size_t m_ioapic_count = 0;

static uint32_t ioapic_read(ioapic_t* ioapic, uint8_t offset) {
    *(volatile uint8_t*)(ioapic->address + IOAPIC_INDEX_OFFSET) = offset;
    uint32_t data = *(volatile uint32_t*)(ioapic->address + IOAPIC_DATA_OFFSET);
    return data;
}

static void ioapic_write(ioapic_t* ioapic, uint8_t offset, uint32_t value) {
    *(volatile uint8_t*)(ioapic->address + IOAPIC_INDEX_OFFSET) = offset;
    *(volatile uint32_t*)(ioapic->address + IOAPIC_DATA_OFFSET) = value;
}

err_t ioapic_add(uint64_t base_address, uint32_t gsi_base) {
    err_t err = NO_ERROR;

    m_ioapic_count++;
    m_ioapics = mem_realloc(m_ioapics, m_ioapic_count * sizeof(ioapic_t));
    ioapic_t* ioapic = &m_ioapics[m_ioapic_count - 1];
    ioapic->address = PHYS_TO_DIRECT(base_address);

    IOAPIC_VERSION_REGISTER version = { .packed = ioapic_read(ioapic, IOAPIC_VERSION_REGISTER_INDEX) };
    CHECK(version.maximum_redirection_entry <= 0xF0);

    ioapic->gsi_start = gsi_base;
    ioapic->gsi_end = gsi_base + version.maximum_redirection_entry;

    TRACE("IOAPIC[%ld]: apic_id %d, version %d, address 0x%lx, GSI %d-%d",
        m_ioapic_count - 1,
        (ioapic_read(ioapic, IOAPIC_IDENTIFICATION_REGISTER_INDEX) >> 24) & 0xF,
        version.version, DIRECT_TO_PHYS(ioapic->address), ioapic->gsi_start, ioapic->gsi_end
    );

cleanup:
    return err;
}

static ioapic_t* get_ioapic(uint32_t gsi) {
    for (int i = 0; i < m_ioapic_count; i++) {
        ioapic_t* ioapic = &m_ioapics[i];
        if (ioapic->gsi_start <= gsi && gsi < ioapic->gsi_end) {
            return ioapic;
        }
    }
    return NULL;
}

err_t ioapic_enable_irq(uint32_t irq, bool enable) {
    err_t err = NO_ERROR;
    bool locked = false;

    ioapic_t* ioapic = get_ioapic(irq);
    CHECK(ioapic != NULL);

    bool status = irq_spinlock_lock(&ioapic->irq);
    locked = true;

    IOAPIC_REDIRECTION_TABLE_ENTRY entry = { .packed_low = ioapic_read(ioapic, IOAPIC_REDIRECTION_TABLE_ENTRY_INDEX + irq * 2) };
    entry.mask = enable ? 0 : 1;
    ioapic_write(ioapic, IOAPIC_REDIRECTION_TABLE_ENTRY_INDEX + irq * 2, entry.packed_low);

cleanup:
    if (locked) {
        irq_spinlock_unlock(&ioapic->irq, status);
    }

    return err;
}

err_t ioapic_configure_irq(ioapic_irq_t* irq, uint8_t vector, uint8_t cpu) {
    err_t err = NO_ERROR;
    bool locked = false;

    ioapic_t* ioapic = get_ioapic(irq->irq);
    CHECK(ioapic != NULL);

    bool status = irq_spinlock_lock(&ioapic->irq);
    locked = true;

    IOAPIC_REDIRECTION_TABLE_ENTRY entry = {
        // physical cpu at the given id
        .destination_mode = 0,
        .destination_id = cpu,

        // fixed delivery
        .delivery_mode = IOAPIC_DELIVERY_MODE_FIXED,
        .vector = vector,

        // the line config
        .polarity = irq->assertion_level ? 0 : 1,
        .trigger_mode = irq->level_triggered ? 1 : 0,

        // masked or not
        .mask = true,
    };

    ioapic_write(ioapic, IOAPIC_REDIRECTION_TABLE_ENTRY_INDEX + irq->irq * 2, entry.packed_low);
    ioapic_write(ioapic, IOAPIC_REDIRECTION_TABLE_ENTRY_INDEX + irq->irq * 2 + 1, entry.packed_high);

cleanup:
    if (locked) {
        irq_spinlock_unlock(&ioapic->irq, status);
    }

    return err;
}

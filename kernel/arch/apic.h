#pragma once

#include <stdint.h>
#include <lib/except.h>

/**
 * Initialize the APIC globally
 */
err_t init_lapic(void);

/**
 * Initialize the APIC per core
 */
void init_lapic_per_core(void);

/**
 * Request an EOI signal to be sent
 */
void lapic_eoi(void);

void lapic_timer_set_timeout(uint64_t ms_timeout);
void lapic_timer_clear(void);

typedef enum ioapic_delivery_mode {
    IOAPIC_DELIVERY_MODE_FIXED = 0,
    IOAPIC_DELIVERY_MODE_LOWEST_PRIORITY = 1,
    IOAPIC_DELIVERY_MODE_SMI = 2,
    IOAPIC_DELIVERY_MODE_NMI = 4,
    IOAPIC_DELIVERY_MODE_INIT = 5,
    IOAPIC_DELIVERY_MODE_EXTINIT = 7,
} ioapic_delivery_mode_t;

typedef struct ioapic_irq {
    uint32_t irq;
    bool level_triggered;
    bool assertion_level;
} ioapic_irq_t;

/**
 * Add an ioapic to the ioapic driver
 */
err_t ioapic_add(uint64_t base_address, uint32_t gsi_base);

/**
 * Either enable or disable the given irq
 *
 * @param irq       [IN] The irq to handle
 * @param enable    [IN] Should we mask or unmask it
 */
err_t ioapic_enable_irq(uint32_t irq, bool enable);

/**
 * Configure the given ioapic interrupt
 *
 * @param entry     [IN] The interrupt configuration
 * @param vector    [IN] The vector to which we should redirect
 * @param cpu       [IN] The cpu to redirect to
 * @param mask      [IN] Start as masked or not
 */
err_t ioapic_configure_irq(ioapic_irq_t* entry, uint8_t vector, uint8_t cpu);

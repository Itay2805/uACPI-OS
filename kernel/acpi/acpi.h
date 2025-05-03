#pragma once

#include <stdint.h>

#include "arch/apic.h"
#include "lib/except.h"
#include "uacpi/resources.h"

#define CHECK_UACPI(status) \
    do { \
        uacpi_status __status = status; \
        CHECK_ERROR(__status == UACPI_STATUS_OK, __status); \
    } while (0)

/**
 * Initialize the early acpi subsystem, should just be enough for
 * doing whatever we need to do
 */
err_t early_init_acpi(void);

/**
 * Finalize the ACPI initialzation
 */
err_t init_acpi(void);

uint32_t acpi_get_timer_tick(void);

/**
 * Stall for the given amount of NS
 */
void acpi_stall(uint64_t ns);

/**
 * Convert an ISA IRQ into a GSI value.
 *
 * @param isa_irq                   [IN] The ISA interrupt we want to convert
 * @param default_assertion_level   [IN] If no override was found, what level do you want (true == level-triggered, false == edge-triggered)
 * @param default_level_triggered   [IN] If no override was found, what polarity do you want (true == active-high, false == active-low)
 */
ioapic_irq_t acpi_convert_isa_to_gsi(uint8_t isa_irq, bool default_level_triggered, bool default_assertion_level);

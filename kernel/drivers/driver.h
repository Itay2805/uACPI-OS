#pragma once
#include "lib/except.h"
#include "uacpi/utilities.h"

typedef struct acpi_driver {
    /**
     * The name of the driver, for debugging
     */
    const char* name;

    /**
     * The signature for looking up the acpi node
     */
    const char* acpi_signature;

    /**
     * Initialize an ACPI device that matched the description
     */
    err_t (*init)(uacpi_namespace_node* node, uacpi_namespace_node_info* info, const uacpi_char* abs_path, uint32_t sta_flags);
} acpi_driver_t;

#define ACPI_DRIVER(driver_name, acpi_name, init_function) \
    __attribute__((aligned(1), used, section("acpi_drivers"))) \
    static acpi_driver_t m_acpi_driver = { \
        .name = driver_name, \
        .acpi_signature = acpi_name, \
        .init = init_function \
    }

extern acpi_driver_t __start_acpi_drivers[];
extern acpi_driver_t __stop_acpi_drivers[];

/**
 * Initialize all acpi drivers, must be called after acpi init
 */
err_t init_acpi_drivers(void);

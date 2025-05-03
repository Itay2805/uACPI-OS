#include <drivers/driver.h>

#include "acpi/acpi.h"
#include "uacpi/notify.h"

static uacpi_status acpi_lid_notification(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
    err_t err = NO_ERROR;

    if ((value & 0x80) != 0) {
        uint64_t value = 0;
        CHECK_UACPI(uacpi_eval_simple_integer(node, "_LID", &value));
        TRACE("Lid state changed! current state - %s", value == 0 ? "closed" : "open");
    }

cleanup:
    return err.status;
}


static err_t acpi_lid_init(uacpi_namespace_node* node, uacpi_namespace_node_info* info, const uacpi_char* abs_path, uint32_t sta_flags) {
    err_t err = NO_ERROR;

    // just install a notification handler, nothing else to do really
    CHECK_UACPI(uacpi_install_notify_handler(node, acpi_lid_notification, NULL));

cleanup:
    return err;
}

ACPI_DRIVER("acpi-lid", "PNP0C0D", acpi_lid_init);

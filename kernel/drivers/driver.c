#include "driver.h"

#include "acpi/acpi.h"
#include "lib/string.h"
#include "sync/spinlock.h"
#include "uacpi/utilities.h"

static uacpi_iteration_decision acpi_init_one_device(void* ctx, uacpi_namespace_node* node, uacpi_u32 node_depth) {
    err_t err = NO_ERROR;
    uacpi_namespace_node_info* info = NULL;
    const uacpi_char* path = NULL;

    // check that the device is actually active
    uint32_t flags;
    CHECK_UACPI(uacpi_eval_sta(node, &flags));

    // if device is not present continue
    if ((flags & ACPI_STA_RESULT_DEVICE_PRESENT) == 0) {
        goto cleanup;
    }

    // If device is not enabled continue
    // TODO: maybe we can allow a driver to ignore this check when it can enable it on its own
    //       but for now and for simplicity ignore the device
    if ((flags & ACPI_STA_RESULT_DEVICE_PRESENT) == 0) {
        path = uacpi_namespace_node_generate_absolute_path(node);
        WARN("\t%s not enabled, ignoring", path);
        goto cleanup;
    }

    // get the node info
    CHECK_UACPI(uacpi_get_namespace_node_info(node, &info));

    acpi_driver_t* matched_driver = NULL;

    // match against HID
    if (info->flags & UACPI_NS_NODE_INFO_HAS_HID) {
        for (acpi_driver_t* driver = __start_acpi_drivers; driver < __stop_acpi_drivers; ++driver) {
            if (strcmp(driver->acpi_signature, info->hid.value) == 0) {
                matched_driver = driver;
                break;
            }
        }
    }

    if (matched_driver == NULL && (info->flags & UACPI_NS_NODE_INFO_HAS_CID)) {
        for (acpi_driver_t* driver = __start_acpi_drivers; driver < __stop_acpi_drivers; ++driver) {

            // match against CIDs
            for (int i = 0; i < info->cid.num_ids; i++) {
                if (strcmp(driver->acpi_signature, info->cid.ids[i].value) == 0) {
                    matched_driver = driver;
                    break;
                }
            }

            // found match, break early
            if (matched_driver != NULL) {
                break;
            }
        }
    }

    // found driver! initialize it
    if (matched_driver != NULL) {
        path = uacpi_namespace_node_generate_absolute_path(node);
        TRACE("\t%s connected to %s", path, matched_driver->name);

        if ((flags & ACPI_STA_RESULT_DEVICE_FUNCTIONING) == 0) {
            WARN("\t\tdevice failed its diagnostics");
        }

        // and call initialization
        matched_driver->init(node, info, path, flags);
    }

cleanup:
    if (info != NULL) uacpi_free_namespace_node_info(info);
    if (path != NULL) uacpi_free_absolute_path(path);

    return UACPI_ITERATION_DECISION_CONTINUE;
}

err_t init_acpi_drivers(void) {
    err_t err = NO_ERROR;

    TRACE("Dispatching ACPI drivers:");
    CHECK_UACPI(uacpi_namespace_for_each_child(
        uacpi_namespace_root(),
        acpi_init_one_device,
        UACPI_NULL,
        UACPI_OBJECT_DEVICE_BIT,
        UACPI_MAX_DEPTH_ANY,
        UACPI_NULL
    ));

cleanup:
    return err;
}

#include "driver.h"

#include "acpi/acpi.h"
#include "lib/string.h"
#include "sync/spinlock.h"
#include "uacpi/utilities.h"

static uacpi_iteration_decision acpi_init_one_device(void* ctx, uacpi_namespace_node* node, uacpi_u32 node_depth) {
    err_t err = NO_ERROR;
    uacpi_namespace_node_info* info = NULL;
    uacpi_id_string* id_string = NULL;
    uacpi_pnp_id_list* id_list = NULL;
    const uacpi_char* path = NULL;

    // get the node info
    CHECK_UACPI(uacpi_get_namespace_node_info(node, &info));

    acpi_driver_t* matched_driver = NULL;

    // match against HID
    if (info->flags & UACPI_NS_NODE_INFO_HAS_HID) {
        CHECK_UACPI(uacpi_eval_hid(node, &id_string));
        for (acpi_driver_t* driver = __start_acpi_drivers; driver < __stop_acpi_drivers; ++driver) {
            if (strcmp(driver->acpi_signature, id_string->value) == 0) {
                matched_driver = driver;
                break;
            }
        }
    }


    if (matched_driver == NULL && (info->flags & UACPI_NS_NODE_INFO_HAS_CID)) {
        CHECK_UACPI(uacpi_eval_cid(node, &id_list));

        for (acpi_driver_t* driver = __start_acpi_drivers; driver < __stop_acpi_drivers; ++driver) {

            // match against CIDs
            for (int i = 0; i < id_list->num_ids; i++) {
                if (strcmp(driver->acpi_signature, id_list->ids[i].value) == 0) {
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
        TRACE("\t%s matched against %s", matched_driver->name, path);

        // and call initialization
        matched_driver->init(node, path);
    }

cleanup:
    if (info != NULL) uacpi_free_namespace_node_info(info);
    if (id_string != NULL) uacpi_free_id_string(id_string);
    if (id_list != NULL) uacpi_free_pnp_id_list(id_list);
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

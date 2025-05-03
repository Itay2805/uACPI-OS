#include "acpi-ec.h"

#include <sys/types.h>

#include "drivers/driver.h"
#include "acpi/acpi.h"
#include "mem/alloc.h"
#include "sync/spinlock.h"
#include "uacpi/event.h"
#include "uacpi/opregion.h"
#include "uacpi/tables.h"
#include "uacpi/internal/io.h"
#include "uacpi/internal/namespace.h"
#include "acpi-ec-internal.h"
#include "arch/intrin.h"
#include "sync/cond.h"
#include "sync/mutex.h"
#include "thread/scheduler.h"
#include "thread/thread.h"
#include "uacpi/resources.h"
#include "uacpi/utilities.h"

typedef struct acpi_ec {
    /**
     * The name of the EC
     */
    const char* name;

    /**
     * Thread to handle the GPE events for the EC
     */
    thread_t* thread;

    /**
     * The node of this EC
     */
    uacpi_namespace_node* node;

    /**
     * The GPE device used to trigger events, if any
     */
    uacpi_namespace_node* gpe_device;

    /**
     * The control/data resources
     * TODO: in theory this can be mmio, but for now we will just do io since its simpelr
     */
    uint16_t control_port;
    uint16_t data_port;

    /**
     * The GPE bit this triggers
     */
    uint8_t gpe_bit;

    /**
     * Is the global lock needed?
     */
    bool global_lock;

    /**
     * Mutex for accessing the controller, we only ever
     * access the controller in a thread, so its fine
     */
    mutex_t lock;

    /**
     * Synchronize between the gpe
     * handler and the thread
     */
    semaphore_t semaphore;

    /**
     * The thread that owns the lock
     */
    thread_t* lock_owner;

    /**
     * The depth of the lock
     */
    size_t lock_depth;
} acpi_ec_t;

/**
 * The EC set by the ECDT
 */
static acpi_ec_t* m_boot_ec = NULL;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// EC access helpers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void acpi_ec_poll(acpi_ec_t* ec, uint8_t want, uint8_t dont_want) {
    uint8_t status = __inbyte(ec->control_port);
    while ((status & want) != want || (status & dont_want) != 0) {
        cpu_relax();
        status = __inbyte(ec->control_port);
    }
}

static void acpi_ec_write_command(acpi_ec_t* ec, uint8_t value) {
    acpi_ec_poll(ec, 0, IBF);
    __outbyte(ec->control_port, value);

    // wait 1us before we continue to ensure
    // that the IBF is set
    uacpi_kernel_stall(1);
}

static void acpi_ec_write_data(acpi_ec_t* ec, uint8_t value) {
    acpi_ec_poll(ec, 0, IBF);
    __outbyte(ec->data_port, value);

    // wait 1us before we continue to ensure
    // that the IBF is set before we continue
    uacpi_kernel_stall(1);
}

static uint8_t acpi_ec_read_data(acpi_ec_t* ec) {
    acpi_ec_poll(ec, OBF, 0);
    return __inbyte(ec->data_port);
}

static uint32_t acpi_ec_lock(acpi_ec_t* ec) {
    // recursive lock, don't lock twice
    thread_t* current = scheduler_get_current_thread();
    if (ec->lock_owner == current) {
        ec->lock_depth++;
        return 0;
    }

    // actually need to lock
    uint32_t glk_seq = 0;
    if (ec->global_lock) {
        uacpi_acquire_global_lock(0xFFFF, &glk_seq);
    } else {
        mutex_lock(&ec->lock);
    }

    ec->lock_owner = current;

    return glk_seq;
}

static void acpi_ec_unlock(acpi_ec_t* ec, uint32_t seq) {
    ASSERT(ec->lock_owner == scheduler_get_current_thread());
    if (--ec->lock_depth == 0) {
        ec->lock_owner = NULL;
        if (ec->global_lock) {
            uacpi_release_global_lock(seq);
        } else {
            mutex_unlock(&ec->lock);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GPE handling
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uacpi_status acpi_ec_op_handler(uacpi_region_op op, uacpi_handle op_data) {
    switch (op) {
        case UACPI_REGION_OP_ATTACH: {
            // nothing to do
        } break;

        case UACPI_REGION_OP_DETACH: {
            // nothing to do
        } break;

        case UACPI_REGION_OP_READ: {
            uacpi_region_rw_data* data = op_data;
            acpi_ec_t* ec = data->handler_context;

            // can only read one byte at a time from the EC
            if (data->byte_width != 1 || data->offset > 0xFF) {
                return UACPI_STATUS_INVALID_ARGUMENT;
            }

            // perform the read
            uint32_t seq = acpi_ec_lock(ec);
            acpi_ec_write_command(ec, RD_EC);
            acpi_ec_write_data(ec,  data->offset);
            data->value = acpi_ec_read_data(ec);
            acpi_ec_unlock(ec, seq);
        } break;

        case UACPI_REGION_OP_WRITE: {
            uacpi_region_rw_data* data = op_data;
            acpi_ec_t* ec = data->handler_context;

            // can only read one byte at a time from the EC
            if (data->byte_width != 1 || data->offset > 0xFF || data->value > 0xFF) {
                return UACPI_STATUS_INVALID_ARGUMENT;
            }

            // perform the read
            uint32_t seq = acpi_ec_lock(ec);
            acpi_ec_write_command(ec, WD_EC);
            acpi_ec_write_data(ec, data->offset);
            acpi_ec_write_data(ec, data->value);
            acpi_ec_unlock(ec, seq);
        } break;

        default: {
            ERROR("UNKNOWN OP %d", op);
        } return UACPI_STATUS_UNIMPLEMENTED;
    }
    return UACPI_STATUS_OK;
}

static uacpi_interrupt_ret acpi_ec_gpe_handler(uacpi_handle ctx, uacpi_namespace_node* gpe_device, uacpi_u16 idx) {
    err_t err = NO_ERROR;
    acpi_ec_t* ec = ctx;

    // ensure that the device and idx matches our handler
    CHECK(ec->gpe_bit == idx);

    // get the status and check if what we need to do
    uint8_t status = __inbyte(ec->control_port);
    if (status & SCI_EVT) {
        // there is an event we need to handle, wakeup the thread to handle it
        semaphore_release(&ec->semaphore, false);

    } else {
        // nothing to do, finish handling right now
        CHECK_UACPI(uacpi_finish_handling_gpe(ec->gpe_device, ec->gpe_bit));
    }

cleanup:
    return IS_ERROR(err) ? UACPI_INTERRUPT_NOT_HANDLED : UACPI_INTERRUPT_HANDLED;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Query dispatcher
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void acpi_ec_worker(void *arg) {
    err_t err = NO_ERROR;
    acpi_ec_t* ec = arg;

    // Enable the GPE for the device
    CHECK_UACPI(uacpi_enable_gpe(
        ec->gpe_device,
        ec->gpe_bit
    ));

    while (true) {
        // wait for event
        semaphore_acquire(&ec->semaphore, false);

        uint32_t seq = acpi_ec_lock(ec);

        // run this until the SCI_EVT is cleared
        for (;;) {
            uint8_t status = __inbyte(ec->control_port);
            if ((status & SCI_EVT) == 0) {
                break;
            }

            // read the query result
            acpi_ec_write_command(ec, QR_EC);
            uint8_t value = acpi_ec_read_data(ec);

            // call the method
            char method_name[5] = "_QXX";
            method_name[2] = "0123456789ABCDEF"[(value >> 4) & 0xF];
            method_name[3] = "0123456789ABCDEF"[value & 0xF];
            uacpi_status ustatus = uacpi_eval(ec->node, method_name, NULL, NULL);
            if (uacpi_unlikely_error(ustatus)) {
                ERROR("acpi-ec: Failed to dispatch %s.%s: %s", ec->name, method_name, uacpi_status_to_string(ustatus));
            }
        }

        acpi_ec_unlock(ec, seq);

        // unmask the GPE, so more interrupts can come, at this point another interrupt
        // may fire and we will just handle it at the next loop
        CHECK_UACPI(uacpi_finish_handling_gpe(ec->gpe_device, ec->gpe_bit));
    }

cleanup:
    (void)err;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// EC initialization
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static err_t acpi_init_ec(acpi_ec_t* ec) {
    err_t err = NO_ERROR;

    // set the name, for fun and profit
    ec->name = uacpi_namespace_node_generate_absolute_path(ec->node);
    CHECK_ERROR(ec->name != NULL, UACPI_STATUS_OUT_OF_MEMORY);

    // TODO: print gpe device if any
    TRACE("\t\tglobal-lock: %s", ec->global_lock ? "yes" : "no");
    TRACE("\t\tgpe: %u", ec->gpe_bit);

    // create the handler thread
    ec->thread = thread_create(acpi_ec_worker, ec, "acpi-ec@%s", ec->name);
    CHECK_ERROR(ec != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    scheduler_start_thread(ec->thread);

    // apparently some firmwares do a funny and will use the operation region outside of the
    // EC node, to handle that we are going to forcefully install at the root node the handler
    // of either the ECDT or the uid 0 (aka the first EC), the rest are going to be scoped
    // correctly
    uacpi_namespace_node* install_node = (ec == m_boot_ec) ? uacpi_namespace_root() : ec->node;
    CHECK_UACPI(uacpi_install_address_space_handler(
        install_node,
        UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        acpi_ec_op_handler,
        ec
    ));

    // Set the GPE event handler so we can properly call the EC
    // TODO: gpe device when applicable
    CHECK_UACPI(uacpi_install_gpe_handler(
        ec->gpe_device,
        ec->gpe_bit,
        UACPI_GPE_TRIGGERING_EDGE,
        acpi_ec_gpe_handler,
        ec
    ));

cleanup:
    return err;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ECDT parsing
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static err_t init_from_ecdt(void) {
    err_t err = NO_ERROR;
    uacpi_table ecdt_table;
    bool unref_table = false;
    acpi_ec_t* ec = NULL;

    // get the table, if any
    // TODO: multiple EC tables? is it allowed?
    uacpi_status status = uacpi_table_find_by_signature(ACPI_ECDT_SIGNATURE, &ecdt_table);
    if (status == UACPI_STATUS_NOT_FOUND) {
        goto cleanup;
    }
    CHECK_UACPI(status);
    unref_table = true;

    // ensure the table is valid
    struct acpi_ecdt* ecdt = ecdt_table.ptr;
    CHECK(ecdt_table.hdr->length >= sizeof(*ecdt));

    // find the related node
    // TODO: do I need to unref this?
    uacpi_namespace_node* ec_node = NULL;
    CHECK_UACPI(uacpi_namespace_node_find(UACPI_NULL, ecdt->ec_id, &ec_node));

    // initialize the ec structure
    ec = mem_alloc(sizeof(*ec));
    CHECK_ERROR(ec != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    memset(ec, 0, sizeof(*ec));

    CHECK(ecdt->ec_control.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO);
    CHECK(ecdt->ec_data.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO);
    ec->control_port = ecdt->ec_control.address;
    ec->data_port = ecdt->ec_data.address;
    ec->node = ec_node;
    ec->gpe_bit = ecdt->gpe_bit;

    // initialize and start the EC
    RETHROW(acpi_init_ec(ec));

    TRACE("acpi-ec: Added boot EC@%s", ec->name);

cleanup:
    if (IS_ERROR(err)) {
        mem_free(ec);
    }

    if (unref_table) {
        uacpi_table_unref(&ecdt_table);
    }

    return err;
}

err_t init_early_ec() {
    err_t err = NO_ERROR;

    // perform boot EC initialization
    RETHROW(init_from_ecdt());

cleanup:
    return err;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// EC ACPI driver
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static err_t acpi_ec_init(uacpi_namespace_node* node, uacpi_namespace_node_info* info, const uacpi_char* abs_path, uint32_t sta_flags) {
    err_t err = NO_ERROR;
    uacpi_resources* resources = NULL;

    // ignore the boot EC
    if (m_boot_ec != NULL && node == m_boot_ec->node) {
        // we already have the device, but we
        // need to call the _REG for it
        CHECK_UACPI(uacpi_reg_all_opregions(m_boot_ec->node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER));
        goto cleanup;
    }

    acpi_ec_t* ec = mem_alloc(sizeof(*ec));
    CHECK_ERROR(ec != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    memset(ec, 0, sizeof(*ec));

    ec->node = node;

    // parse the resources
    CHECK_UACPI(uacpi_get_current_resources(node, &resources));

    // parse the data port
    uacpi_resource* resource = resources->entries;
    CHECK(resource->type == UACPI_RESOURCE_TYPE_IO);
    ec->data_port = resource->io.minimum;

    // parse the command port
    resource = UACPI_NEXT_RESOURCE(resource);
    CHECK(resource->type == UACPI_RESOURCE_TYPE_IO);
    ec->control_port = resource->io.minimum;

    // TODO: gpio interrupt for hardware-reduced acpi

    // get the gpe number
    // TODO: support package, support GPIO instead of GPE
    uint64_t gpe;
    CHECK_UACPI(uacpi_eval_simple_integer(ec->node, "_GPE", &gpe));
    ec->gpe_bit = gpe;

    // check if the global lock is required
    uint64_t glk = 0;
    uacpi_status status = uacpi_eval_simple_integer(ec->node, "_GLK", &glk);
    if (status == UACPI_STATUS_NOT_FOUND) status = UACPI_STATUS_OK;
    CHECK_UACPI(status);

    // and fully initialize it
    RETHROW(acpi_init_ec(ec));

cleanup:
    if (resources != NULL) {
        uacpi_free_resources(resources);
    }

    if (IS_ERROR(err)) {
        mem_free(ec);
    }

    return err;
}

ACPI_DRIVER("acpi-ec", "PNP0C09", acpi_ec_init);

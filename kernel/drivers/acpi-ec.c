#include "acpi-ec.h"

#include "acpi/acpi.h"
#include "mem/alloc.h"
#include "sync/spinlock.h"
#include "uacpi/event.h"
#include "uacpi/opregion.h"
#include "uacpi/tables.h"
#include "uacpi/internal/io.h"
#include "uacpi/internal/namespace.h"
#include "acpi-ec-internal.h"
#include "uacpi/resources.h"
#include "uacpi/utilities.h"

typedef struct acpi_ec {
    const char* name;
    uacpi_namespace_node* node;
    uacpi_mapped_gas control;
    uacpi_mapped_gas data;
    uint8_t gpe_bit;
    irq_spinlock_t lock;
} acpi_ec_t;

static bool acpi_ec_get_gpe_status(acpi_ec_t* ec) {
    uacpi_event_info event;
    ASSERT(!uacpi_unlikely_error(uacpi_gpe_info(ec->node, ec->gpe_bit, &event)));
    return (event & UACPI_EVENT_INFO_HW_STATUS) != 0;
}

static acpi_ec_t* m_boot_ec = NULL;

static uacpi_status acpi_ec_op_handler(uacpi_region_op op, uacpi_handle op_data) {
    switch (op) {
        default: {
            ERROR("UNKNOWN OP %d", op);
        } return UACPI_STATUS_UNIMPLEMENTED;
    }
}

static err_t acpi_ec_handle(acpi_ec_t* ec) {
    err_t err = NO_ERROR;

    // get the status and check if what we need to do
    uint64_t status;
    CHECK_UACPI(uacpi_gas_read_mapped(&ec->control, &status));

    // There is an event that AML needs to know about
    if (status & SCI_EVT) {
        // TODO: mask events
        // TODO: wakeup a thread to process the event
    }

cleanup:
    return err;
}

static uacpi_interrupt_ret acpi_ec_gpe_handler(uacpi_handle ctx, uacpi_namespace_node *gpe_device, uacpi_u16 idx) {
    err_t err = NO_ERROR;
    acpi_ec_t* ec = ctx;
    bool irq_status = irq_spinlock_lock(&ec->lock);

    // ensure that the device and idx matches our handler
    CHECK(ec->node == gpe_device);
    CHECK(ec->gpe_bit == idx);

    TRACE("acpi-ec: Got GPE from %s", ec->name);

    // clear the GPE status
    if (acpi_ec_get_gpe_status(ec)) {
        CHECK_UACPI(uacpi_clear_gpe(ec->node, ec->gpe_bit));
    }

    // handle the event
    RETHROW(acpi_ec_handle(ec));

cleanup:
    irq_spinlock_unlock(&ec->lock, irq_status);
    return IS_ERROR(err) ? UACPI_INTERRUPT_NOT_HANDLED : UACPI_INTERRUPT_HANDLED;
}

static err_t acpi_init_ec(acpi_ec_t* ec) {
    err_t err = NO_ERROR;

    // set the name, for fun and profit
    ec->name = uacpi_namespace_node_generate_absolute_path(ec->node);
    TRACE("acpi-ec: Found EC at %s", ec->name);

    // Install the address space handler for this EC
    CHECK_UACPI(uacpi_install_address_space_handler(
        ec->node,
        UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        acpi_ec_op_handler,
        ec
    ));

    // Set the GPE event handler so we can properly call the EC
    CHECK_UACPI(uacpi_install_gpe_handler(
        ec->node,
        ec->gpe_bit,
        UACPI_GPE_TRIGGERING_EDGE,
        acpi_ec_gpe_handler,
        ec
    ));

    // Enable the GPE for the device
    CHECK_UACPI(uacpi_enable_gpe(ec->node, ec->gpe_bit));

cleanup:
    return err;
}

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
    CHECK_UACPI(uacpi_map_gas_noalloc(&ecdt->ec_control, &ec->control));
    CHECK_UACPI(uacpi_map_gas_noalloc(&ecdt->ec_data, &ec->data));
    ec->node = ec_node;
    ec->gpe_bit = ecdt->gpe_bit;

    // initialize and start the EC
    RETHROW(acpi_init_ec(ec));

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

static err_t acpi_ec_create_gas(uacpi_resource* resource, uacpi_mapped_gas* mapped_gas) {
    err_t err = NO_ERROR;

    // convert to a gas structure, assumes 8bit regs like
    // the spec defines
    struct acpi_gas gas = {};
    switch (resource->type) {
        case UACPI_RESOURCE_TYPE_IO: {
            gas.address_space_id = UACPI_ADDRESS_SPACE_SYSTEM_IO;
            gas.address = resource->io.minimum;
            gas.access_size = 1;
            gas.register_bit_offset = 0;
            gas.register_bit_width = 8;
        } break;

        default:
            CHECK_FAIL("Unknown resource type %d", resource->type);
    }

    // and map it
    CHECK_UACPI(uacpi_map_gas_noalloc(&gas, mapped_gas));

cleanup:
    return err;
}

static uacpi_iteration_decision acpi_ec_probe(void *user, uacpi_namespace_node *node, uacpi_u32 node_depth) {
    err_t err = NO_ERROR;
    uacpi_resources* resources = NULL;

    // ignore the boot EC
    if (m_boot_ec != NULL && node == m_boot_ec->node) {
        goto cleanup;
    }

    acpi_ec_t* ec = mem_alloc(sizeof(*ec));
    CHECK_ERROR(ec != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    memset(ec, 0, sizeof(*ec));

    ec->node = node;

    // parse the resources
    CHECK_UACPI(uacpi_get_current_resources(node, &resources));
    CHECK(resources->length == 2);

    // parse command and data
    uacpi_resource* resource = resources->entries;
    RETHROW(acpi_ec_create_gas(resource, &ec->control));

    resource = UACPI_NEXT_RESOURCE(resource);
    RETHROW(acpi_ec_create_gas(resource, &ec->data));

    // TODO: gpio interrupt for hardware-reduced acpi

    // get the gpe number
    // TODO: support package, support GPIO instead of GPE
    uint64_t gpe;
    CHECK_UACPI(uacpi_eval_simple_integer(ec->node, "_GPE", &gpe));
    ec->gpe_bit = gpe;

    // and fully initialize it
    RETHROW(acpi_init_ec(ec));

cleanup:
    if (resources != NULL) {
        uacpi_free_resources(resources);
    }

    if (IS_ERROR(err)) {
        mem_free(ec);
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

err_t init_ec() {
    err_t err = NO_ERROR;

    // start by running the _REG of the EC, if any boot EC
    if (m_boot_ec != NULL) {
        CHECK_UACPI(uacpi_reg_all_opregions(m_boot_ec->node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER));
    }

    // now go over and initialize the rest of the embedded controllers
    // that exist in the namespace
    CHECK_UACPI(uacpi_find_devices("PNP0C09", acpi_ec_probe, NULL));

cleanup:
    return err;
}

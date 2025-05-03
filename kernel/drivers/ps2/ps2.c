#include "ps2.h"

#include "acpi/acpi.h"
#include "arch/apic.h"
#include "arch/intrin.h"
#include "drivers/driver.h"
#include "lib/except.h"
#include "mem/alloc.h"
#include "thread/intr.h"
#include "thread/scheduler.h"
#include "thread/thread.h"
#include "uacpi/resources.h"

typedef struct ps2_keyboard {
    /**
     * The ports for communicating with the keyboard
     */
    uint16_t command_port;
    uint16_t data_port;

    /**
     * The irq of this keyboard
     */
    uint32_t irq;

    /**
     * The worker thread
     */
    thread_t* worker;

    /**
     * the interrupt handler for the keyboard
     */
    interrupt_handler_t handler;

    /**
     * Mutex to protect against sending commands
     * in parallel to the keyboard
     */
    mutex_t mutex;
} ps2_keyboard_t;

static void ps2_poll(ps2_keyboard_t* keyboard, uint8_t want, uint8_t dont_want) {
    uint8_t status = __inbyte(keyboard->command_port);
    while (((status & want) != want) || ((status & dont_want) != 0)) {
        uacpi_kernel_stall(30);
        status = __inbyte(keyboard->command_port);
    }
}

static void ps2_write_command(ps2_keyboard_t* keyboard, uint8_t data) {
    ps2_poll(keyboard, 0, PS2_INPUT_BUFFER_FULL);
    __outbyte(keyboard->command_port, data);
    ps2_poll(keyboard, 0, PS2_INPUT_BUFFER_FULL);
}

static void ps2_write_data(ps2_keyboard_t* keyboard, uint8_t data) {
    ps2_poll(keyboard, 0, PS2_INPUT_BUFFER_FULL);
    __outbyte(keyboard->data_port, data);
}

static void ps2_keyboard_worker(void* _ctx) {
    err_t err = NO_ERROR;
    ps2_keyboard_t* ctx = (ps2_keyboard_t*)_ctx;

    for (;;) {
        // wait for an interrupt, when returns this will be unmasked
        irq_wait(&ctx->handler);

        // unlock the keyboard to ensure we can
        // read data without any commands are issued
        mutex_lock(&ctx->mutex);
        while (__inbyte(ctx->command_port) & PS2_OUTPUT_BUFFER_FULL) {
            uint8_t scan_code = __inbyte(ctx->data_port);
            TRACE("GOT SCANCODE %d", scan_code);
        }
        mutex_unlock(&ctx->mutex);

        // unmask the interrupts
        RETHROW(ioapic_enable_irq(ctx->irq, true));
    }

cleanup:
    (void)err;
}

static void ps2_keyboard_interrupt(interrupt_handler_t* handler) {
    ps2_keyboard_t* ctx = containerof(handler, ps2_keyboard_t, handler);

    // mask further interrupts
    ioapic_enable_irq(ctx->irq, false);

    // and wakeup the worker thread
    irq_wakeup(&ctx->handler);
}

static err_t ps2_keyboard_init(uacpi_namespace_node* node, uacpi_namespace_node_info* info, const uacpi_char* abs_path, uint32_t sta_flags) {
    err_t err = NO_ERROR;
    uacpi_resources* resources = NULL;

    // prepare the structure
    ps2_keyboard_t* keyboard = mem_alloc(sizeof(*keyboard));
    CHECK_ERROR(keyboard != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    memset(keyboard, 0, sizeof(*keyboard));

    // parse the resource
    CHECK_UACPI(uacpi_get_current_resources(node, &resources));

    // data port
    uacpi_resource* resource = resources->entries;
    CHECK(resource->type == UACPI_RESOURCE_TYPE_IO);
    keyboard->data_port = resource->io.minimum;

    // command port
    resource = UACPI_NEXT_RESOURCE(resource);
    CHECK(resource->type == UACPI_RESOURCE_TYPE_IO);
    keyboard->command_port = resource->io.minimum;

    // isa interrupt
    resource = UACPI_NEXT_RESOURCE(resource);
    CHECK(resource->type == UACPI_RESOURCE_TYPE_IRQ);
    CHECK(resource->irq.num_irqs == 1);
    keyboard->handler.handler = ps2_keyboard_interrupt;
    RETHROW(irq_allocate(&keyboard->handler));
    ioapic_irq_t irq = acpi_convert_isa_to_gsi(
        resource->irq.irqs[0],
        resource->irq.triggering == UACPI_TRIGGERING_LEVEL,
        resource->irq.polarity == UACPI_POLARITY_ACTIVE_HIGH
    );
    keyboard->irq = irq.irq;
    RETHROW(ioapic_configure_irq(&irq, keyboard->handler.vector, 0));

    // create and start the worker thread
    keyboard->worker = thread_create(ps2_keyboard_worker, (void*)keyboard, "ps2-keyboard@%s", abs_path);
    CHECK_ERROR(keyboard->worker != NULL, UACPI_STATUS_OUT_OF_MEMORY);
    scheduler_start_thread(keyboard->worker);

    // empty the data port
    while (__inbyte(keyboard->command_port) & PS2_OUTPUT_BUFFER_FULL) {
        __inbyte(keyboard->data_port);
    }

    // configure the controller to our liking
    ps2_write_command(keyboard, PS2_CMD_WRITE_CONFIG);
    ps2_write_data(keyboard,
        PS2_CONFIG_FIRST_PS2_INTERRUPT_ENABLE |
        PS2_CONFIG_SYSTEM_FLAG |
        PS2_CONFIG_SECOND_PS2_CLOCK_DISABLE |
        PS2_CONFIG_FIRST_PS2_PORT_TRANSLATION
    );

    // enable the irq
    RETHROW(ioapic_enable_irq(keyboard->irq, true));

cleanup:
    if (IS_ERROR(err)) {
        irq_free(&keyboard->handler);
        mem_free(keyboard);
    }

    return err;
}


ACPI_DRIVER("ps2-keyboard", "PNP0303", ps2_keyboard_init);

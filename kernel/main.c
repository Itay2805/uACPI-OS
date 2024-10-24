#include <limine.h>
#include <mem/page.h>
#include <thread/sleep.h>
#include <uacpi/event.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/internal/log.h>

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// uACPI helpers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request m_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request m_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rdsp_address) {
    if (m_rsdp_request.response == NULL) {
        return UACPI_STATUS_NOT_FOUND;
    }

    *out_rdsp_address = (uacpi_phys_addr)m_rsdp_request.response->address - m_hhdm_request.response->offset;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_raw_memory_read(uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 *out_value) {
    void* ptr = (void*)address + m_hhdm_request.response->offset;
    switch (byte_width) {
        case 1: *out_value = *(volatile uint8_t*)ptr; break;
        case 2: *out_value = *(volatile uint16_t*)ptr; break;
        case 4: *out_value = *(volatile uint32_t*)ptr; break;
        case 8: *out_value = *(volatile uint64_t*)ptr; break;
        default: return UACPI_STATUS_INVALID_ARGUMENT;
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_raw_memory_write(uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 in_value) {
    void* ptr = (void*)address + m_hhdm_request.response->offset;
    switch (byte_width) {
        case 1: *(volatile uint8_t*)ptr = in_value; break;
        case 2: *(volatile uint16_t*)ptr = in_value; break;
        case 4: *(volatile uint32_t*)ptr = in_value; break;
        case 8: *(volatile uint64_t*)ptr = in_value; break;
        default: return UACPI_STATUS_INVALID_ARGUMENT;
    }
    return UACPI_STATUS_OK;
}

static inline uint8_t inb(uint16_t port) { uint8_t ret; __asm__ volatile ("inb %w1, %0" : "=a"(ret) : "Nd"(port) : "memory"); return ret; }
static inline uint16_t inw(uint16_t port) { uint16_t ret; __asm__ volatile ("inw %w1, %0" : "=a"(ret) : "Nd"(port) : "memory"); return ret; }
static inline uint32_t inl(uint16_t port) { uint32_t ret; __asm__ volatile ("inl %w1, %0" : "=a"(ret) : "Nd"(port) : "memory"); return ret; }

uacpi_status uacpi_kernel_raw_io_read(uacpi_io_addr address, uacpi_u8 byte_width, uacpi_u64 *out_value) {
    if (address > UINT16_MAX) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    switch (byte_width) {
        case 1: *out_value = inb(address); break;
        case 2: *out_value = inw(address); break;
        case 4: *out_value = inl(address); break;
        default: return UACPI_STATUS_INVALID_ARGUMENT;
    }
    return UACPI_STATUS_OK;
}

static inline void outb(uint16_t port, uint8_t val) { __asm__ volatile ( "outb %0, %w1" : : "a"(val), "Nd"(port) : "memory"); }
static inline void outw(uint16_t port, uint16_t val) { __asm__ volatile ( "outw %0, %w1" : : "a"(val), "Nd"(port) : "memory"); }
static inline void outl(uint16_t port, uint32_t val) { __asm__ volatile ( "outl %0, %w1" : : "a"(val), "Nd"(port) : "memory"); }

uacpi_status uacpi_kernel_raw_io_write(uacpi_io_addr address, uacpi_u8 byte_width, uacpi_u64 in_value) {
    if (address > UINT16_MAX) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    switch (byte_width) {
        case 1: outb(address, in_value); break;
        case 2: outw(address, in_value); break;
        case 4: outl(address, in_value); break;
        default: return UACPI_STATUS_INVALID_ARGUMENT;
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    // TODO: make sure is mapped
    *out_handle = (void*)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    // nothing to do
}

uacpi_status uacpi_kernel_io_read(uacpi_handle handle, uacpi_size offset, uacpi_u8 byte_width, uacpi_u64 *value) {
    return uacpi_kernel_raw_io_read((uintptr_t)handle + offset, byte_width, value);
}

uacpi_status uacpi_kernel_io_write(uacpi_handle handle, uacpi_size offset, uacpi_u8 byte_width, uacpi_u64 value) {
    return uacpi_kernel_raw_io_write((uintptr_t)handle + offset, byte_width, value);
}

uacpi_status uacpi_kernel_pci_read(
    uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value
) {
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_write(
    uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value
) {
    return UACPI_STATUS_UNIMPLEMENTED;
}

void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    // TODO: make sure is mapped
    return (void*)addr + m_hhdm_request.response->offset;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    // nothing to do
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle
) {
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle
) {
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_schedule_work(
    uacpi_work_type work_type, uacpi_work_handler handler, uacpi_handle ctx
) {
    return UACPI_STATUS_UNIMPLEMENTED;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// host startup
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request) {
    switch (request->type) {
        case UACPI_FIRMWARE_REQUEST_TYPE_FATAL: {
            uacpi_error("Fatal\n");

            asm("cli; hlt");
        } break;

        case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT: {
            uacpi_debug("Breakpoint\n");
        } break;
    }
    return UACPI_STATUS_OK;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* msg) {
    while (*msg != '\0') {
        outb(0xe9, *msg);
        msg++;
    }
}

uacpi_status uacpi_kernel_initialize(uacpi_init_level current_init_lvl) {
    switch (current_init_lvl) {
        case UACPI_INIT_LEVEL_EARLY: {
        } break;

        case UACPI_INIT_LEVEL_SUBSYSTEM_INITIALIZED: {
            // initialize sleeping, requires access
            // to acpi tables
            init_sleep();
        } break;

        case UACPI_INIT_LEVEL_NAMESPACE_LOADED: {

        } break;

        case UACPI_INIT_LEVEL_NAMESPACE_INITIALIZED: {

        } break;
    }
    return UACPI_STATUS_OK;
}

void uacpi_kernel_deinitialize(void) {
    // keep the log level
    uacpi_context_set_log_level(UACPI_LOG_DEBUG);
}


__attribute__((used, section(".requests")))
volatile struct limine_module_request m_module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

void _start() {
    // give me everything for now
    uacpi_context_set_log_level(UACPI_LOG_TRACE);

    // initialize the allocator
    init_page();

    // initialize uacpi
    uacpi_status status = uacpi_initialize(0);
    if (uacpi_unlikely_error(status)) {
        uacpi_error("uacpi_initialize error: %s\n", uacpi_status_to_string(status));
        goto error;
    }

    // install all the modules as acpi tables
    if (m_module_request.response != NULL) {
        for (int i = 0; i < m_module_request.response->module_count; i++) {
            struct limine_file* module = m_module_request.response->modules[i];
            uacpi_table table;
            uacpi_info("Installing table %s\n", module->path);
            status = uacpi_table_install(module->address, &table);
            if (uacpi_unlikely_error(status)) {
                uacpi_error("\terror: %s, skipping\n", uacpi_status_to_string(status));
            }
        }
    }

    // load the acpi namespace
    status = uacpi_namespace_load();
    if (uacpi_unlikely_error(status)) {
        uacpi_error("uacpi_namespace_load error: %s\n", uacpi_status_to_string(status));
        goto error;
    }

    // initialize the namespace
    status = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(status)) {
        uacpi_error("uacpi_namespace_initialize error: %s\n", uacpi_status_to_string(status));
        goto error;
    }

    // initialize the namespace
    status = uacpi_finalize_gpe_initialization();
    if (uacpi_unlikely_error(status)) {
        uacpi_error("uACPI GPE initialization error: %s\n", uacpi_status_to_string(status));
        goto error;
    }

    uacpi_info("Initialized!");

error:
    asm("cli; hlt");
}

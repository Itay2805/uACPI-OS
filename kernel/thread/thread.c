#include <uacpi/kernel_api.h>

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    // TODO: for now a single core
    return 0;
}

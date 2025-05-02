#pragma once

#include <stdarg.h>

#include <uacpi/internal/log.h>

/**
 * Early logging initialization
 */
void init_early_logging(void);

/**
 * logging initialization after allocator init
 */
void init_logging(void);

// log levels
#define DEBUG(fmt, ...)         uacpi_log(UACPI_LOG_DEBUG, "[?] " fmt "\n", ##__VA_ARGS__)
#define TRACE(fmt, ...)         uacpi_log(UACPI_LOG_INFO, "[*] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...)          uacpi_log(UACPI_LOG_WARN, "[!] " fmt "\n", ##__VA_ARGS__)
#define ERROR(fmt, ...)         uacpi_log(UACPI_LOG_ERROR, "[-] " fmt "\n", ##__VA_ARGS__)

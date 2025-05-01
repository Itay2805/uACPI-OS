#pragma once

#include <stdarg.h>

#include <uacpi/internal/log.h>

void init_early_logging();

// log levels
#define DEBUG(fmt, ...)         uacpi_log(UACPI_LOG_DEBUG, "[?] " fmt "\n", ##__VA_ARGS__)
#define TRACE(fmt, ...)         uacpi_log(UACPI_LOG_INFO, "[*] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...)          uacpi_log(UACPI_LOG_WARN, "[!] " fmt "\n", ##__VA_ARGS__)
#define ERROR(fmt, ...)         uacpi_log(UACPI_LOG_ERROR, "[-] " fmt "\n", ##__VA_ARGS__)

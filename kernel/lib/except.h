#pragma once

#include "cpp_magic.h"
#include "defs.h"
#include "debug/log.h"
#include "uacpi/status.h"

typedef struct err {
    uacpi_status status;
} err_t;

#define NO_ERROR ((err_t){ .status = UACPI_STATUS_OK })

/**
 * Check if there was an error
 */
#define IS_ERROR(err) ((err).status != UACPI_STATUS_OK)

//----------------------------------------------------------------------------------------------------------------------
// A check that fails if the expression returns false
//----------------------------------------------------------------------------------------------------------------------

#define CHECK_ERROR_LABEL(check, error, label, ...) \
    do { \
        if (UNLIKELY(!(check))) { \
            err.status = error; \
            IF(HAS_ARGS(__VA_ARGS__))(ERROR(__VA_ARGS__)); \
            ERROR("Check failed with error %s (%d) in function %s (%s:%d)", uacpi_status_to_string(err.status), err.status, __FUNCTION__, __FILE__, __LINE__); \
            goto label; \
        } \
    } while(0)

#define CHECK_ERROR(check, error, ...)              CHECK_ERROR_LABEL(check, error, cleanup, ## __VA_ARGS__)
#define CHECK_LABEL(check, label, ...)              CHECK_ERROR_LABEL(check, UACPI_STATUS_INTERNAL_ERROR, label, ## __VA_ARGS__)
#define CHECK(check, ...)                           CHECK_ERROR_LABEL(check, UACPI_STATUS_INTERNAL_ERROR, cleanup, ## __VA_ARGS__)

//----------------------------------------------------------------------------------------------------------------------
// A check that fails without a condition
//----------------------------------------------------------------------------------------------------------------------

#define CHECK_FAIL(...)                             CHECK_ERROR_LABEL(0, UACPI_STATUS_INTERNAL_ERROR, cleanup, ## __VA_ARGS__)
#define CHECK_FAIL_ERROR(error, ...)                CHECK_ERROR_LABEL(0, error, cleanup, ## __VA_ARGS__)
#define CHECK_FAIL_LABEL(label, ...)                CHECK_ERROR_LABEL(0, UACPI_STATUS_INTERNAL_ERROR, label, ## __VA_ARGS__)
#define CHECK_FAIL_ERROR_LABEL(error, label, ...)   CHECK_ERROR_LABEL(0, error, label, ## __VA_ARGS__)

//----------------------------------------------------------------------------------------------------------------------
// A check that fails if an error was returned, used around functions returning an error
//----------------------------------------------------------------------------------------------------------------------

#define RETHROW_LABEL(error, label) \
    do { \
        err = error; \
        if (UNLIKELY(IS_ERROR(err))) { \
            ERROR("\trethrown at %s (%s:%d)", __FUNCTION__, __FILE__, __LINE__); \
            goto label; \
        } \
    } while(0)

#define RETHROW(error) RETHROW_LABEL(error, cleanup)

//----------------------------------------------------------------------------------------------------------------------
// Assertion
//----------------------------------------------------------------------------------------------------------------------

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            ERROR("Assertion failed at %s (%s:%d)", __FUNCTION__, __FILE__, __LINE__); \
            __builtin_trap(); \
        } \
    } while (0)

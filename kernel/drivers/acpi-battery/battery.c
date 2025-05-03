#include <drivers/driver.h>

#include "acpi/acpi.h"
#include "lib/string.h"
#include "uacpi/notify.h"

#define UACPI_CHECK_TYPE(expr, type) \
    do { \
        typeof(expr) __expr = expr; \
        typeof(type) __type = type; \
        CHECK(__expr == __type, "Expected %s, got %s", uacpi_object_type_to_string(type), uacpi_object_type_to_string(__expr)); \
    } while (0)


#define UACPI_CHECK_STRING(expr) \
    do { \
        typeof(expr) __expr = expr; \
        CHECK(__expr == UACPI_OBJECT_STRING || __expr == UACPI_OBJECT_BUFFER, "Expected buffer or string, got %s", uacpi_object_type_to_string(__expr)); \
    } while (0)


// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily
// TODO: Have a single generic struct that takes the info from both the BIF and BIX so we can print it more easily

static err_t acpi_battery_dump_bif(uacpi_namespace_node* node) {
    err_t err = NO_ERROR;

    // get the static information
    uacpi_object* object = NULL;
    CHECK_UACPI(uacpi_eval_simple_package(node, "_BIF", &object));

    uacpi_object_array array;
    CHECK_UACPI(uacpi_object_get_package(object, &array));

    CHECK(array.count == 13, "Expected 13, got %ld", array.count);
    UACPI_CHECK_TYPE(array.objects[0]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[1]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[2]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[3]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[4]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[5]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[6]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[7]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[8]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_STRING(array.objects[9]->type);
    UACPI_CHECK_STRING(array.objects[10]->type);
    UACPI_CHECK_STRING(array.objects[11]->type);
    UACPI_CHECK_STRING(array.objects[12]->type);

    uint32_t power_unit = array.objects[0]->integer;
    CHECK(power_unit == 0 || power_unit == 1, "Unknown power unit %d", power_unit);
    const char* capacity_str = power_unit == 0 ? "mWh" : "mAh";
    const char* rate_str = power_unit == 0 ? "mW" : "mA";

    if (array.objects[1]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign Capacity: %ld%s", array.objects[1]->integer, capacity_str);
    else if (array.objects[1]->integer == 0xFFFFFFFF) TRACE("\t\tDesign Capacity: Unknown");
    else TRACE("\t\tDesign Capacity: Invalid (%lx)", array.objects[1]->integer);

    if (array.objects[2]->integer <= 0x7FFFFFFF) TRACE("\t\tLast full charge capacity: %ld%s", array.objects[2]->integer, capacity_str);
    else if (array.objects[2]->integer == 0xFFFFFFFF) TRACE("\t\tLast full charge capacity: Unknown");
    else TRACE("\t\tLast full charge capacity: Invalid (%lx)", array.objects[2]->integer);

     // TODO: technology

    if (array.objects[4]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign voltage: %ldmV", array.objects[4]->integer);
    else if (array.objects[4]->integer == 0xFFFFFFFF) TRACE("\t\tDesign voltage: Unknown");
    else TRACE("\t\tDesign voltage: Invalid (%lx)", array.objects[4]->integer);

    if (array.objects[5]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign capacity of warning: %ld%s", array.objects[5]->integer, capacity_str);
    else TRACE("\t\tDesign capacity of warning: Invalid (%lx)", array.objects[5]->integer);

    if (array.objects[6]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign capacity of low: %ld%s", array.objects[6]->integer, capacity_str);
    else TRACE("\t\tDesign capacity of low: Invalid (%lx)", array.objects[6]->integer);

    TRACE("\t\tBattery Capacity Granularity (Low-Warning): %ld%s", array.objects[7]->integer, capacity_str);
    TRACE("\t\tBattery Capacity Granularity (Warning-Full): %ld%s", array.objects[8]->integer, capacity_str);

    TRACE("\t\tModel Number: %s", array.objects[9]->buffer->text);
    TRACE("\t\tSerial Number: %s", array.objects[10]->buffer->text);
    TRACE("\t\tBattery Type: %s", array.objects[11]->buffer->text);
    TRACE("\t\tOEM Information: %s", array.objects[12]->buffer->text);

cleanup:
    if (object != NULL) {
        uacpi_object_unref(object);
    }

    return err;
}

static err_t acpi_battery_dump_bix(uacpi_namespace_node* node) {
    err_t err = NO_ERROR;

    // get the static information
    uacpi_object* object = NULL;
    CHECK_UACPI(uacpi_eval_simple_package(node, "_BIX", &object));

    uacpi_object_array array;
    CHECK_UACPI(uacpi_object_get_package(object, &array));

    CHECK(array.count == 21, "Expected 13, got %ld", array.count);
    UACPI_CHECK_TYPE(array.objects[0]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[1]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[2]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[3]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[4]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[5]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[6]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[7]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[8]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[9]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[10]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[11]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[12]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[13]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[14]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[15]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_STRING(array.objects[16]->type);
    UACPI_CHECK_STRING(array.objects[17]->type);
    UACPI_CHECK_STRING(array.objects[18]->type);
    UACPI_CHECK_STRING(array.objects[19]->type);
    UACPI_CHECK_TYPE(array.objects[20]->type, UACPI_OBJECT_INTEGER);

    CHECK(array.objects[0]->integer == 1, "Expected revision 1, got %ld", array.objects[0]->integer);

    uint32_t power_unit = array.objects[1]->integer;
    CHECK(power_unit == 0 || power_unit == 1, "Unknown power unit %d", power_unit);
    const char* capacity_str = power_unit == 0 ? "mWh" : "mAh";
    const char* rate_str = power_unit == 0 ? "mW" : "mA";

    if (array.objects[2]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign Capacity: %ld%s", array.objects[2]->integer, capacity_str);
    else if (array.objects[2]->integer == 0xFFFFFFFF) TRACE("\t\tDesign Capacity: Unknown");
    else TRACE("\t\tDesign Capacity: Invalid (%lx)", array.objects[2]->integer);

    if (array.objects[3]->integer <= 0x7FFFFFFF) TRACE("\t\tLast full charge capacity: %ld%s", array.objects[3]->integer, capacity_str);
    else if (array.objects[3]->integer == 0xFFFFFFFF) TRACE("\t\tLast full charge capacity: Unknown");
    else TRACE("\t\tLast full charge capacity: Invalid (%lx)", array.objects[3]->integer);

     // TODO: technology

    if (array.objects[5]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign voltage: %ldmV", array.objects[5]->integer);
    else if (array.objects[5]->integer == 0xFFFFFFFF) TRACE("\t\tDesign voltage: Unknown");
    else TRACE("\t\tDesign voltage: Invalid (%lx)", array.objects[5]->integer);

    if (array.objects[6]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign capacity of warning: %ld%s", array.objects[6]->integer, capacity_str);
    else TRACE("\t\tDesign capacity of warning: Invalid (%lx)", array.objects[6]->integer);

    if (array.objects[7]->integer <= 0x7FFFFFFF) TRACE("\t\tDesign capacity of low: %ld%s", array.objects[7]->integer, capacity_str);
    else TRACE("\t\tDesign capacity of low: Invalid (%lx)", array.objects[7]->integer);

    if (array.objects[8]->integer <= 0xFFFFFFFE) TRACE("\t\tCycle count: %ld", array.objects[8]->integer);
    else TRACE("\t\tCycle count: Unknown");

    if (array.objects[9]->integer <= 100000) TRACE("\t\tMeasurement accuracy: %ld.%ld", array.objects[9]->integer / 1000, array.objects[9]->integer % 1000);
    else TRACE("\t\tMeasurement accuracy: Invalid (%ld)", array.objects[9]->integer);

    if (array.objects[10]->integer <= 0xFFFFFFFE) TRACE("\t\tMax sampling time: %ldms", array.objects[10]->integer);
    else TRACE("\t\tMax sampling time: Unknown");

    if (array.objects[11]->integer <= 0xFFFFFFFE) TRACE("\t\tMin sampling time: %ldms", array.objects[11]->integer);
    else TRACE("\t\tMin sampling time: Unknown");

    TRACE("\t\tMax averaging interval: %ldms", array.objects[12]->integer);
    TRACE("\t\tMin averaging interval: %ldms", array.objects[13]->integer);

    TRACE("\t\tBattery Capacity Granularity (Low-Warning): %ld%s", array.objects[14]->integer, capacity_str);
    TRACE("\t\tBattery Capacity Granularity (Warning-Full): %ld%s", array.objects[15]->integer, capacity_str);

    TRACE("\t\tModel Number: %s", array.objects[16]->buffer->text);
    TRACE("\t\tSerial Number: %s", array.objects[17]->buffer->text);
    TRACE("\t\tBattery Type: %s", array.objects[18]->buffer->text);
    TRACE("\t\tOEM Information: %s", array.objects[19]->buffer->text);

    switch (array.objects[20]->integer) {
        case 0x0: TRACE("\t\tBattery swapping capability: Non swappable"); break;
        case 0x1: TRACE("\t\tBattery swapping capability: Cold swappable"); break;
        case 0x10: TRACE("\t\tBattery swapping capability: Hot swappable"); break;
        default: break;
    }


cleanup:
    if (object != NULL) {
        uacpi_object_unref(object);
    }

    return err;
}

static err_t acpi_battery_dump_info(uacpi_namespace_node* node) {
    err_t err = NO_ERROR;

    // dump the information, use BIX when possible
    uacpi_status status = uacpi_namespace_node_find(node, "_BIX", NULL);
    if (status == UACPI_STATUS_NOT_FOUND) {
        RETHROW(acpi_battery_dump_bif(node));
    } else {
        CHECK_UACPI(status);
        RETHROW(acpi_battery_dump_bix(node));
    }

cleanup:
    return err;
}

static err_t acpi_battery_dump_status(uacpi_namespace_node *node) {
    err_t err = NO_ERROR;

    uacpi_object* object = NULL;
    CHECK_UACPI(uacpi_eval_simple_package(node, "_BST", &object));

    uacpi_object_array array;
    CHECK_UACPI(uacpi_object_get_package(object, &array));

    CHECK(array.count == 4);
    UACPI_CHECK_TYPE(array.objects[0]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[1]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[2]->type, UACPI_OBJECT_INTEGER);
    UACPI_CHECK_TYPE(array.objects[3]->type, UACPI_OBJECT_INTEGER);

    const char* b0 = (array.objects[0]->integer & BIT0) ? "discharging " : "";
    const char* b1 = (array.objects[0]->integer & BIT1) ? "charging " : "";
    const char* b2 = (array.objects[0]->integer & BIT2) ? "critical-energy-state " : "";
    const char* b3 = (array.objects[0]->integer & BIT3) ? "battery-charge-limiting " : "";

    // TODO: find the rate from the BIX/BIF and keep it for printing here
    TRACE("\t\tBattery state: %s%s%s%s", b0, b1, b2, b3);
    TRACE("\t\tBattery present rate: %ld", array.objects[1]->integer);

    if (array.objects[2]->integer <= 0x7FFFFFFF) TRACE("\t\tBattery remaining capacity: %ld", array.objects[2]->integer);
    else if (array.objects[2]->integer == 0xFFFFFFFF) TRACE("\t\tBattery remaining capacity: Unknown");
    else TRACE("\t\tBattery remaining capacity: Invalid (%lx)", array.objects[2]->integer);

    if (array.objects[3]->integer <= 0x7FFFFFFF) TRACE("\t\tBattery present voltage: %ldmV", array.objects[3]->integer);
    else if (array.objects[3]->integer == 0xFFFFFFFF) TRACE("\t\tBattery present voltage: Unknown");
    else TRACE("\t\tBattery present voltage: Invalid (%lx)", array.objects[3]->integer);

cleanup:
    if (object != NULL) {
        uacpi_object_unref(object);
    }

    return err;
}

static uacpi_status acpi_battery_notification(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
    err_t err = NO_ERROR;
    const char* abs_path = context;

    if (value == 0x81) {
        //
        // battery was added/removed
        //
        uint32_t flags;
        CHECK_UACPI(uacpi_eval_sta(node, &flags));
        TRACE("acpi-battery: [%s] %s", abs_path,
            (flags & ACPI_STA_RESULT_DEVICE_BATTERY_PRESENT) ? "battery added" : "battery removed");

        if (flags & ACPI_STA_RESULT_DEVICE_BATTERY_PRESENT) {
            RETHROW(acpi_battery_dump_info(node));
        }

    } else if (value == 0x80) {
        RETHROW(acpi_battery_dump_status(node));

        // _BMD - battery information

    } else {
        WARN("acpi-battery: [%s] unknown notification %02lx", abs_path, value);
    }

cleanup:
    return err.status;
}


static err_t acpi_battery_init(uacpi_namespace_node* node, uacpi_namespace_node_info* info, const uacpi_char* abs_path, uint32_t sta_flags) {
    err_t err = NO_ERROR;

    // duplicate
    abs_path = strdup(abs_path);
    CHECK_ERROR(abs_path != NULL, UACPI_STATUS_OUT_OF_MEMORY);

    if (sta_flags & ACPI_STA_RESULT_DEVICE_BATTERY_PRESENT) {
        RETHROW(acpi_battery_dump_info(node));
        RETHROW(acpi_battery_dump_status(node));
    } else {
        TRACE("\t\tbattery not present!");
    }

    // just install a notification handler, nothing else to do really
    CHECK_UACPI(uacpi_install_notify_handler(node, acpi_battery_notification, (uacpi_handle)abs_path));

cleanup:
    return err;
}

ACPI_DRIVER("acpi-battery", "PNP0C0A", acpi_battery_init);

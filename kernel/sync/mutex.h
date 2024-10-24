#pragma once

#include <uacpi/kernel_api.h>

#include <stdatomic.h>

typedef struct mutex {
    atomic_flag flag;
} mutex_t;


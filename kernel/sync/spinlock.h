#pragma once

#include <uacpi/kernel_api.h>

#include <stdatomic.h>

typedef struct spinlock {
    atomic_flag flag;
} spinlock_t;


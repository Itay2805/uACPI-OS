#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <uacpi/internal/log.h>

typedef struct list_entry {
    struct list_entry* next;
    struct list_entry* prev;
} list_entry_t;

typedef list_entry_t list_t;

#define INIT_LIST_HEAD(name) {&(name), &(name)}

#define CR(Record, TYPE, Field)  ((TYPE*)((char*)(Record) - offsetof(TYPE, Field)))

static inline void list_insert(list_t* list, list_entry_t* entry) {
    entry->next = list->next;
    entry->prev = list;
    entry->next->prev = entry;
    list->next = entry;
}

static inline void list_insert_tail(list_t* list, list_entry_t* entry) {
    entry->next = list;
    entry->prev = list->prev;
    entry->prev->next = entry;
    list->prev = entry;
}

static inline void list_remove(list_entry_t* entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}

static inline bool list_is_empty(list_t* entry) {
    return entry->next == entry;
}

#pragma once

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE   4096
#define PAGE_MASK   0xFFF
#define PAGE_SHIFT  12

#define SIZE_TO_PAGES(Size)  (((Size) >> PAGE_SHIFT) + (((Size) & PAGE_MASK) ? 1 : 0))
#define PAGES_TO_SIZE(Pages)  ((Pages) << PAGE_SHIFT)

void init_page(void);

void* page_alloc(size_t page_count);

void* page_alloc_max(size_t page_count, uintptr_t max_address);

void page_free(void* ptr, size_t page_count);

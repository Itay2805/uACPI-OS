#include "page.h"

#include <limine.h>
#include <util/list.h>
#include <sync/mutex.h>

#include <uacpi/internal/log.h>

typedef struct free_page_list {
    list_entry_t link;
    size_t number_of_pages;
} free_page_list_t;

/**
 * The freelist of pages
 */
static list_t m_memory_map = INIT_LIST_HEAD(m_memory_map);

/**
 * Lock to protect the page allocator
 */
static mutex_t m_memory_map_lock;

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request m_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

extern volatile struct limine_hhdm_request m_hhdm_request;

static const char* m_memmap_entry_name[] = {
    [LIMINE_MEMMAP_USABLE] = "USABLE",
    [LIMINE_MEMMAP_RESERVED] = "RESERVED",
    [LIMINE_MEMMAP_ACPI_RECLAIMABLE] = "ACPI_RECLAIMABLE",
    [LIMINE_MEMMAP_ACPI_NVS] = "ACPI_NVS",
    [LIMINE_MEMMAP_BAD_MEMORY] = "BAD_MEMORY",
    [LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE] = "BOOTLOADER_RECLAIMABLE",
    [LIMINE_MEMMAP_KERNEL_AND_MODULES] = "KERNEL_AND_MODULES",
    [LIMINE_MEMMAP_FRAMEBUFFER] = "FRAMEBUFFER",
};

void init_page(void) {
    // add all the usable entries to the allocator
    uacpi_info("Initializing memory map:\n");
    if (m_memmap_request.response == NULL) {
        uacpi_error("No memory map found!\n");
    }

    struct limine_memmap_entry** entries = m_memmap_request.response->entries;
    size_t entry_count = m_memmap_request.response->entry_count;

    size_t total = 0;
    for (size_t i = 0; i < entry_count; i++) {
        // log that we saw the entry
        uacpi_info("\t%lx-%lx: %s\n",
            entries[i]->base, entries[i]->base + entries[i]->length,
            m_memmap_entry_name[entries[i]->type]);

        // add it if its a usable entry
        if (entries[i]->type == LIMINE_MEMMAP_USABLE) {
            page_free((void*)entries[i]->base + m_hhdm_request.response->offset, SIZE_TO_PAGES(entries[i]->length));
            total += entries[i]->length;
        }
    }
    uacpi_info("Total memory map size: %lu\n", total);
}

static void* alloc_pages_on_node(free_page_list_t* pages, size_t number_of_pages, uintptr_t max_address) {
    // get the top address of the allocation, truncate it to the
    // range of the node
    uintptr_t top = (max_address + 1 - (uintptr_t)pages) >> PAGE_SHIFT;
    if (top > pages->number_of_pages) {
        top = pages->number_of_pages;
    }

    // if the top is less than the number of nodes then we will
    // need to split the node to fit it
    if (top < pages->number_of_pages) {
        free_page_list_t* node = (free_page_list_t*)((uintptr_t)pages + PAGES_TO_SIZE(top));
        node->number_of_pages = pages->number_of_pages - top;
        list_insert(&pages->link, &node->link);
    }

    // and now take the amount we need
    uintptr_t bottom = top - number_of_pages;
    if (bottom > 0) {
        pages->number_of_pages = bottom;
    } else {
        list_remove(&pages->link);
    }

    // return the pointer
    return (void*)pages + PAGES_TO_SIZE(bottom);
}

void* page_alloc_max(size_t page_count, uintptr_t max_address) {
    void* ptr = NULL;

    uacpi_kernel_acquire_mutex(&m_memory_map_lock, UINT16_MAX);

    for (list_entry_t* node = m_memory_map.prev; node != &m_memory_map; node = node->prev) {
        free_page_list_t* pages = CR(node, free_page_list_t, link);
        if (pages->number_of_pages >= page_count && ((uintptr_t)pages + PAGES_TO_SIZE(page_count) - 1) <= max_address) {
            ptr = alloc_pages_on_node(pages, page_count, max_address);
            break;
        }
    }

    uacpi_kernel_release_mutex(&m_memory_map_lock);

    return ptr;
}

void* page_alloc(size_t page_count) {
    return page_alloc_max(page_count, UINT64_MAX);
}

static free_page_list_t* page_merge_nodes(free_page_list_t* node) {
    free_page_list_t* next = CR(node->link.next, free_page_list_t, link);

    // if this is exactly next to each other
    if (((uintptr_t)next - (uintptr_t)node) >> PAGE_SHIFT == node->number_of_pages) {
        node->number_of_pages += next->number_of_pages;
        list_remove(&next->link);
        next = node;
    }

    return next;
}

void page_free(void* ptr, size_t page_count) {
    uacpi_kernel_acquire_mutex(&m_memory_map_lock, UINT16_MAX);

    // find the node which is before this new entry
    free_page_list_t* pages = NULL;
    list_entry_t* node = m_memory_map.next;
    while (node != &m_memory_map) {
        pages = CR(node, free_page_list_t, link);
        if (ptr < (void*)pages) {
            break;
        }
        node = node->next;
    }

    // and now add the pointer to it
    pages = ptr;
    pages->number_of_pages = page_count;
    list_insert_tail(node, &pages->link);

    // check if we can merge backwards
    if (pages->link.prev != &m_memory_map) {
        pages = page_merge_nodes(CR(pages->link.prev, free_page_list_t, link));
    }

    // and now merge forward as well
    if (node != &m_memory_map) {
        page_merge_nodes(pages);
    }

    uacpi_kernel_release_mutex(&m_memory_map_lock);
}

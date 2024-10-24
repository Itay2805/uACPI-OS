#include <sync/mutex.h>
#include <uacpi/kernel_api.h>
#include <util/list.h>

#include "page.h"

// min allocation size is 64 bytes
#define MIN_POOL_SHIFT  6
#define MIN_POOL_SIZE   (1 << MIN_POOL_SHIFT)

// max allocation size is 2k
#define MAX_POOL_SHIFT  (PAGE_SHIFT - 1)
#define MAX_POOL_SIZE   (1 << MAX_POOL_SHIFT)

// the pool entries
#define MAX_POOL_INDEX  (MAX_POOL_SHIFT - MIN_POOL_SHIFT + 1)

typedef struct pool_header {
    size_t size;
} pool_header_t;

typedef struct free_pool_header {
    pool_header_t header;
    list_entry_t link;
} free_pool_header_t;

static list_entry_t m_alloc_pool_lists[MAX_POOL_INDEX] = {
    INIT_LIST_HEAD(m_alloc_pool_lists[0]),
    INIT_LIST_HEAD(m_alloc_pool_lists[1]),
    INIT_LIST_HEAD(m_alloc_pool_lists[2]),
    INIT_LIST_HEAD(m_alloc_pool_lists[3]),
    INIT_LIST_HEAD(m_alloc_pool_lists[4]),
    INIT_LIST_HEAD(m_alloc_pool_lists[5]),
};

static mutex_t m_alloc_pool_mutex;

static free_pool_header_t* alloc_pool_by_index(size_t pool_index) {
    free_pool_header_t* hdr = NULL;

    // attempt to allocate from the given pool size
    if (pool_index == MAX_POOL_INDEX) {
        // we reached the max pool size, use the page allocator
        // directly for this case
        hdr = page_alloc(1);

    } else if (!list_is_empty(&m_alloc_pool_lists[pool_index])) {
        // we have an empty entry, use it
        hdr = CR(m_alloc_pool_lists[pool_index].next, free_pool_header_t, link);
        list_remove(&hdr->link);

    } else {
        // attempt to allocate from the next level
        hdr = alloc_pool_by_index(pool_index + 1);
        if (hdr != NULL) {
            // split the allocated entry into two entries, one we are going
            // to add to our pool, and one is going to be for returning to
            // the caller
            hdr->header.size >>= 1;
            list_insert(&m_alloc_pool_lists[pool_index], &hdr->link);

            hdr = (free_pool_header_t*)((uintptr_t)hdr + hdr->header.size);
        }
    }

    // set the header for this entry
    if (hdr != NULL) {
        hdr->header.size = MIN_POOL_SIZE << pool_index;
    }

    return hdr;
}

static size_t highest_set_bit(uint32_t val) {
    return 31 - __builtin_clz(val);
}

void* uacpi_kernel_alloc(uacpi_size size) {
    size += sizeof(pool_header_t);

    if (size > MAX_POOL_SIZE) {
        // larger than what we represent in the pool, just
        // allocate from the page allocator
        size_t no_pages = SIZE_TO_PAGES(size);
        pool_header_t* pool_hdr = page_alloc(no_pages);
        if (pool_hdr == NULL) {
            return NULL;
        }

        // remember the size and return the data after it
        pool_hdr->size = PAGES_TO_SIZE(no_pages);
        return pool_hdr + 1;
    }

    // find the pool index we need to allocate from
    size = (size + MIN_POOL_SIZE - 1) >> MIN_POOL_SHIFT;
    size_t pool_index = highest_set_bit(size);
    if ((size & (size - 1)) != 0) {
        pool_index++;
    }

    uacpi_kernel_acquire_mutex(&m_alloc_pool_mutex, UINT16_MAX);
    pool_header_t* ptr = (pool_header_t*)alloc_pool_by_index(pool_index);
    uacpi_kernel_release_mutex(&m_alloc_pool_mutex);

    // if we managed to allocate it, then advance the pointer
    // by one to get the actual data
    if (ptr != NULL) {
        ptr++;
    }

    return ptr;
}


void uacpi_kernel_free(void* mem) {
    if (mem == NULL) {
        return;
    }

    free_pool_header_t* header = (free_pool_header_t*)((pool_header_t*)mem - 1);

    if (header->header.size > MAX_POOL_SIZE) {
        // this is allocated using page allocator
        page_free(header, SIZE_TO_PAGES(header->header.size));
        return;
    }

    // this is allocated using pool allocator
    uacpi_kernel_acquire_mutex(&m_alloc_pool_mutex, UINT16_MAX);

    // just insert it into the freelist
    const size_t pool_index = highest_set_bit(header->header.size) - MIN_POOL_SHIFT;
    list_insert(&m_alloc_pool_lists[pool_index], &header->link);

    uacpi_kernel_release_mutex(&m_alloc_pool_mutex);
}

void* uacpi_kernel_calloc(uacpi_size count, uacpi_size size) {
    uacpi_size result;
    if (__builtin_mul_overflow(count, size, &result)) {
        return NULL;
    }

    void* ptr = uacpi_kernel_alloc(result);
    if (ptr != NULL) {
        __builtin_memset(ptr, 0, result);
    }

    return ptr;
}

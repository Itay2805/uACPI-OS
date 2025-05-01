#pragma once

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The kernel memory map
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// 0x00000000_00000000 - 0x00000000_FFFFFFFF: always unmapped
//
// 0xFFFF8000_00000000 - 0xFFFF807F_FFFFFFFF: Direct memory map
//
// 0xFFFF8F00_00000000 - 0xFFFF8F7F_FFFFFFFF: Stacks
// 0xFFFFC000_00000000 - 0xFFFFCFFF_FFFFFFFF: Kernel heap
//
// 0xFFFFFF00_00000000 - 0xFFFFFF7F_FFFFFFFF: Page Mapping Level 1 (Page Tables)
// 0xFFFFFF7F_80000000 - 0xFFFFFF7F_BFFFFFFF: Page Mapping Level 2 (Page Directories)
// 0xFFFFFF7F_BFC00000 - 0xFFFFFF7F_BFDFFFFF: Page Mapping Level 3 (PDPTs / Page-Directory-Pointer Tables)
// 0xFFFFFF7F_BFDFE000 - 0xFFFFFF7F_BFDFEFFF: Page Mapping Level 4 (PML4)
//
// 0xFFFFFFFF_80000000 - 0xFFFFFFFF_8FFFFFFF: Kernel
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Direct map offset, static in memory, no KASLR please
 */
#define DIRECT_MAP_OFFSET       0xFFFF800000000000ULL

/**
 * The bottom of the stack allocator
 *
 * Each thread gets its own 8MB area, where we have 6MB of stack
 * and 2MB of guard "page", we can adjust these numbers as we see fit
 */
#define STACKS_ADDR             (0xFFFF8F0000000000ULL)
#define STACKS_ADDR_END         (0xFFFF8F8000000000ULL)

/**
 * Convert direct map pointers as required
 */
#define PHYS_TO_DIRECT(x) (void*)((uintptr_t)(x) + DIRECT_MAP_OFFSET)
#define DIRECT_TO_PHYS(x) (uintptr_t)((uintptr_t)(x) - DIRECT_MAP_OFFSET)

// page size is 4k
#define PAGE_SIZE   SIZE_4KB
#define PAGE_MASK   0xFFF
#define PAGE_SHIFT  12

#define SIZE_TO_PAGES(size)   (((size) >> PAGE_SHIFT) + (((size) & PAGE_MASK) ? 1 : 0))
#define PAGES_TO_SIZE(pages)  ((pages) << PAGE_SHIFT)

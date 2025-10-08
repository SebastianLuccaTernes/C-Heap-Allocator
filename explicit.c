#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "./allocator.h"
#include "./debug_break.h"

// Memory layout constants
static const size_t HDR_SIZE = sizeof(size_t);                  // Size of block header
static const size_t PTR_SIZE = sizeof(void *);                  // Size of pointer
static const size_t FREE_NODE_OVERHEAD = 2 * sizeof(void *);    // Space for prev/next pointers
static const size_t MIN_PAYLOAD = (FREE_NODE_OVERHEAD > ALIGNMENT) ? FREE_NODE_OVERHEAD : ALIGNMENT;
static const size_t MIN_BLOCK = sizeof(size_t) + MIN_PAYLOAD;   // Minimum block size including header

// Header flags and masks
static const size_t FLAG_ALLOC = (size_t)1;                     // Allocation flag (LSB)
static const size_t SIZE_MASK = ~(ALIGNMENT - 1);               // Mask to extract size from header

// Global heap management variables
static uint8_t *g_heap_base = NULL;     // Pointer to the beginning of the heap
static size_t g_heap_size = 0;          // Total size of the heap
static void *g_free_head = NULL;        // Head of the free blocks linked list

// Helper function to get the end of the heap
static inline uint8_t *heap_end(void) {
    if (!g_heap_base) {
        return NULL;
    }
    return g_heap_base + g_heap_size;
}

// Align size up to the next ALIGNMENT boundary
static inline size_t align_up(size_t n) {
    const size_t a = ALIGNMENT;
    return (n + (a - 1)) & ~(a - 1);
}

// Get raw header value without any processing
static inline size_t hdr_raw(void *hdr) {
    return *(size_t *)hdr;
}

// Write header with size and allocation status
static inline void hdr_write(void *hdr, size_t size, bool alloc) {
    *(size_t *)hdr = (size & SIZE_MASK) | (alloc ? FLAG_ALLOC : 0);
}

// Extract block size from header
static inline size_t blk_size(void *hdr) {
    return hdr_raw(hdr) & SIZE_MASK;
}

// Check if block is allocated
static inline bool blk_alloc(void *hdr) {
    return (hdr_raw(hdr) & FLAG_ALLOC) != 0;
}

// Get payload pointer from block header
static inline void *blk_payload(void *hdr) {
    return (uint8_t *)hdr + HDR_SIZE;
}

// Get block header from payload pointer
static inline void *blk_from_payload(void *payload) {
    return (uint8_t *)payload - HDR_SIZE;
}

// Check if pointer is within heap bounds
static inline bool ptr_in_heap(void *p) {
    uint8_t *u = (uint8_t *)p;
    return g_heap_base && u >= g_heap_base && u < heap_end();
}

// Get pointer to next block in linear sequence
static inline void *blk_next(void *hdr) {
    uint8_t *p = (uint8_t *)hdr + blk_size(hdr);
    if (p < heap_end()) {
        return (void *)p;
    }
    return (void *)heap_end();
}

// Get pointer to the previous pointer field in a free block
static inline void **free_prevp(void *hdr) {
    return (void **)((uint8_t *)hdr + HDR_SIZE);
}

// Get pointer to the next pointer field in a free block
static inline void **free_nextp(void *hdr) {
    return (void **)((uint8_t *)hdr + HDR_SIZE + PTR_SIZE);
}

// Get the previous free block pointer from a free block
static inline void *free_prev(void *hdr) {
    return *free_prevp(hdr);
}

// Get the next free block pointer from a free block
static inline void *free_next(void *hdr) {
    return *free_nextp(hdr);
}

// Insert a free block at the front of the free list
static void freelist_insert_front(void *hdr) {
    *free_prevp(hdr) = NULL;
    *free_nextp(hdr) = g_free_head;
    if (g_free_head) {
        *free_prevp(g_free_head) = hdr;
    }
    g_free_head = hdr;
}

// Remove a block from the free list
static void freelist_remove(void *hdr) {
    void *prev = free_prev(hdr);
    void *next = free_next(hdr);
    if (prev) {
        *free_nextp(prev) = next;
    } else {
        g_free_head = next;
    }
    if (next) {
        *free_prevp(next) = prev;
    }
    *free_prevp(hdr) = NULL;
    *free_nextp(hdr) = NULL;
}

// Find the previous block in linear order (expensive operation)
static inline void *blk_prev_linear(void *hdr) {
    if (hdr == (void *)g_heap_base) {
        return NULL;
    }
    void *prev = (void *)g_heap_base;
    while (prev && prev < hdr) {
        void *n = blk_next(prev);
        if (n == hdr) {
            return prev;
        }
        prev = n;
        if (prev == heap_end()) {
            break;
        }
    }
    return NULL;
}

// Convert requested size to aligned block size
static inline size_t request_to_asize(size_t requested) {
    if (requested == 0) {
        return 0;
    }
    size_t need = (requested < MIN_PAYLOAD) ? MIN_PAYLOAD : requested;
    need = align_up(need);
    size_t total = HDR_SIZE + need;
    if (total < MIN_BLOCK) {
        total = MIN_BLOCK;
    }
    return total;
}

// Coalesce free block with consecutive right neighbors
static void coalesce_right_chain(void *hdr_free) {
    for (;;) {
        void *n = blk_next(hdr_free);
        if (!ptr_in_heap(n) || n == heap_end()) {
            break;
        }
        if (blk_alloc(n)) {
            break;
        }
        freelist_remove(n);
        size_t merged = blk_size(hdr_free) + blk_size(n);
        hdr_write(hdr_free, merged, false);
    }
}

// Coalesce free block bidirectionally (with left and right neighbors)
static void coalesce_bidir(void **hdr_free_io) {
    void *hdr = *hdr_free_io;
    void *left = blk_prev_linear(hdr);
    if (left && !blk_alloc(left)) {
        freelist_remove(hdr);
        size_t merged = blk_size(left) + blk_size(hdr);
        hdr_write(left, merged, false);
        hdr = left;
        *hdr_free_io = hdr;
    }
    coalesce_right_chain(hdr);
}

// Allocate memory from a specific free block, splitting if necessary
static void *allocate_from_free(void *hdr, size_t asize) {
    size_t sz = blk_size(hdr);
    freelist_remove(hdr);
    if (sz >= asize + MIN_BLOCK) {
        void *right = (uint8_t *)hdr + asize;
        hdr_write(hdr, asize, true);
        hdr_write(right, sz - asize, false);
        freelist_insert_front(right);
        coalesce_right_chain(right);
        return blk_payload(hdr);
    } else {
        hdr_write(hdr, sz, true);
        return blk_payload(hdr);
    }
}

// Try to grow an allocated block in place by absorbing adjacent free blocks
static bool grow_in_place(void *hdr_alloc, size_t asize) {
    size_t cur = blk_size(hdr_alloc);
    while (cur < asize) {
        void *n = blk_next(hdr_alloc);
        if (!ptr_in_heap(n) || n == heap_end() || blk_alloc(n)) {
            break;
        }
        freelist_remove(n);
        cur += blk_size(n);
        hdr_write(hdr_alloc, cur, true);
    }
    if (cur < asize) {
        return false;
    }
    if (cur >= asize + MIN_BLOCK) {
        void *right = (uint8_t *)hdr_alloc + asize;
        hdr_write(hdr_alloc, asize, true);
        hdr_write(right, cur - asize, false);
        freelist_insert_front(right);
        coalesce_right_chain(right);
    }
    return true;
}


bool myinit(void *heap_start, size_t heap_size) {
    g_heap_base = NULL;
    g_heap_size = 0;
    g_free_head = NULL;
    if (heap_start == NULL) {
        return false;
    }
    if (((uintptr_t)heap_start) % ALIGNMENT != 0) {
        return false;
    }
    if (heap_size % ALIGNMENT != 0) {
        return false;
    }
    if (heap_size < MIN_BLOCK) {
        return false;
    }
    g_heap_base = (uint8_t *)heap_start;
    g_heap_size = heap_size;
    void *hdr = (void *)g_heap_base;
    hdr_write(hdr, heap_size, false);
    freelist_insert_front(hdr);
    return true;
}


void *mymalloc(size_t requested_size) {
    // Convert requested size to aligned block size
    size_t asize = request_to_asize(requested_size);
    if (asize == 0) {
        return NULL;
    }
    
    // First-fit search through free list
    for (void *p = g_free_head; p != NULL; p = free_next(p)) {
        if (blk_size(p) >= asize) {
            return allocate_from_free(p, asize);
        }
    }
    
    // No suitable free block found
    return NULL;
}



void myfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    
    // Get block header from payload pointer
    void *hdr = blk_from_payload(ptr);
    if (!ptr_in_heap(hdr)) {
        return;
    }
    
    // Mark block as free and add to free list
    size_t sz = blk_size(hdr);
    hdr_write(hdr, sz, false);
    freelist_insert_front(hdr);
    
    // Coalesce with adjacent free blocks
    coalesce_bidir(&hdr);
}


void *myrealloc(void *old_ptr, size_t new_size) {
    // Handle edge cases
    if (old_ptr == NULL) {
        return mymalloc(new_size);
    }
    if (new_size == 0) {
        myfree(old_ptr);
        return NULL;
    }
    
    // Validate the existing pointer
    void *hdr = blk_from_payload(old_ptr);
    if (!ptr_in_heap(hdr) || !blk_alloc(hdr)) {
        // Invalid pointer, just allocate new memory
        void *np = mymalloc(new_size);
        if (!np) {
            return NULL;
        }
        return np;
    }
    
    size_t asize = request_to_asize(new_size);
    size_t cur = blk_size(hdr);
    if (asize <= cur) {
        if (cur >= asize + MIN_BLOCK) {
            void *right = (uint8_t *)hdr + asize;
            hdr_write(hdr, asize, true);
            hdr_write(right, cur - asize, false);
            freelist_insert_front(right);
            coalesce_right_chain(right);
        }
        return old_ptr;
    }
    if (grow_in_place(hdr, asize)) {
        return old_ptr;
    }
    void *np2 = mymalloc(new_size);
    if (!np2) {
        return NULL;
    }
    size_t copy = (cur > HDR_SIZE) ? (cur - HDR_SIZE) : 0;
    if (copy > new_size) {
        copy = new_size;
    }
    memmove(np2, old_ptr, copy);
    myfree(old_ptr);
    return np2;
}


// Validate heap by walking through all blocks linearly
static bool validate_linear_walk(size_t *out_free_linear) {
    size_t walked = 0;
    size_t free_linear = 0;
    for (uint8_t *p = g_heap_base; p < heap_end();) {
        void *hdr = (void *)p;
        size_t sz = blk_size(hdr);
        bool al = blk_alloc(hdr);
        if (sz < MIN_BLOCK || sz % ALIGNMENT != 0) {
            breakpoint();
            return false;
        }
        if (!ptr_in_heap(hdr)) {
            breakpoint();
            return false;
        }
        void *n = blk_next(hdr);
        if (n != heap_end() && !al && !blk_alloc(n)) {
            breakpoint();
            return false;
        }
        if (!al) {
            free_linear++;
        }
        walked += sz;
        p += sz;
    }
    if (walked != g_heap_size) {
        breakpoint();
        return false;
    }
    *out_free_linear = free_linear;
    return true;
}

// Validate the free list for consistency and detect cycles
static bool validate_freelist(size_t expect_free_count) {
    size_t free_list_count = 0;
    void *slow = g_free_head;
    void *fast = g_free_head;
    while (slow) {
        if (!ptr_in_heap(slow)) {
            breakpoint();
            return false;
        }
        if (blk_alloc(slow)) {
            breakpoint();
            return false;
        }
        void *n = free_next(slow);
        void *p = free_prev(slow);
        if (p && free_next(p) != slow) {
            breakpoint();
            return false;
        }
        if (n && free_prev(n) != slow) {
            breakpoint();
            return false;
        }
        free_list_count++;
        slow = n;
        if (fast) {
            fast = free_next(fast);
        }
        if (fast) {
            fast = free_next(fast);
        }
        if (fast && fast == slow) {
            breakpoint();
            return false;
        }
    }
    if (expect_free_count != free_list_count) {
        breakpoint();
        return false;
    }
    return true;
}


bool validate_heap(void) {
    if (!g_heap_base || g_heap_size < MIN_BLOCK) {
        return false;
    }
    size_t free_linear = 0;
    if (!validate_linear_walk(&free_linear)) {
        return false;
    }
    if (!validate_freelist(free_linear)) {
        return false;
    }
    return true;
}

// Debug function to print the heap structure
void dump_heap(void) {
    printf("==== HEAP DUMP base=%p size=%zu free_head=%p ====\n", (void *)g_heap_base, g_heap_size, g_free_head);
    size_t i = 0;
    for (uint8_t *p = g_heap_base; p < heap_end();) {
        void *hdr = (void *)p;
        size_t sz = blk_size(hdr);
        if (blk_alloc(hdr)) {
            printf("[%04zu] %p  size=%6zu  ALLOC\n", i, hdr, sz);
        } else {
            printf("[%04zu] %p  size=%6zu  FREE", i, hdr, sz);
            printf("  prev=%p next=%p\n", free_prev(hdr), free_next(hdr));
        }
        p += sz;
        i++;
    }
    printf("==== END DUMP ====\n");
}

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>


#include "./allocator.h"
#include "./debug_break.h"
#include <string.h>


// cosntants
static uint8_t *heap_lo = NULL;
static uint8_t *heap_hi = NULL;

enum {
    HDR_SIZE = 8,
    FLAG_MASK = 0x7,   // lower 3 bits reserved for flags
    ALLOC_BIT =  0x1,   // allocation flag in bit 0
    MIN_PAYLOAD = 8,
    PREVIEW_BYTES = 16
};



// helper funcs
// allign to byte size
static inline size_t align_up(size_t n) {
    size_t r = ALIGNMENT - 1;
    if (n > SIZE_MAX - r) {
        return SIZE_MAX;  // overflow protection
    }
    return (n + r) & ~r;
}

// Raw header read/write
static inline size_t hdr_load(const void *hdrp) {
    return *(const size_t *)hdrp;
}

static inline void hdr_store(void *hdrp, size_t value) {
    *(size_t *)hdrp = value;
}


// Pack/unpack
static inline size_t pack(size_t total_block_size, bool allocated) {
    return (total_block_size & ~(size_t)FLAG_MASK) | (allocated ? ALLOC_BIT : 0);
}

static inline size_t block_size(const void *hdrp) {
    return hdr_load(hdrp) & ~(size_t)FLAG_MASK;
}

static inline bool is_alloc(const void *hdrp) {
    return (hdr_load(hdrp) & ALLOC_BIT) != 0;
}

// Pointer conversions 
static inline void *payload_from_hdr(void *hdrp) {
    return (uint8_t *)hdrp + HDR_SIZE;
}

static inline void *hdr_from_payload(void *payloadp) {
    return (uint8_t *)payloadp - HDR_SIZE;
}

// Traversal
static inline void *next_hdr(void *hdrp) {
    return (uint8_t *)hdrp + block_size(hdrp);
}

// Bounds
static inline bool in_heap(const void *p) {
    return (const uint8_t *)p >= heap_lo && (const uint8_t *)p <= heap_hi;
}

static inline bool aligned_ptr(const void *p) {
    return (((uintptr_t)p) & (ALIGNMENT - 1)) == 0;
}

// smallest reasonable size block
static inline size_t min_block_size(void) {
    return HDR_SIZE + MIN_PAYLOAD;
}


bool myinit(void *heap_start, size_t heap_size) {
    breakpoint();
    if (heap_start == NULL) {
        return false;
    }
    // Reset globals
    heap_lo = (uint8_t *)heap_start;
    
    breakpoint();
    // Trim heap to ALIGNMENT and sanity-check capacity
    size_t total = heap_size & ~(size_t)(ALIGNMENT - 1);
    if (total < min_block_size()) {
        return false;
    }

    // Compute hi after trimming and validate
    heap_hi = heap_lo + total;
    if (!aligned_ptr(heap_start)) {
        return false;  // should already be true per spec
    }
    if (((uintptr_t)heap_hi & (ALIGNMENT - 1)) != 0) {
        return false;
    }
    if (heap_hi < heap_lo) {
        return false;  // overflow guard
    }

    // One big free block covering the entire segment
    hdr_store(heap_lo, pack(total, false));

    return true;
}

void *mymalloc(size_t requested_size) {
    if (requested_size == 0) { 
        return NULL;
    }
    if (heap_lo == NULL || heap_hi == NULL) {
        return NULL;  // not initialized
    }

    // Align the payload; compute total size (header + payload) with overflow guard
    size_t need_payload = align_up(requested_size);
    if (need_payload > SIZE_MAX - HDR_SIZE) {
        return NULL;  // overflow check
    }
    size_t need_total = need_payload + HDR_SIZE;

    // First-fit search over implicit list
    for (uint8_t *hdr = heap_lo; hdr < heap_hi; hdr = (uint8_t *)next_hdr(hdr)) {
        size_t sz = block_size(hdr);
        bool a = is_alloc(hdr);

        if (!a && sz >= need_total) {
            size_t rem = sz - need_total;

            if (rem >= min_block_size()) {
                // Split: allocate front part, leave remainder as a free block
                hdr_store(hdr, pack(need_total, true));

                uint8_t *split_hdr = (uint8_t *)hdr + need_total;
                hdr_store(split_hdr, pack(rem, false));
            } else {
                // No useful split; consume the whole free block
                hdr_store(hdr, pack(sz, true));
            }

            return payload_from_hdr(hdr);
        }
    }

    // No fit
    return NULL;
}

void myfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    // Convert payload to header
    uint8_t *hdr = (uint8_t *)hdr_from_payload(ptr);

    // help while debugging.
    if (!in_heap(hdr) || !aligned_ptr(ptr)) {
        // breakpoint();
        return;
    }

    // Mark the block as FREE (keep the size)
    size_t sz = block_size(hdr);
    if (!is_alloc(hdr)) {
        // breakpoint();
        return;
    }
    hdr_store(hdr, pack(sz, false));
}

void *myrealloc(void *old_ptr, size_t new_size) {
    if (old_ptr == NULL) {
        return mymalloc(new_size);
    }
    if (new_size == 0) {
        myfree(old_ptr);
        return NULL;
    }

    uint8_t *old_hdr = (uint8_t *)hdr_from_payload(old_ptr);
    size_t old_total = block_size(old_hdr);  // header+payload
    size_t old_pay = (old_total >= HDR_SIZE) ? (old_total - HDR_SIZE) : 0;

    // Compute needed total (header + aligned payload)
    size_t need_pay = align_up(new_size);
    if (need_pay > SIZE_MAX - HDR_SIZE) {
        // overflow guard: can't satisfy
        return NULL;
    }
    size_t need_total = need_pay + HDR_SIZE;

    if (need_total <= old_total) {
        size_t rem = old_total - need_total;
        if (rem >= (HDR_SIZE + MIN_PAYLOAD)) {
            // Split: keep front as ALLOC, leave remainder as FREE^
            hdr_store(old_hdr, pack(need_total, true));
            uint8_t *split_hdr = old_hdr + need_total;
            hdr_store(split_hdr, pack(rem, false));
        }
        // If remainder too tiny, keep the current block size
        return old_ptr;
    }

    // Need a bigger block: allocate new, copy, free old
    void *new_ptr = mymalloc(new_size);
    if (new_ptr == NULL) {
        // Per realloc contract, old block stays valid on failure
        return NULL;
    }

    size_t to_copy = (old_pay < new_size) ? old_pay : new_size;
    memmove(new_ptr, old_ptr, to_copy);
    myfree(old_ptr);
    return new_ptr;
}

bool validate_heap() {
    if (heap_lo == NULL || heap_hi == NULL) {
        return false;
    }
    if (heap_hi < heap_lo) {
        return false;
    }

    // Alignment of bounds
    if ((((uintptr_t)heap_lo) & (ALIGNMENT - 1)) != 0) {
        return false;
    }
    if ((((uintptr_t)heap_hi) & (ALIGNMENT - 1)) != 0) {
        return false;
    }

    // Walk the heap by headers
    uint8_t *hdr = heap_lo;
    while (hdr < heap_hi) {
        if (!in_heap(hdr)) {
            return false;
        }

        size_t sz = block_size(hdr);
        // size must be aligned and large enough for a block
        if ((sz & (ALIGNMENT - 1)) != 0) {
            return false;
        }
        if (sz < (HDR_SIZE + MIN_PAYLOAD)) {
            return false;
        }

        // Step to next header
        if ((size_t)(heap_hi - hdr) < sz) {
            return false;  // overrun/overflow
        }
        hdr += sz;
    }

    // Must end exactly at heap_hi
    if (hdr != heap_hi) {
        return false;
    }
    return true;
}

/* Function: dump_heap
 * -------------------
 * This function prints out the block contents of the heap.  It is not
 * called anywhere, but is a useful helper function to call from gdb when
 * tracing through programs.  It prints out the total range of the heap, and
 * information about each block within it.
 */


static inline size_t clamp_preview(size_t pay, uint8_t *payload) {
    size_t max_bytes = (size_t)(heap_hi - payload);
    size_t n = (pay < PREVIEW_BYTES) ? pay : PREVIEW_BYTES;
    return (n > max_bytes) ? max_bytes : n;
}

static void print_payload(uint8_t *payload, size_t pay) {
    size_t preview = clamp_preview(pay, payload);
    printf("data: ");
    for (size_t i = 0; i < preview; i++) {
        printf("%02x ", payload[i]);
    }
    if (pay > preview) {
        printf("...");
    }
    printf("\n");
}

static bool block_corrupt(uint8_t *hdr, size_t sz, uint8_t *next) {
    return sz < (HDR_SIZE + MIN_PAYLOAD)  || (sz & (ALIGNMENT - 1)) != 0 || next <= hdr || next > heap_hi;
}


void dump_heap(void) {
    if (heap_lo == NULL || heap_hi == NULL) {
        printf("HEAP not initialized\n");
        return;
    }

    size_t total = (size_t)(heap_hi - heap_lo);
    printf("HEAP [%p .. %p) total=%zu bytes\n", heap_lo, heap_hi, total);

    size_t idx = 0;
    uint8_t *hdr = heap_lo;

    while (hdr < heap_hi) {
        size_t raw = hdr_load(hdr);
        size_t sz = block_size(hdr);
        bool a = is_alloc(hdr);
        size_t pay = (sz >= HDR_SIZE) ? (sz - HDR_SIZE) : 0;
        uint8_t *payload = (uint8_t *)payload_from_hdr(hdr);
        uint8_t *next = hdr + sz;
        size_t offset = (size_t)(hdr - heap_lo);

        printf("#%04zu off=%8zu  hdr=%p  raw=0x%016lx  size=%8zu  payload=%8zu  %s  next=%p\n",
               idx, offset, hdr, (unsigned long)raw, sz, pay, a ? "ALLOC" : "FREE", next);

        if (pay > 0 && in_heap(payload)) {
            print_payload(payload, pay);
        }

        if (block_corrupt(hdr, sz, next)) {
            printf("  !! Corrupt block encountered (size/alignment/overrun). Stopping dump.\n");
            break;
        }

        hdr = next;
        idx++;
    }
}

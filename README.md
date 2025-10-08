# Heap Allocators

I built three heap allocators in C following the Stanford Course CS107 during my time there in Summer 2025.

Used GDB, GCC, valgrind, assembly code, and callgrind. Developed source code using Ubuntu, a linux terminal, and EMACS, a text editor.

## Allocator Implementations

### 1. Bump Allocator (bump.c)

The simplest allocator that demonstrates the basic principles of heap management without any sophisticated memory reuse.

### 2. Implicit Free List Allocator (implicit.c)

An intermediate allocator that uses block headers to track allocation status and enables memory reuse through a first-fit search algorithm.

### 3. Explicit Free List Allocator (explicit.c)

The most sophisticated implementation that maintains a doubly-linked list of free blocks for efficient allocation and includes bidirectional coalescing.

## Core Principles

### Memory Alignment

All allocators maintain strict alignment requirements (8-byte alignment) to ensure proper memory access and optimal performance across different architectures. Block sizes and addresses are always rounded up to alignment boundaries.

### Block Headers

Each memory block contains metadata stored in a header that tracks:

- Block size (stored with lower bits masked for flags)
- Allocation status (allocated or free)
- In the explicit allocator: pointers to previous/next free blocks

### Allocation Strategies

**Bump Allocator:**

- Sequential allocation only - places new blocks at the end of the heap
- No memory reuse or recycling
- Fast allocation but extremely poor memory utilization
- Free is a no-op

**Implicit Free List:**

- First-fit search through all blocks in linear order
- Maintains allocation status in block headers
- Enables block splitting when a free block is larger than needed
- No coalescing of adjacent free blocks

**Explicit Free List:**

- Maintains a doubly-linked list of only free blocks
- First-fit search through the free list (much faster than implicit)
- Bidirectional coalescing merges adjacent free blocks immediately
- Block splitting creates new free blocks when excess space remains

### Memory Reuse and Fragmentation Management

**Splitting:**
When a free block is larger than the requested size by at least the minimum block size, it is split into:

- An allocated block of the requested size
- A new free block containing the remainder

**Coalescing (Explicit allocator only):**
When a block is freed, it is immediately merged with:

- Adjacent free blocks to the right (forward coalescing)
- Adjacent free block to the left if present (backward coalescing)
  This reduces external fragmentation and improves memory utilization.

### Realloc Optimization

All allocators support memory resizing with optimizations:

- Shrinking: splits the block if enough space can be reclaimed
- Growing (explicit): attempts in-place expansion by absorbing adjacent free blocks before allocating a new block
- Falls back to malloc/copy/free pattern when in-place modification is not possible

### Heap Consistency Validation

Each allocator implements validation checks:

- Verifying alignment of all blocks and pointers
- Ensuring blocks don't overflow heap boundaries
- Checking that block sizes are valid and properly aligned
- In explicit allocator: validating free list integrity, detecting cycles, and ensuring no adjacent free blocks exist

### Robustness

The allocators handle edge cases:

- Null pointer checks in free and realloc
- Zero-size allocation requests
- Integer overflow protection in size calculations
- Validation of pointer boundaries before dereferencing
- Proper handling of requests that exceed available memory

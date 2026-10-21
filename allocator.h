#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

/* Placement strategies (for the list-based allocator) */
typedef enum {
    FIRST_FIT,
    NEXT_FIT,
    BEST_FIT,
    WORST_FIT,
    EXPLICIT_FIT,
    NUM_STRATEGIES
} strategy_t;

/* Current heap statistics (computed by walking the heap) */
typedef struct {
    size_t heap_size;        /* total arena size in bytes */
    size_t free_bytes;       /* total free space (including headers/footers) */
    size_t largest_free;     /* largest single contiguous free block */
    size_t used_block_bytes; /* total size of allocated blocks (including overhead) */
    size_t requested_bytes;  /* total payload bytes requested by user */
    size_t n_alloc_blocks;
    size_t n_free_blocks;
    size_t errors;           /* count of caught invalid/double free/realloc errors */
} alloc_stats_t;

typedef struct {
    size_t offset;
    size_t size;
    size_t requested;
    int allocated;
} alloc_block_info_t;

/* Initializes (or re-initializes) the 1 MiB arena. Returns 0 on success. */
int  allocator_init(strategy_t s);
void allocator_destroy(void);
const char *strategy_name(strategy_t s);

void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_calloc(size_t n, size_t size);
void *my_realloc(void *ptr, size_t size);
void *my_aligned_alloc(size_t alignment, size_t size);
size_t my_usable_size(void *ptr);

/* Metrics */
void   allocator_get_stats(alloc_stats_t *out);
size_t allocator_get_blocks(alloc_block_info_t *out, size_t capacity);
double external_fragmentation(const alloc_stats_t *s); /* 1 - largest_free / free_bytes */
double internal_fragmentation(const alloc_stats_t *s); /* (block - requested) / block */

/* Debugging & Introspection */
int    heap_check(int verbose);     /* 0: valid, -1: corrupted (prints to stderr) */
void   heap_map(int width);         /* ASCII map: # allocated, . free */
void   heap_dump(void);             /* dumps all blocks */
size_t allocator_report_leaks(void);/* reports unfreed blocks, returns leaked bytes */

#endif

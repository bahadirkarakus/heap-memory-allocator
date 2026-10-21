#ifndef BUDDY_ALLOCATOR_H
#define BUDDY_ALLOCATOR_H

#include <stddef.h>

typedef struct {
    size_t heap_size;
    size_t free_bytes;
    size_t largest_free;
    size_t used_block_bytes;
    size_t requested_bytes;
    size_t n_alloc_blocks;
    size_t n_free_blocks;
    size_t errors;
} buddy_stats_t;

typedef struct {
    size_t offset;
    size_t size;
    size_t requested;
    int allocated;
} buddy_block_info_t;

int buddy_init(void);
void buddy_destroy(void);
void *buddy_malloc(size_t size);
void buddy_free(void *ptr);
void *buddy_calloc(size_t n, size_t size);
void *buddy_realloc(void *ptr, size_t size);
void buddy_get_stats(buddy_stats_t *out);
size_t buddy_get_blocks(buddy_block_info_t *out, size_t capacity);
double buddy_external_fragmentation(const buddy_stats_t *stats);
double buddy_internal_fragmentation(const buddy_stats_t *stats);
int buddy_check(int verbose);
void buddy_map(int width);

#endif

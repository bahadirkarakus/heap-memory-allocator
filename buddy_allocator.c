#define _GNU_SOURCE
#include "buddy_allocator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define BUDDY_HEAP_SIZE (1u << 20)
#define MIN_ORDER 5
#define MAX_ORDER 20
#define MIN_BLOCK (1u << MIN_ORDER)
#define MAGIC UINT32_C(0xBADDCAFE)

/* 16-byte header => payload (block + 16) is always 16-byte aligned */
typedef struct buddy_header {
    uint32_t magic;
    uint16_t order;
    uint16_t allocated;
    size_t   requested;
} buddy_header_t;

typedef struct free_node {
    struct free_node *next;
} free_node_t;

static char *arena;
static free_node_t *free_lists[MAX_ORDER + 1];
static size_t error_count;

static pthread_mutex_t buddy_lock;
static pthread_once_t  buddy_once = PTHREAD_ONCE_INIT;

static void init_buddy_lock(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&buddy_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static inline void lock_buddy(void)
{
    pthread_once(&buddy_once, init_buddy_lock);
    pthread_mutex_lock(&buddy_lock);
}

static inline void unlock_buddy(void)
{
    pthread_mutex_unlock(&buddy_lock);
}

static size_t order_size(unsigned order) { return (size_t)1 << order; }

static free_node_t *node_at(void *block)
{
    return (free_node_t *)((char *)block + sizeof(buddy_header_t));
}

static unsigned required_order(size_t size)
{
    if (size > BUDDY_HEAP_SIZE - sizeof(buddy_header_t)) return 0;
    size_t total = size + sizeof(buddy_header_t);
    unsigned order = MIN_ORDER;
    while (order < MAX_ORDER && order_size(order) < total) order++;
    return order;
}

static void list_push(unsigned order, void *block)
{
    buddy_header_t *header = block;
    header->magic = MAGIC;
    header->order = (uint16_t)order;
    header->allocated = 0;
    header->requested = 0;
    free_node_t *node = node_at(block);
    node->next = free_lists[order];
    free_lists[order] = node;
}

static void *list_pop(unsigned order)
{
    free_node_t *node = free_lists[order];
    if (node) free_lists[order] = node->next;
    return (char *)node - sizeof(buddy_header_t);
}

static int list_remove(unsigned order, void *block)
{
    free_node_t **current = &free_lists[order];
    while (*current) {
        if (*current == node_at(block)) {
            *current = (*current)->next;
            return 1;
        }
        current = &(*current)->next;
    }
    return 0;
}

int buddy_init(void)
{
    lock_buddy();
    buddy_destroy();
    void *memory = mmap(NULL, BUDDY_HEAP_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { unlock_buddy(); return -1; }
    arena = memory;
    memset(free_lists, 0, sizeof free_lists);
    error_count = 0;
    list_push(MAX_ORDER, arena);
    unlock_buddy();
    return 0;
}

void buddy_destroy(void)
{
    lock_buddy();
    if (arena) munmap(arena, BUDDY_HEAP_SIZE);
    arena = NULL;
    memset(free_lists, 0, sizeof free_lists);
    unlock_buddy();
}

void *buddy_malloc(size_t size)
{
    lock_buddy();
    if (!arena && buddy_init() != 0) { unlock_buddy(); return NULL; }
    if (size == 0) { unlock_buddy(); return NULL; }
    unsigned wanted = required_order(size);
    if (wanted == 0) { unlock_buddy(); return NULL; }

    unsigned order = wanted;
    while (order <= MAX_ORDER && !free_lists[order]) order++;
    if (order > MAX_ORDER) { unlock_buddy(); return NULL; }

    char *block = list_pop(order);
    while (order > wanted) {
        order--;
        char *buddy = block + order_size(order);
        list_push(order, buddy);
    }

    buddy_header_t *header = (buddy_header_t *)block;
    header->magic = MAGIC;
    header->order = (uint16_t)wanted;
    header->allocated = 1;
    header->requested = size;
    unlock_buddy();
    return block + sizeof(*header);
}

static buddy_header_t *header_from_ptr(void *ptr)
{
    if (!arena || !ptr) return NULL;
    char *payload = ptr;
    if (((uintptr_t)payload & 15) != 0) return NULL;
    if (payload < arena + sizeof(buddy_header_t) ||
        payload >= arena + BUDDY_HEAP_SIZE)
        return NULL;
    buddy_header_t *header = (buddy_header_t *)(payload - sizeof(*header));
    if (header->magic != MAGIC || header->order < MIN_ORDER ||
        header->order > MAX_ORDER || !header->allocated)
        return NULL;
    char *block = (char *)header;
    size_t size = order_size(header->order);
    if (block < arena || block + size > arena + BUDDY_HEAP_SIZE ||
        (size_t)(block - arena) % size != 0)
        return NULL;
    return header;
}

void buddy_free(void *ptr)
{
    if (!ptr) return;
    lock_buddy();
    buddy_header_t *header = header_from_ptr(ptr);
    if (!header) {
        fprintf(stderr, "buddy_free: invalid or double-free pointer %p\n", ptr);
        error_count++;
        unlock_buddy();
        return;
    }

    unsigned order = header->order;
    char *block = (char *)header;
    header->allocated = 0;
    header->requested = 0;
    header->magic = MAGIC;

    while (order < MAX_ORDER) {
        size_t offset = (size_t)(block - arena);
        char *buddy = arena + (offset ^ order_size(order));
        buddy_header_t *buddy_header = (buddy_header_t *)buddy;
        if (buddy_header->magic != MAGIC || buddy_header->allocated ||
            buddy_header->order != order || !list_remove(order, buddy))
            break;
        if (buddy < block) block = buddy;
        order++;
    }
    list_push(order, block);
    unlock_buddy();
}

void *buddy_calloc(size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n) return NULL;
    size_t total = n * size;
    void *ptr = buddy_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *buddy_realloc(void *ptr, size_t size)
{
    if (!ptr) return buddy_malloc(size);
    if (size == 0) {
        buddy_free(ptr);
        return NULL;
    }
    lock_buddy();
    buddy_header_t *header = header_from_ptr(ptr);
    if (!header) {
        error_count++;
        unlock_buddy();
        return NULL;
    }
    size_t old_size = header->requested;
    unsigned wanted = required_order(size);
    if (wanted == 0) {
        unlock_buddy();
        return NULL;
    }

    /* 1) Current block is large enough; if shrinking, return upper buddies to free lists */
    if (wanted <= header->order) {
        while (header->order > wanted) {
            header->order--;
            char *buddy = (char *)header + order_size(header->order);
            list_push(header->order, buddy);
        }
        header->requested = size;
        unlock_buddy();
        return ptr;
    }

    /* 2) Expand in-place if right buddy(ies) are free */
    char *block = (char *)header;
    unsigned cur_order = header->order;
    while (cur_order < wanted && cur_order < MAX_ORDER) {
        size_t offset = (size_t)(block - arena);
        if (offset & order_size(cur_order)) break; /* must be left buddy to expand in-place */
        char *buddy = arena + (offset ^ order_size(cur_order));
        buddy_header_t *bh = (buddy_header_t *)buddy;
        if (bh->magic != MAGIC || bh->allocated || bh->order != cur_order) break;
        cur_order++;
    }
    if (cur_order >= wanted) {
        while (header->order < wanted) {
            size_t offset = (size_t)(block - arena);
            char *buddy = arena + (offset ^ order_size(header->order));
            list_remove(header->order, buddy);
            header->order++;
        }
        header->requested = size;
        unlock_buddy();
        return ptr;
    }

    /* 3) Allocate new block, copy payload, free old block */
    void *new_ptr = buddy_malloc(size);
    if (!new_ptr) { unlock_buddy(); return NULL; }
    memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    buddy_free(ptr);
    unlock_buddy();
    return new_ptr;
}

void buddy_get_stats(buddy_stats_t *stats)
{
    lock_buddy();
    memset(stats, 0, sizeof(*stats));
    stats->errors = error_count;
    if (!arena) { unlock_buddy(); return; }
    stats->heap_size = BUDDY_HEAP_SIZE;
    for (unsigned order = MIN_ORDER; order <= MAX_ORDER; order++) {
        for (free_node_t *node = free_lists[order]; node; node = node->next) {
            stats->n_free_blocks++;
            stats->free_bytes += order_size(order);
            if (order_size(order) > stats->largest_free)
                stats->largest_free = order_size(order);
        }
    }
    for (size_t offset = 0; offset < BUDDY_HEAP_SIZE;) {
        buddy_header_t *header = (buddy_header_t *)(arena + offset);
        if (header->magic != MAGIC || header->order < MIN_ORDER ||
            header->order > MAX_ORDER)
            break;
        size_t block_size = order_size(header->order);
        if (header->allocated) {
            stats->n_alloc_blocks++;
            stats->used_block_bytes += block_size;
            stats->requested_bytes += header->requested;
        }
        offset += block_size;
    }
    unlock_buddy();
}

size_t buddy_get_blocks(buddy_block_info_t *out, size_t capacity)
{
    lock_buddy();
    size_t count = 0;
    if (!arena) { unlock_buddy(); return 0; }
    for (size_t offset = 0; offset < BUDDY_HEAP_SIZE;) {
        buddy_header_t *header = (buddy_header_t *)(arena + offset);
        if (header->magic != MAGIC || header->order < MIN_ORDER ||
            header->order > MAX_ORDER)
            break;
        size_t block_size = order_size(header->order);
        if (out && count < capacity) {
            out[count].offset = offset;
            out[count].size = block_size;
            out[count].requested = header->allocated ? header->requested : 0;
            out[count].allocated = header->allocated;
        }
        count++;
        offset += block_size;
    }
    unlock_buddy();
    return count;
}

double buddy_external_fragmentation(const buddy_stats_t *stats)
{
    return stats->free_bytes == 0
        ? 0.0
        : 1.0 - (double)stats->largest_free / stats->free_bytes;
}

double buddy_internal_fragmentation(const buddy_stats_t *stats)
{
    return stats->used_block_bytes == 0
        ? 0.0
        : (double)(stats->used_block_bytes - stats->requested_bytes) /
          stats->used_block_bytes;
}

int buddy_check(int verbose)
{
    lock_buddy();
    if (!arena) { unlock_buddy(); return -1; }
    size_t free_list_bytes = 0;
    for (unsigned order = MIN_ORDER; order <= MAX_ORDER; order++) {
        for (free_node_t *node = free_lists[order]; node; node = node->next) {
            char *block = (char *)node - sizeof(buddy_header_t);
            if (block < arena || block + order_size(order) > arena + BUDDY_HEAP_SIZE ||
                (size_t)(block - arena) % order_size(order) != 0) {
                unlock_buddy();
                return -1;
            }
            buddy_header_t *h = (buddy_header_t *)block;
            if (h->magic != MAGIC || h->allocated || h->order != order) {
                unlock_buddy();
                return -1;
            }
            free_list_bytes += order_size(order);
        }
    }

    size_t walked_free_bytes = 0, walked_alloc_bytes = 0;
    size_t offset = 0;
    while (offset < BUDDY_HEAP_SIZE) {
        buddy_header_t *h = (buddy_header_t *)(arena + offset);
        if (h->magic != MAGIC || h->order < MIN_ORDER || h->order > MAX_ORDER) {
            unlock_buddy();
            return -1;
        }
        size_t sz = order_size(h->order);
        if (offset % sz != 0 || offset + sz > BUDDY_HEAP_SIZE) {
            unlock_buddy();
            return -1;
        }
        if (h->allocated) {
            walked_alloc_bytes += sz;
        } else {
            walked_free_bytes += sz;
            /* Coalesce invariant: a free block's buddy cannot also be free at the same order */
            if (h->order < MAX_ORDER) {
                size_t buddy_off = offset ^ sz;
                buddy_header_t *bh = (buddy_header_t *)(arena + buddy_off);
                if (bh->magic == MAGIC && !bh->allocated && bh->order == h->order) {
                    unlock_buddy();
                    return -1;
                }
            }
        }
        offset += sz;
    }

    if (offset != BUDDY_HEAP_SIZE ||
        walked_free_bytes != free_list_bytes ||
        walked_free_bytes + walked_alloc_bytes != BUDDY_HEAP_SIZE) {
        unlock_buddy();
        return -1;
    }

    if (verbose) {
        buddy_stats_t stats;
        buddy_get_stats(&stats);
        printf("buddy: free=%zu allocated=%zu free-list-bytes=%zu\n",
               stats.free_bytes, stats.used_block_bytes, free_list_bytes);
    }
    unlock_buddy();
    return 0;
}

void buddy_map(int width)
{
    lock_buddy();
    if (!arena || width <= 0) { unlock_buddy(); return; }
    putchar('[');
    for (int i = 0; i < width; i++) {
        size_t target = (size_t)i * BUDDY_HEAP_SIZE / width;
        size_t off = 0;
        int alloc = 0;
        while (off < BUDDY_HEAP_SIZE) {
            buddy_header_t *h = (buddy_header_t *)(arena + off);
            size_t sz = order_size(h->order);
            if (off + sz > target) {
                alloc = h->allocated;
                break;
            }
            off += sz;
        }
        putchar(alloc ? '#' : '.');
    }
    printf("]  (1 char = %d bytes)\n", BUDDY_HEAP_SIZE / width);
    unlock_buddy();
}

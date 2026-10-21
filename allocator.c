/*
 * List-based heap allocator (implicit/explicit list + boundary tag).
 *
 * Block layout (16-byte aligned address, block size is a multiple of 16):
 *
 *   +--------------------+--------------------+----------------+--------------------+
 *   | size | alloc bit   | requested (8 byte) |  payload ...   | footer = header    |
 *   |      (8 byte)      |                    |                |     (8 byte)       |
 *   +--------------------+--------------------+----------------+--------------------+
 *   ^ block start        ^ +8                  ^ +16 (16-byte aligned user payload)
 *
 * - size: total block size (header + payload + footer), lower 4 bits reserved for flags.
 * - requested: user requested size (for internal fragmentation measurement).
 * - footer: copy of size_alloc. Enables O(1) coalescing with previous physical block.
 */

#define _GNU_SOURCE
#include "allocator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define ALIGNMENT   16
#define HDR_SIZE    16
#define FTR_SIZE    8
#define OVERHEAD    (HDR_SIZE + FTR_SIZE)
#define MIN_BLOCK   32
#define ARENA_SIZE  (1u << 20) /* 1 MB */

typedef struct hdr {
    size_t size_alloc; /* block size | alloc bit */
    size_t requested;  /* allocated: requested size; free: prev_free pointer */
} hdr_t;

/* ---- global state ---- */
static char       *arena      = NULL;
static hdr_t      *heap_start = NULL;
static char       *heap_end   = NULL;
static hdr_t      *rover      = NULL; /* for next fit */
static hdr_t      *free_head  = NULL; /* explicit free list (LIFO, doubly-linked) */
static strategy_t  strategy   = FIRST_FIT;
static size_t      err_count  = 0;

/* Recursive mutex for thread safety */
static pthread_mutex_t alloc_lock;
static pthread_once_t  lock_once = PTHREAD_ONCE_INIT;

static void init_recursive_lock(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&alloc_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static inline void lock_heap(void)
{
    pthread_once(&lock_once, init_recursive_lock);
    pthread_mutex_lock(&alloc_lock);
}

static inline void unlock_heap(void)
{
    pthread_mutex_unlock(&alloc_lock);
}

/* ---- block helpers ---- */
static inline size_t blk_size(const hdr_t *h)  { return h->size_alloc & ~(size_t)15; }
static inline int    blk_alloc(const hdr_t *h) { return (int)(h->size_alloc & 1); }
static inline size_t *footer_of(hdr_t *h)      { return (size_t *)((char *)h + blk_size(h) - FTR_SIZE); }
static inline hdr_t *next_blk(hdr_t *h)        { return (hdr_t *)((char *)h + blk_size(h)); }
static inline int    has_next(hdr_t *h)        { return (char *)next_blk(h) < heap_end; }
static inline void  *payload_of(hdr_t *h)      { return (char *)h + HDR_SIZE; }

/* Doubly-linked list pointers stored in free blocks (offset 8 and offset 16) */
static inline hdr_t **free_prev_slot(hdr_t *h) { return (hdr_t **)&h->requested; }
static inline hdr_t **free_next_slot(hdr_t *h) { return (hdr_t **)payload_of(h); }

static void free_list_insert(hdr_t *h)
{
    *free_prev_slot(h) = NULL;
    *free_next_slot(h) = free_head;
    if (free_head) *free_prev_slot(free_head) = h;
    free_head = h;
}

static void free_list_remove(hdr_t *h)
{
    hdr_t *prev = *free_prev_slot(h);
    hdr_t *next = *free_next_slot(h);
    if (prev) *free_next_slot(prev) = next;
    else      free_head = next;
    if (next) *free_prev_slot(next) = prev;
    *free_prev_slot(h) = NULL;
    *free_next_slot(h) = NULL;
}

static void set_block(hdr_t *h, size_t size, int alloc, size_t requested)
{
    h->size_alloc = size | (size_t)(alloc ? 1 : 0);
    h->requested  = requested;
    *footer_of(h) = h->size_alloc;
}

static inline size_t align_up(size_t n) { return (n + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1); }

/* Required total block size for payload; returns 0 on overflow */
static size_t needed_block(size_t size)
{
    if (size > ARENA_SIZE - OVERHEAD) return 0;
    size_t need = align_up(size + OVERHEAD);
    return need < MIN_BLOCK ? MIN_BLOCK : need;
}

/* ---- init / destroy ---- */
const char *strategy_name(strategy_t s)
{
    static const char *names[] = {
        "first_fit", "next_fit", "best_fit", "worst_fit", "explicit_fit"
    };
    return s < NUM_STRATEGIES ? names[s] : "unknown";
}

void allocator_destroy(void)
{
    lock_heap();
    if (arena) munmap(arena, ARENA_SIZE);
    arena = NULL; heap_start = NULL; heap_end = NULL; rover = NULL; free_head = NULL;
    unlock_heap();
}

int allocator_init(strategy_t s)
{
    lock_heap();
    allocator_destroy();
    void *p = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { unlock_heap(); return -1; }

    arena      = p;                       /* page-aligned => 16-byte aligned */
    heap_start = (hdr_t *)arena;
    heap_end   = arena + ARENA_SIZE;
    strategy   = s;
    err_count  = 0;
    free_head  = NULL;
    set_block(heap_start, ARENA_SIZE, 0, 0); /* single large free block */
    free_list_insert(heap_start);
    rover = heap_start;
    unlock_heap();
    return 0;
}

/* ---- placement search strategies ---- */
static hdr_t *find_first(size_t need)
{
    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b))
        if (!blk_alloc(b) && blk_size(b) >= need) return b;
    return NULL;
}

static hdr_t *find_next(size_t need)
{
    hdr_t *start = rover ? rover : heap_start;
    hdr_t *b = start;
    do {
        if (!blk_alloc(b) && blk_size(b) >= need) return b;
        b = next_blk(b);
        if ((char *)b >= heap_end) b = heap_start;
    } while (b != start);
    return NULL;
}

static hdr_t *find_best(size_t need)
{
    hdr_t *best = NULL;
    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b)) {
        if (blk_alloc(b) || blk_size(b) < need) continue;
        if (!best || blk_size(b) < blk_size(best)) {
            best = b;
            if (blk_size(b) == need) break; /* exact match */
        }
    }
    return best;
}

static hdr_t *find_worst(size_t need)
{
    hdr_t *worst = NULL;
    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b))
        if (!blk_alloc(b) && blk_size(b) >= need &&
            (!worst || blk_size(b) > blk_size(worst)))
            worst = b;
    return worst;
}

static hdr_t *find_explicit(size_t need)
{
    for (hdr_t *b = free_head; b != NULL; b = *free_next_slot(b))
        if (blk_size(b) >= need) return b;
    return NULL;
}

static hdr_t *find_fit(size_t need)
{
    switch (strategy) {
    case FIRST_FIT:    return find_first(need);
    case NEXT_FIT:     return find_next(need);
    case BEST_FIT:     return find_best(need);
    case WORST_FIT:    return find_worst(need);
    case EXPLICIT_FIT: return find_explicit(need);
    default:           return NULL;
    }
}

/* ---- split + placement ---- */
static void place(hdr_t *h, size_t need, size_t requested)
{
    size_t total = blk_size(h);
    free_list_remove(h);
    if (total - need >= MIN_BLOCK) {          /* split remainder into a new free block */
        set_block(h, need, 1, requested);
        hdr_t *rest = next_blk(h);
        set_block(rest, total - need, 0, 0);
        free_list_insert(rest);
        rover = rest;
    } else {                                  /* internal fragmentation: use entire block */
        set_block(h, total, 1, requested);
        rover = has_next(h) ? next_blk(h) : heap_start;
    }
}

/* ---- coalesce: merge free block with adjacent free neighbors ---- */
static hdr_t *coalesce(hdr_t *h)
{
    size_t size = blk_size(h);

    if (has_next(h)) {
        hdr_t *n = next_blk(h);
        if (!blk_alloc(n)) {
            free_list_remove(n);
            if (rover == n) rover = h;
            size += blk_size(n);
        }
    }
    if (h != heap_start) {
        size_t psize = *(size_t *)((char *)h - FTR_SIZE) & ~(size_t)15;
        hdr_t *p = (hdr_t *)((char *)h - psize);
        if (!blk_alloc(p)) {
            free_list_remove(p);
            if (rover == h) rover = p;
            h = p;
            size += psize;
        }
    }
    set_block(h, size, 0, 0);
    free_list_insert(h);
    return h;
}

/* ---- public API ---- */
void *my_malloc(size_t size)
{
    lock_heap();
    if (!arena && allocator_init(FIRST_FIT) != 0) { unlock_heap(); return NULL; }
    if (size == 0) { unlock_heap(); return NULL; }

    size_t need = needed_block(size);
    if (!need) { unlock_heap(); return NULL; }

    hdr_t *h = find_fit(need);
    if (!h) { unlock_heap(); return NULL; }
    place(h, need, size);
    void *res = payload_of(h);
    unlock_heap();
    return res;
}

/* Checks whether p points to a valid block payload inside this arena */
static int valid_ptr(void *p)
{
    char *c = p;
    if (!arena || ((uintptr_t)c & (ALIGNMENT - 1))) return 0;
    if (c - HDR_SIZE < (char *)heap_start || c >= heap_end) return 0;
    hdr_t *h = (hdr_t *)(c - HDR_SIZE);
    size_t sz = blk_size(h);
    if (sz < MIN_BLOCK || (sz & (ALIGNMENT - 1)) || (char *)h + sz > heap_end) return 0;
    if (*footer_of(h) != h->size_alloc) return 0; /* corrupted boundary tag */
    return 1;
}

void my_free(void *ptr)
{
    if (!ptr) return;
    lock_heap();
    if (!valid_ptr(ptr)) {
        fprintf(stderr, "my_free: invalid pointer %p\n", ptr);
        err_count++;
        unlock_heap();
        return;
    }
    hdr_t *h = (hdr_t *)((char *)ptr - HDR_SIZE);
    if (!blk_alloc(h)) {
        fprintf(stderr, "my_free: double free %p\n", ptr);
        err_count++;
        unlock_heap();
        return;
    }
    set_block(h, blk_size(h), 0, 0);
    coalesce(h);
    unlock_heap();
}

void *my_calloc(size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n) return NULL; /* multiplication overflow */
    size_t total = n * size;
    void *p = my_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *my_realloc(void *ptr, size_t size)
{
    if (!ptr) return my_malloc(size);
    if (size == 0) { my_free(ptr); return NULL; }

    lock_heap();
    if (!valid_ptr(ptr)) {
        fprintf(stderr, "my_realloc: invalid pointer %p\n", ptr);
        err_count++;
        unlock_heap();
        return NULL;
    }

    hdr_t *h = (hdr_t *)((char *)ptr - HDR_SIZE);
    if (!blk_alloc(h)) {
        fprintf(stderr, "my_realloc: pointer to free block %p\n", ptr);
        err_count++;
        unlock_heap();
        return NULL;
    }

    size_t need = needed_block(size);
    if (!need) { unlock_heap(); return NULL; }
    size_t cur = blk_size(h);

    /* 1) Current block is large enough (split and coalesce remainder if shrinking) */
    if (need <= cur) {
        if (cur - need >= MIN_BLOCK) {
            set_block(h, need, 1, size);
            hdr_t *rest = next_blk(h);
            set_block(rest, cur - need, 0, 0);
            rover = coalesce(rest);
        } else {
            h->requested = size;
        }
        unlock_heap();
        return ptr;
    }

    /* 2) Expand in-place if the next block is free and large enough */
    if (has_next(h)) {
        hdr_t *n = next_blk(h);
        if (!blk_alloc(n) && cur + blk_size(n) >= need) {
            size_t total = cur + blk_size(n);
            free_list_remove(n);
            if (rover == n) rover = h;
            if (total - need >= MIN_BLOCK) {
                set_block(h, need, 1, size);
                hdr_t *rest = next_blk(h);
                set_block(rest, total - need, 0, 0);
                free_list_insert(rest);
                rover = rest;
            } else {
                set_block(h, total, 1, size);
            }
            unlock_heap();
            return ptr;
        }
    }

    /* 3) Allocate new block, copy payload, free old block */
    size_t old_req = h->requested;
    void *np = my_malloc(size);
    if (!np) { unlock_heap(); return NULL; }
    memcpy(np, ptr, old_req < size ? old_req : size);
    my_free(ptr);
    unlock_heap();
    return np;
}

void *my_aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || size == 0)
        return NULL;
    if (alignment <= ALIGNMENT)
        return my_malloc(size);

    lock_heap();
    if (!arena && allocator_init(FIRST_FIT) != 0) { unlock_heap(); return NULL; }

    size_t need = needed_block(size);
    if (!need) { unlock_heap(); return NULL; }

    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b)) {
        if (blk_alloc(b)) continue;
        size_t total = blk_size(b);
        if (total < need) continue;

        uintptr_t p_addr = (uintptr_t)payload_of(b);
        uintptr_t aligned_p = (p_addr + alignment - 1) & ~(uintptr_t)(alignment - 1);
        size_t front_pad = (size_t)(aligned_p - p_addr);

        while (front_pad > 0 && front_pad < MIN_BLOCK) {
            aligned_p += alignment;
            front_pad = (size_t)(aligned_p - p_addr);
        }
        if (front_pad + need > total) continue;

        if (front_pad > 0) {
            free_list_remove(b);
            hdr_t *aligned_blk = (hdr_t *)((char *)b + front_pad);
            set_block(b, front_pad, 0, 0);
            free_list_insert(b);
            set_block(aligned_blk, total - front_pad, 0, 0);
            free_list_insert(aligned_blk);
            b = aligned_blk;
        }
        place(b, need, size);
        void *res = payload_of(b);
        unlock_heap();
        return res;
    }
    unlock_heap();
    return NULL;
}

size_t my_usable_size(void *ptr)
{
    if (!ptr) return 0;
    lock_heap();
    if (!valid_ptr(ptr)) { unlock_heap(); return 0; }
    hdr_t *h = (hdr_t *)((char *)ptr - HDR_SIZE);
    size_t usable = blk_alloc(h) ? (blk_size(h) - OVERHEAD) : 0;
    unlock_heap();
    return usable;
}

/* ---- metrics ---- */
void allocator_get_stats(alloc_stats_t *s)
{
    lock_heap();
    memset(s, 0, sizeof *s);
    s->errors = err_count;
    if (!arena) { unlock_heap(); return; }
    s->heap_size = ARENA_SIZE;
    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b)) {
        size_t sz = blk_size(b);
        if (blk_alloc(b)) {
            s->n_alloc_blocks++;
            s->used_block_bytes += sz;
            s->requested_bytes  += b->requested;
        } else {
            s->n_free_blocks++;
            s->free_bytes += sz;
            if (sz > s->largest_free) s->largest_free = sz;
        }
    }
    unlock_heap();
}

size_t allocator_get_blocks(alloc_block_info_t *out, size_t capacity)
{
    lock_heap();
    size_t count = 0;
    if (!arena) { unlock_heap(); return 0; }
    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b)) {
        if (out && count < capacity) {
            out[count].offset = (size_t)((char *)b - arena);
            out[count].size = blk_size(b);
            out[count].requested = blk_alloc(b) ? b->requested : 0;
            out[count].allocated = blk_alloc(b);
        }
        count++;
    }
    unlock_heap();
    return count;
}

double external_fragmentation(const alloc_stats_t *s)
{
    if (s->free_bytes == 0) return 0.0;
    return 1.0 - (double)s->largest_free / (double)s->free_bytes;
}

double internal_fragmentation(const alloc_stats_t *s)
{
    if (s->used_block_bytes == 0) return 0.0;
    return (double)(s->used_block_bytes - s->requested_bytes) / (double)s->used_block_bytes;
}

/* ---- debugging & verification ---- */
#define FAIL(...) do { fprintf(stderr, "heap_check: " __VA_ARGS__); fputc('\n', stderr); unlock_heap(); return -1; } while (0)

int heap_check(int verbose)
{
    lock_heap();
    if (!arena) FAIL("allocator not initialized");
    int prev_free = 0, rover_ok = (rover == NULL);
    size_t count = 0, heap_free_count = 0, list_free_count = 0;

    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b)) {
        size_t sz = blk_size(b);
        if ((uintptr_t)b & (ALIGNMENT - 1))       FAIL("block %p is not aligned", (void *)b);
        if (sz < MIN_BLOCK)                        FAIL("block %p is too small (%zu)", (void *)b, sz);
        if (sz & (ALIGNMENT - 1))                  FAIL("block %p size is not a multiple of 16", (void *)b);
        if ((char *)b + sz > heap_end)             FAIL("block %p extends past heap_end", (void *)b);
        if (*footer_of(b) != b->size_alloc)        FAIL("block %p header/footer mismatch", (void *)b);
        if (!blk_alloc(b) && prev_free)            FAIL("two adjacent free blocks at %p (missed coalesce)", (void *)b);
        if (blk_alloc(b) && b->requested > sz - OVERHEAD)
                                                   FAIL("block %p requested > capacity", (void *)b);
        if (!blk_alloc(b)) heap_free_count++;
        if (b == rover) rover_ok = 1;
        prev_free = !blk_alloc(b);
        count++;
        if (verbose) printf("  block %zu: %p size=%zu %s\n", count, (void *)b, sz,
                            blk_alloc(b) ? "ALLOC" : "FREE");
    }
    if (!rover_ok) FAIL("rover does not point to a valid block boundary");

    /* Explicit free list invariant verification */
    hdr_t *prev = NULL;
    for (hdr_t *f = free_head; f != NULL; f = *free_next_slot(f)) {
        if ((char *)f < (char *)heap_start || (char *)f >= heap_end)
            FAIL("free_list node %p out of heap bounds", (void *)f);
        if (blk_alloc(f))
            FAIL("allocated block %p found in free_list", (void *)f);
        if (*free_prev_slot(f) != prev)
            FAIL("free_list prev pointer mismatch at %p", (void *)f);
        prev = f;
        list_free_count++;
        if (list_free_count > count)
            FAIL("cycle detected in free_list");
    }
    if (list_free_count != heap_free_count)
        FAIL("free_list count (%zu) != heap free block count (%zu)",
             list_free_count, heap_free_count);

    unlock_heap();
    return 0;
}

void heap_dump(void) { heap_check(1); }

size_t allocator_report_leaks(void)
{
    lock_heap();
    if (!arena) { unlock_heap(); return 0; }
    size_t leaked_bytes = 0, leaked_blocks = 0;
    for (hdr_t *b = heap_start; (char *)b < heap_end; b = next_blk(b)) {
        if (blk_alloc(b)) {
            if (leaked_blocks == 0)
                fprintf(stderr, "=== MEMORY LEAK REPORT ===\n");
            leaked_blocks++;
            leaked_bytes += b->requested;
            fprintf(stderr, "  [LEAK #%zu] ptr=%p offset=%zu block=%zuB requested=%zuB\n",
                    leaked_blocks, payload_of(b), (size_t)((char *)b - arena),
                    blk_size(b), b->requested);
        }
    }
    if (leaked_blocks > 0) {
        fprintf(stderr, "Total: %zu unfreed bytes in %zu blocks.\n",
                leaked_bytes, leaked_blocks);
    }
    unlock_heap();
    return leaked_bytes;
}

void heap_map(int width)
{
    lock_heap();
    if (!arena || width <= 0) { unlock_heap(); return; }
    hdr_t *b = heap_start;
    size_t b_start = 0;
    putchar('[');
    for (int i = 0; i < width; i++) {
        size_t off = (size_t)i * ARENA_SIZE / width;
        while (b_start + blk_size(b) <= off) {
            b_start += blk_size(b);
            b = next_blk(b);
        }
        putchar(blk_alloc(b) ? '#' : '.');
    }
    printf("]  (1 char = %d bytes)\n", ARENA_SIZE / width);
    unlock_heap();
}

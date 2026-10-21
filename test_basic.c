#include "allocator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void expect_heap_ok(const char *where)
{
    if (heap_check(0) != 0) { printf("  [FAIL] heap_check: %s\n", where); failures++; }
}

static void test_basic(void)
{
    void *a = my_malloc(100), *b = my_malloc(200), *c = my_malloc(300);
    CHECK(a && b && c, "malloc returned NULL");
    CHECK(((uintptr_t)a % 16) == 0 && ((uintptr_t)b % 16) == 0, "misaligned pointer");
    CHECK(my_usable_size(a) >= 100, "invalid usable_size");
    memset(a, 0xAA, 100); memset(b, 0xBB, 200); memset(c, 0xCC, 300);
    expect_heap_ok("after malloc");

    /* Leak detector should report 3 blocks (600 bytes) */
    CHECK(allocator_report_leaks() == 600, "leak detector should report 600 bytes");

    my_free(b);                       /* middle block freed */
    expect_heap_ok("after free(b)");
    my_free(a);                       /* a + b should coalesce */
    expect_heap_ok("after free(a)");
    my_free(c);                       /* entire arena should coalesce into 1 block */
    expect_heap_ok("after free(c)");

    CHECK(allocator_report_leaks() == 0, "leak count should be 0 after freeing all blocks");

    alloc_stats_t s; allocator_get_stats(&s);
    CHECK(s.n_free_blocks == 1 && s.n_alloc_blocks == 0, "heap must have a single free block");
    CHECK(s.largest_free == s.heap_size, "entire heap must be free and coalesced");
}

static void test_edge_cases(void)
{
    CHECK(my_malloc(0) == NULL, "malloc(0) must return NULL");
    CHECK(my_malloc((size_t)-1) == NULL, "huge allocation must return NULL");
    CHECK(my_calloc((size_t)-1, 16) != NULL ? 0 : 1, "calloc overflow must be caught");

    unsigned char *z = my_calloc(64, 4);
    int zero = 1;
    for (int i = 0; i < 256; i++) if (z[i]) zero = 0;
    CHECK(zero, "calloc did not zero-initialize memory");

    alloc_stats_t s0; allocator_get_stats(&s0);
    my_free(z);
    my_free(z);                       /* double free: must be caught without corrupting heap */
    my_free((void *)0x1234);          /* invalid pointer */
    CHECK(my_realloc(z, 128) == NULL, "realloc on freed block must return NULL");
    alloc_stats_t s1; allocator_get_stats(&s1);
    CHECK(s1.errors == 3, "double free / invalid free / invalid realloc not caught");
    expect_heap_ok("after invalid free tests");
}

static void test_realloc(void)
{
    unsigned char *p = my_malloc(100);
    for (int i = 0; i < 100; i++) p[i] = (unsigned char)i;

    p = my_realloc(p, 5000);          /* expand */
    CHECK(p != NULL, "realloc expand failed");
    int ok = 1;
    for (int i = 0; i < 100; i++) if (p[i] != (unsigned char)i) ok = 0;
    CHECK(ok, "realloc did not preserve payload (expand)");
    expect_heap_ok("after realloc expand");

    p = my_realloc(p, 10);            /* shrink: block must split */
    ok = 1;
    for (int i = 0; i < 10; i++) if (p[i] != (unsigned char)i) ok = 0;
    CHECK(ok, "realloc did not preserve payload (shrink)");
    CHECK(my_usable_size(p) < 100, "realloc shrink did not split block");
    expect_heap_ok("after realloc shrink");

    CHECK(my_realloc(p, 0) == NULL, "realloc(p, 0) must return NULL");
    expect_heap_ok("after realloc(p, 0)");
}

static void test_aligned_alloc(void)
{
    void *p64   = my_aligned_alloc(64, 120);
    void *p256  = my_aligned_alloc(256, 500);
    void *p4096 = my_aligned_alloc(4096, 1024);
    CHECK(p64 && p256 && p4096, "aligned_alloc returned NULL");
    CHECK(((uintptr_t)p64 % 64) == 0, "64-byte alignment failed");
    CHECK(((uintptr_t)p256 % 256) == 0, "256-byte alignment failed");
    CHECK(((uintptr_t)p4096 % 4096) == 0, "4096-byte alignment failed");
    CHECK(my_aligned_alloc(48, 100) == NULL, "non-power-of-two alignment must be rejected");
    expect_heap_ok("after aligned_alloc");

    my_free(p64);
    my_free(p256);
    my_free(p4096);
    expect_heap_ok("after aligned_alloc free");
}

/* Random malloc/free stress test verifying payload integrity and heap invariants */
#define SLOTS 256
static void test_random(unsigned seed)
{
    struct { unsigned char *p; size_t n; unsigned char tag; } slot[SLOTS];
    memset(slot, 0, sizeof slot);
    srand(seed);

    for (int it = 0; it < 20000; it++) {
        int i = rand() % SLOTS;
        if (slot[i].p) {
            int ok = 1;
            for (size_t k = 0; k < slot[i].n; k++) if (slot[i].p[k] != slot[i].tag) { ok = 0; break; }
            CHECK(ok, "block payload corrupted (overwritten by another block)");
            my_free(slot[i].p);
            slot[i].p = NULL;
        } else {
            size_t n = (rand() % 10 == 0) ? 1 + rand() % 20000 : 1 + rand() % 1500;
            unsigned char *p = my_malloc(n);
            if (p) {
                slot[i].p = p; slot[i].n = n; slot[i].tag = (unsigned char)(it & 0xFF);
                memset(p, slot[i].tag, n);
            }
        }
        if (heap_check(0) != 0) { printf("  [FAIL] heap corrupted during random test (it=%d)\n", it); failures++; return; }
    }
    for (int i = 0; i < SLOTS; i++) my_free(slot[i].p);

    alloc_stats_t s; allocator_get_stats(&s);
    CHECK(s.n_free_blocks == 1 && s.largest_free == s.heap_size, "all blocks freed must coalesce into 1 block");
}

/* Multi-threaded (pthread) stress test */
#define THREAD_COUNT 4
#define THREAD_ITERS 2500

static void *thread_worker(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    void *ptrs[32] = {0};
    size_t sizes[32] = {0};

    for (int it = 0; it < THREAD_ITERS; it++) {
        seed = seed * 1103515245u + 12345u;
        int idx = (seed >> 16) % 32;
        if (ptrs[idx]) {
            if ((seed & 3u) == 0) {
                size_t new_sz = 16 + ((seed >> 8) % 512);
                void *np = my_realloc(ptrs[idx], new_sz);
                if (np) {
                    memset(np, (int)(seed & 0xFF), new_sz);
                    ptrs[idx] = np;
                    sizes[idx] = new_sz;
                }
            } else {
                my_free(ptrs[idx]);
                ptrs[idx] = NULL;
                sizes[idx] = 0;
            }
        } else {
            size_t sz = 16 + ((seed >> 8) % 512);
            void *p = my_malloc(sz);
            if (p) {
                memset(p, (int)(seed & 0xFF), sz);
                ptrs[idx] = p;
                sizes[idx] = sz;
            }
        }
    }
    for (int i = 0; i < 32; i++) my_free(ptrs[i]);
    return NULL;
}

static void test_multithreaded(void)
{
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, thread_worker, (void *)(uintptr_t)(100 + i));
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    expect_heap_ok("after multithreaded stress test");
    alloc_stats_t s; allocator_get_stats(&s);
    CHECK(s.n_free_blocks == 1 && s.n_alloc_blocks == 0,
          "all blocks must be free and coalesced after multithreaded test");
}

int main(void)
{
    for (int st = 0; st < NUM_STRATEGIES; st++) {
        printf("== %s ==\n", strategy_name((strategy_t)st));
        allocator_init((strategy_t)st); test_basic();
        allocator_init((strategy_t)st); test_edge_cases();
        allocator_init((strategy_t)st); test_realloc();
        allocator_init((strategy_t)st); test_aligned_alloc();
        allocator_init((strategy_t)st); test_random(42);
        allocator_init((strategy_t)st); test_multithreaded();
    }

    /* Fragmented heap visualization sample */
    allocator_init(FIRST_FIT);
    void *p[8];
    for (int i = 0; i < 8; i++) p[i] = my_malloc(20000 + i * 5000);
    for (int i = 0; i < 8; i += 2) my_free(p[i]);
    printf("\nFragmented heap sample:\n");
    heap_map(64);
    alloc_stats_t s; allocator_get_stats(&s);
    printf("external fragmentation: %.3f, internal fragmentation: %.3f\n",
           external_fragmentation(&s), internal_fragmentation(&s));
    allocator_destroy();

    printf("\n%s (%d errors)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

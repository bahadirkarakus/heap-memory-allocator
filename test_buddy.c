#include "buddy_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 128

int main(void)
{
    if (buddy_init() != 0) return 1;

    char *a = buddy_malloc(100);
    char *b = buddy_malloc(5000);
    if (!a || !b || buddy_check(0) != 0) return 1;
    if (((uintptr_t)a % 16) != 0 || ((uintptr_t)b % 16) != 0) {
        fprintf(stderr, "buddy alignment failed\n");
        return 1;
    }

    memset(a, 'A', 100);
    a = buddy_realloc(a, 4000);
    if (!a || a[0] != 'A' || buddy_check(0) != 0) return 1;

    /* Shrink test: order 12 (4096) -> order 7 (128) */
    a = buddy_realloc(a, 50);
    if (!a || a[0] != 'A' || buddy_check(0) != 0) return 1;

    buddy_free(b);
    buddy_free(a);

    /* Double-free & invalid free detection */
    buddy_free(a);
    buddy_free((void *)0x1234);

    buddy_stats_t stats;
    buddy_get_stats(&stats);
    if (stats.n_free_blocks != 1 || stats.largest_free != stats.heap_size || stats.errors != 2)
        return 1;
    if (buddy_calloc((size_t)-1, 16) != NULL) return 1;

    /* Random stress test */
    struct { unsigned char *p; size_t n; unsigned char tag; } slot[SLOTS];
    memset(slot, 0, sizeof slot);
    srand(42);
    for (int it = 0; it < 10000; it++) {
        int i = rand() % SLOTS;
        if (slot[i].p) {
            for (size_t k = 0; k < slot[i].n; k++) {
                if (slot[i].p[k] != slot[i].tag) return 1;
            }
            buddy_free(slot[i].p);
            slot[i].p = NULL;
        } else {
            size_t n = 1 + (size_t)(rand() % 4000);
            unsigned char *p = buddy_malloc(n);
            if (p) {
                if (((uintptr_t)p % 16) != 0) return 1;
                slot[i].p = p;
                slot[i].n = n;
                slot[i].tag = (unsigned char)(it & 0xFF);
                memset(p, slot[i].tag, n);
            }
        }
        if (buddy_check(0) != 0) return 1;
    }
    for (int i = 0; i < SLOTS; i++) buddy_free(slot[i].p);
    buddy_get_stats(&stats);
    if (stats.n_free_blocks != 1 || stats.largest_free != stats.heap_size) return 1;

    buddy_destroy();
    puts("BUDDY TESTS PASSED");
    return 0;
}

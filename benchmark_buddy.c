#define _POSIX_C_SOURCE 200809L

#include "buddy_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SLOT_COUNT 512
#define OPERATION_COUNT 300000

typedef struct {
    void *ptr;
    size_t size;
} slot_t;

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint64_t elapsed_ns(struct timespec start, struct timespec end)
{
    return (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL +
           (uint64_t)(end.tv_nsec - start.tv_nsec);
}

int main(void)
{
    slot_t slots[SLOT_COUNT] = {0};
    uint32_t random_state = 0xC0FFEEu;
    size_t attempts = 0;
    size_t successes = 0;
    struct timespec start, end;

    if (buddy_init() != 0) return 1;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t operation = 0; operation < OPERATION_COUNT; operation++) {
        slot_t *slot = &slots[next_random(&random_state) % SLOT_COUNT];
        if (slot->ptr) {
            if ((next_random(&random_state) & 3u) == 0) {
                size_t size = 1 + next_random(&random_state) % 8192;
                void *new_ptr = buddy_realloc(slot->ptr, size);
                if (new_ptr) {
                    slot->ptr = new_ptr;
                    slot->size = size;
                }
            } else {
                buddy_free(slot->ptr);
                slot->ptr = NULL;
                slot->size = 0;
            }
        } else {
            size_t size = 1 + next_random(&random_state) % 8192;
            attempts++;
            slot->ptr = buddy_malloc(size);
            if (slot->ptr) {
                slot->size = size;
                successes++;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    buddy_stats_t stats;
    buddy_get_stats(&stats);
    printf("buddy_system operations=%d alloc_success=%.2f%% ns/op=%.2f "
           "external=%.4f internal=%.4f\n",
           OPERATION_COUNT, attempts ? 100.0 * successes / attempts : 0.0,
           (double)elapsed_ns(start, end) / OPERATION_COUNT,
           buddy_external_fragmentation(&stats),
           buddy_internal_fragmentation(&stats));

    for (size_t i = 0; i < SLOT_COUNT; i++) buddy_free(slots[i].ptr);
    int result = buddy_check(0);
    buddy_destroy();
    return result == 0 ? 0 : 1;
}

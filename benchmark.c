#define _POSIX_C_SOURCE 200809L

#include "allocator.h"
#include "buddy_allocator.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOT_COUNT 512
#define OPERATION_COUNT 300000
#define SAMPLE_INTERVAL 1000

typedef struct {
    void *ptr;
    size_t size;
} slot_t;

static uint32_t next_random(uint32_t *state)
{
    /* Deterministic input makes strategy comparisons reproducible. */
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint64_t elapsed_ns(const struct timespec *start,
                           const struct timespec *end)
{
    return (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000ULL +
           (uint64_t)(end->tv_nsec - start->tv_nsec);
}

static void sample_stats(alloc_stats_t *peak, size_t *samples)
{
    alloc_stats_t current;
    allocator_get_stats(&current);
    if (current.free_bytes != 0) {
        double external = external_fragmentation(&current);
        double peak_external = external_fragmentation(peak);
        if (external > peak_external) {
            peak->free_bytes = current.free_bytes;
            peak->largest_free = current.largest_free;
        }
    }
    if (current.used_block_bytes != 0) {
        double internal = internal_fragmentation(&current);
        double peak_internal = internal_fragmentation(peak);
        if (internal > peak_internal) {
            peak->used_block_bytes = current.used_block_bytes;
            peak->requested_bytes = current.requested_bytes;
        }
    }
    (*samples)++;
}

static int run_benchmark(strategy_t strategy, int csv, uint32_t seed)
{
    slot_t slots[SLOT_COUNT] = {0};
    alloc_stats_t peak = {0};
    uint32_t random_state = seed;
    size_t successful_allocations = 0;
    size_t allocation_attempts = 0;
    size_t samples = 0;
    uint64_t elapsed = 0;

    if (allocator_init(strategy) != 0) {
        fprintf(stderr, "allocator_init(%s) failed\n", strategy_name(strategy));
        return -1;
    }

    for (size_t base = 0; base < OPERATION_COUNT; base += SAMPLE_INTERVAL) {
        size_t limit = base + SAMPLE_INTERVAL;
        if (limit > OPERATION_COUNT) limit = OPERATION_COUNT;

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (size_t operation = base; operation < limit; operation++) {
            size_t index = next_random(&random_state) % SLOT_COUNT;
            slot_t *slot = &slots[index];

            if (slot->ptr != NULL) {
                if ((next_random(&random_state) & 3u) == 0) {
                    size_t new_size = 1 + next_random(&random_state) % 8192;
                    void *new_ptr = my_realloc(slot->ptr, new_size);
                    if (new_ptr != NULL) {
                        slot->ptr = new_ptr;
                        slot->size = new_size;
                    }
                } else {
                    my_free(slot->ptr);
                    slot->ptr = NULL;
                    slot->size = 0;
                }
            } else {
                size_t requested = 1 + next_random(&random_state) % 8192;
                allocation_attempts++;
                slot->ptr = my_malloc(requested);
                if (slot->ptr != NULL) {
                    slot->size = requested;
                    successful_allocations++;
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        elapsed += elapsed_ns(&start, &end);

        /* Stats sampling is excluded from timing to measure pure allocator speed */
        sample_stats(&peak, &samples);
    }

    for (size_t i = 0; i < SLOT_COUNT; i++) {
        my_free(slots[i].ptr);
    }

    double ns_per_operation = (double)elapsed / OPERATION_COUNT;
    double success_rate = allocation_attempts == 0
        ? 0.0
        : (double)successful_allocations / allocation_attempts;

    if (csv) {
        printf("%s,%zu,%zu,%.2f,%.6f,%.6f,%" PRIu64 "\n",
               strategy_name(strategy), successful_allocations,
               allocation_attempts, success_rate,
               external_fragmentation(&peak), internal_fragmentation(&peak),
               elapsed);
    } else {
        printf("%-13s operations=%d  alloc_success=%.2f%%  "
               "ns/op=%6.2f  peak_external=%.4f  peak_internal=%.4f\n",
               strategy_name(strategy), OPERATION_COUNT, success_rate * 100.0,
               ns_per_operation, external_fragmentation(&peak),
               internal_fragmentation(&peak));
    }

    allocator_destroy();
    (void)samples;
    return 0;
}

static int run_buddy_benchmark(int csv, uint32_t seed)
{
    slot_t slots[SLOT_COUNT] = {0};
    buddy_stats_t peak = {0};
    uint32_t random_state = seed;
    size_t successful_allocations = 0;
    size_t allocation_attempts = 0;
    uint64_t elapsed = 0;

    if (buddy_init() != 0) {
        fprintf(stderr, "buddy_init() failed\n");
        return -1;
    }

    for (size_t base = 0; base < OPERATION_COUNT; base += SAMPLE_INTERVAL) {
        size_t limit = base + SAMPLE_INTERVAL;
        if (limit > OPERATION_COUNT) limit = OPERATION_COUNT;

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (size_t operation = base; operation < limit; operation++) {
            size_t index = next_random(&random_state) % SLOT_COUNT;
            slot_t *slot = &slots[index];

            if (slot->ptr != NULL) {
                if ((next_random(&random_state) & 3u) == 0) {
                    size_t new_size = 1 + next_random(&random_state) % 8192;
                    void *new_ptr = buddy_realloc(slot->ptr, new_size);
                    if (new_ptr != NULL) {
                        slot->ptr = new_ptr;
                        slot->size = new_size;
                    }
                } else {
                    buddy_free(slot->ptr);
                    slot->ptr = NULL;
                    slot->size = 0;
                }
            } else {
                size_t requested = 1 + next_random(&random_state) % 8192;
                allocation_attempts++;
                slot->ptr = buddy_malloc(requested);
                if (slot->ptr != NULL) {
                    slot->size = requested;
                    successful_allocations++;
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        elapsed += elapsed_ns(&start, &end);

        buddy_stats_t current;
        buddy_get_stats(&current);
        if (current.free_bytes != 0 &&
            buddy_external_fragmentation(&current) > buddy_external_fragmentation(&peak)) {
            peak.free_bytes = current.free_bytes;
            peak.largest_free = current.largest_free;
        }
        if (current.used_block_bytes != 0 &&
            buddy_internal_fragmentation(&current) > buddy_internal_fragmentation(&peak)) {
            peak.used_block_bytes = current.used_block_bytes;
            peak.requested_bytes = current.requested_bytes;
        }
    }

    for (size_t i = 0; i < SLOT_COUNT; i++) {
        buddy_free(slots[i].ptr);
    }

    double ns_per_operation = (double)elapsed / OPERATION_COUNT;
    double success_rate = allocation_attempts == 0
        ? 0.0
        : (double)successful_allocations / allocation_attempts;

    if (csv) {
        printf("buddy_system,%zu,%zu,%.2f,%.6f,%.6f,%" PRIu64 "\n",
               successful_allocations, allocation_attempts, success_rate,
               buddy_external_fragmentation(&peak),
               buddy_internal_fragmentation(&peak), elapsed);
    } else {
        printf("%-13s operations=%d  alloc_success=%.2f%%  "
               "ns/op=%6.2f  peak_external=%.4f  peak_internal=%.4f\n",
               "buddy_system", OPERATION_COUNT, success_rate * 100.0,
               ns_per_operation, buddy_external_fragmentation(&peak),
               buddy_internal_fragmentation(&peak));
    }

    buddy_destroy();
    return 0;
}

int main(int argc, char **argv)
{
    int csv = 0;
    uint32_t seed = 0xC0FFEEu;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0) {
            csv = 1;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[++i], &end, 0);
            if (*argv[i] == '\0' || *end != '\0' || parsed > UINT32_MAX) {
                fprintf(stderr, "invalid seed: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            seed = (uint32_t)parsed;
        } else {
            fprintf(stderr, "usage: %s [--csv] [--seed N]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (csv) {
        puts("strategy,successful_allocations,allocation_attempts,success_rate,"
             "peak_external_fragmentation,peak_internal_fragmentation,"
             "elapsed_ns");
    }
    for (int i = 0; i < NUM_STRATEGIES; i++) {
        if (run_benchmark((strategy_t)i, csv, seed) != 0) return EXIT_FAILURE;
    }
    if (run_buddy_benchmark(csv, seed) != 0) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

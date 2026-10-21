#define _POSIX_C_SOURCE 200809L

#include "allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAP_WIDTH 80
#define SLOT_COUNT 8

typedef enum {
    OP_ALLOC,
    OP_FREE,
    OP_REALLOC
} operation_kind_t;

typedef struct {
    operation_kind_t kind;
    int slot;
    size_t size;
} operation_t;

static const char *operation_name(operation_kind_t kind)
{
    switch (kind) {
    case OP_ALLOC: return "ALLOC";
    case OP_FREE: return "FREE";
    case OP_REALLOC: return "REALLOC";
    default: return "UNKNOWN";
    }
}

static void pause_for(unsigned milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };
    nanosleep(&delay, NULL);
}

static void clear_screen(int clear)
{
    if (clear) printf("\033[2J\033[H");
}

static void print_frame(strategy_t strat, size_t step, const operation_t *operation,
                        void *slots[SLOT_COUNT], int clear)
{
    alloc_stats_t stats;
    allocator_get_stats(&stats);
    clear_screen(clear);
    printf("Custom Heap Allocator Simulator | step %zu\n", step);
    printf("strategy: %-12s operation: %s slot=%d size=%zu\n\n",
           strategy_name(strat), operation_name(operation->kind),
           operation->slot, operation->size);
    printf("Legend: # allocated, . free\n");
    heap_map(MAP_WIDTH);
    printf("free=%zu  largest_free=%zu  external=%.3f  internal=%.3f\n",
           stats.free_bytes, stats.largest_free,
           external_fragmentation(&stats), internal_fragmentation(&stats));
    printf("allocated blocks=%zu  free blocks=%zu\n\n",
           stats.n_alloc_blocks, stats.n_free_blocks);
    printf("Slots: ");
    for (int i = 0; i < SLOT_COUNT; i++)
        printf("[%d:%s] ", i, slots[i] ? "USED" : "----");
    putchar('\n');
    fflush(stdout);
}

static int perform(const operation_t *operation, void *slots[SLOT_COUNT])
{
    void **slot = &slots[operation->slot];
    switch (operation->kind) {
    case OP_ALLOC:
        if (*slot) return -1;
        *slot = my_malloc(operation->size);
        return *slot ? 0 : -1;
    case OP_FREE:
        if (!*slot) return -1;
        my_free(*slot);
        *slot = NULL;
        return 0;
    case OP_REALLOC: {
        if (!*slot) return -1;
        void *new_ptr = my_realloc(*slot, operation->size);
        if (!new_ptr) return -1;
        *slot = new_ptr;
        return 0;
    }
    default:
        return -1;
    }
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--no-clear] [--strategy NAME] [delay_ms]\n", program);
}

int main(int argc, char **argv)
{
    unsigned delay_ms = 500;
    int clear = 1;
    strategy_t strat = FIRST_FIT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-clear") == 0) {
            clear = 0;
        } else if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            int found = 0;
            for (int st = 0; st < NUM_STRATEGIES; st++) {
                if (strcmp(s, strategy_name((strategy_t)st)) == 0) {
                    strat = (strategy_t)st;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else {
            char *end = NULL;
            unsigned long value = strtoul(argv[i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || value > 60000) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            delay_ms = (unsigned)value;
        }
    }

    static const operation_t operations[] = {
        {OP_ALLOC, 0, 90000},  {OP_ALLOC, 1, 140000},
        {OP_ALLOC, 2, 70000},  {OP_ALLOC, 3, 110000},
        {OP_FREE, 1, 0},       {OP_FREE, 3, 0},
        {OP_ALLOC, 4, 120000}, {OP_REALLOC, 0, 180000},
        {OP_FREE, 2, 0},       {OP_ALLOC, 5, 50000},
        {OP_ALLOC, 6, 200000}, {OP_FREE, 4, 0},
        {OP_REALLOC, 5, 160000}, {OP_FREE, 0, 0},
        {OP_FREE, 5, 0},       {OP_FREE, 6, 0}
    };
    void *slots[SLOT_COUNT] = {0};

    if (allocator_init(strat) != 0) {
        fprintf(stderr, "allocator initialization failed\n");
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        if (perform(&operations[i], slots) != 0 ||
            heap_check(0) != 0) {
            fprintf(stderr, "\nsimulation failed at step %zu\n", i + 1);
            allocator_destroy();
            return EXIT_FAILURE;
        }
        print_frame(strat, i + 1, &operations[i], slots, clear);
        if (i + 1 < sizeof(operations) / sizeof(operations[0]))
            pause_for(delay_ms);
    }
    allocator_destroy();
    return EXIT_SUCCESS;
}

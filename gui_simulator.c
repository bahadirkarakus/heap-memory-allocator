#include "allocator.h"
#include "buddy_allocator.h"

#include <raylib.h>
#include <stdio.h>
#include <string.h>

#define WINDOW_WIDTH 1100
#define WINDOW_HEIGHT 700
#define MAX_BLOCKS 4096
#define SLOT_COUNT 8

typedef enum { GUI_ALLOC, GUI_FREE, GUI_REALLOC } gui_operation_kind_t;

typedef struct {
    gui_operation_kind_t kind;
    int slot;
    size_t size;
} gui_operation_t;

static const gui_operation_t operations[] = {
    {GUI_ALLOC, 0, 90000}, {GUI_ALLOC, 1, 140000},
    {GUI_ALLOC, 2, 70000}, {GUI_ALLOC, 3, 110000},
    {GUI_FREE, 1, 0},      {GUI_FREE, 3, 0},
    {GUI_ALLOC, 4, 120000}, {GUI_REALLOC, 0, 180000},
    {GUI_FREE, 2, 0},      {GUI_ALLOC, 5, 50000},
    {GUI_ALLOC, 6, 200000}, {GUI_FREE, 4, 0},
    {GUI_REALLOC, 5, 160000}, {GUI_FREE, 0, 0},
    {GUI_FREE, 5, 0},      {GUI_FREE, 6, 0}
};

static const char *operation_name(gui_operation_kind_t kind)
{
    return kind == GUI_ALLOC ? "ALLOC" :
           kind == GUI_FREE ? "FREE" : "REALLOC";
}

static void draw_text_value(const char *label, size_t value, int x, int y)
{
    char text[80];
    snprintf(text, sizeof(text), "%s%zu", label, value);
    DrawText(text, x, y, 20, RAYWHITE);
}

static void restart_simulation(strategy_t strategy, int use_buddy,
                               void *slots[SLOT_COUNT], size_t *step, float *timer)
{
    if (use_buddy) {
        allocator_destroy();
        buddy_init();
    } else {
        buddy_destroy();
        allocator_init(strategy);
    }
    memset(slots, 0, sizeof(void *) * SLOT_COUNT);
    *step = 0;
    *timer = 0.0f;
}

static void render_scene(strategy_t current_strategy, int use_buddy, int paused,
                         size_t step, const alloc_stats_t *stats,
                         const alloc_block_info_t *blocks, size_t block_count,
                         double ext_frag, double int_frag)
{
    ClearBackground((Color){18, 22, 30, 255});
    DrawText("Custom Heap Allocator", 40, 30, 32, RAYWHITE);
    DrawText("SPACE: step  P: pause  R: restart  1-5: list strat  6: buddy  ESC: exit",
             40, 72, 19, LIGHTGRAY);
    char strategy_text[100];
    snprintf(strategy_text, sizeof(strategy_text), "Strategy: %s%s",
             use_buddy ? "buddy_system" : strategy_name(current_strategy),
             paused ? " (PAUSED)" : "");
    DrawText(strategy_text, 740, 35, 22, SKYBLUE);

    char status[160];
    if (step == 0) {
        snprintf(status, sizeof(status), "Waiting to start");
    } else if (step <= sizeof(operations) / sizeof(operations[0])) {
        const gui_operation_t *last = &operations[step - 1];
        snprintf(status, sizeof(status), "Step %zu/%zu: %s slot %d (%zu bytes)",
                 step, sizeof(operations) / sizeof(operations[0]),
                 operation_name(last->kind), last->slot, last->size);
    }
    DrawText(status, 40, 125, 22, GOLD);

    const int map_x = 40;
    const int map_y = 180;
    const int map_width = 1020;
    const int map_height = 150;
    DrawRectangle(map_x, map_y, map_width, map_height, DARKGRAY);
    for (size_t i = 0; i < block_count && i < MAX_BLOCKS; i++) {
        float start = (float)blocks[i].offset / stats->heap_size;
        float width = (float)blocks[i].size / stats->heap_size;
        int x = map_x + (int)(start * map_width);
        int w = (int)(width * map_width);
        if (w < 1) w = 1;
        Color color = blocks[i].allocated
            ? (Color){55, 170, 95, 255}
            : (Color){65, 75, 90, 255};
        DrawRectangle(x, map_y, w, map_height, color);
        if (blocks[i].allocated && blocks[i].size > blocks[i].requested) {
            float req_w = (float)blocks[i].requested / stats->heap_size;
            int rw = (int)(req_w * map_width);
            if (w > rw) {
                DrawRectangle(x + rw, map_y, w - rw, map_height,
                              (Color){210, 140, 45, 255});
            }
        }
        DrawRectangleLines(x, map_y, w, map_height, (Color){30, 35, 45, 255});
        if (w > 45) {
            char size_text[32];
            snprintf(size_text, sizeof(size_text), "%zu KB",
                     blocks[i].size / 1024);
            DrawText(size_text, x + 4, map_y + 62, 16, RAYWHITE);
            DrawText(blocks[i].allocated ? "USED" : "FREE",
                     x + 4, map_y + 88, 16, RAYWHITE);
        }
    }
    DrawRectangleLines(map_x, map_y, map_width, map_height, RAYWHITE);
    DrawText("USED (Payload)", 40, 350, 20, (Color){90, 220, 120, 255});
    DrawText("INTERNAL FRAG", 230, 350, 20, (Color){210, 140, 45, 255});
    DrawText("FREE", 420, 350, 20, LIGHTGRAY);

    draw_text_value("Free bytes: ", stats->free_bytes, 40, 410);
    draw_text_value("Largest free: ", stats->largest_free, 40, 445);
    draw_text_value("Allocated blocks: ", stats->n_alloc_blocks, 40, 480);
    draw_text_value("Free blocks: ", stats->n_free_blocks, 40, 515);
    char metrics[160];
    snprintf(metrics, sizeof(metrics), "External fragmentation: %.3f", ext_frag);
    DrawText(metrics, 520, 410, 20, RAYWHITE);
    snprintf(metrics, sizeof(metrics), "Internal fragmentation: %.3f", int_frag);
    DrawText(metrics, 520, 445, 20, RAYWHITE);
    DrawText("This map is built from the allocator's real blocks.",
             520, 515, 18, LIGHTGRAY);
}

static void export_mode_screenshot(strategy_t strat, int use_buddy,
                                   size_t target_steps, const char *filename)
{
    void *slots[SLOT_COUNT] = {0};
    alloc_block_info_t blocks[MAX_BLOCKS];
    size_t step = 0;
    float timer = 0.0f;
    restart_simulation(strat, use_buddy, slots, &step, &timer);

    for (size_t s = 0; s < target_steps && s < sizeof(operations) / sizeof(operations[0]); s++) {
        const gui_operation_t *op = &operations[s];
        if (use_buddy) {
            if (op->kind == GUI_ALLOC) slots[op->slot] = buddy_malloc(op->size);
            else if (op->kind == GUI_FREE) { buddy_free(slots[op->slot]); slots[op->slot] = NULL; }
            else slots[op->slot] = buddy_realloc(slots[op->slot], op->size);
        } else {
            if (op->kind == GUI_ALLOC) slots[op->slot] = my_malloc(op->size);
            else if (op->kind == GUI_FREE) { my_free(slots[op->slot]); slots[op->slot] = NULL; }
            else slots[op->slot] = my_realloc(slots[op->slot], op->size);
        }
        step++;
    }

    alloc_stats_t stats = {0};
    size_t block_count = 0;
    double ext_frag = 0.0, int_frag = 0.0;
    if (use_buddy) {
        buddy_stats_t bstats;
        buddy_get_stats(&bstats);
        stats.heap_size = bstats.heap_size;
        stats.free_bytes = bstats.free_bytes;
        stats.largest_free = bstats.largest_free;
        stats.used_block_bytes = bstats.used_block_bytes;
        stats.requested_bytes = bstats.requested_bytes;
        stats.n_alloc_blocks = bstats.n_alloc_blocks;
        stats.n_free_blocks = bstats.n_free_blocks;
        ext_frag = buddy_external_fragmentation(&bstats);
        int_frag = buddy_internal_fragmentation(&bstats);
        block_count = buddy_get_blocks((buddy_block_info_t *)blocks, MAX_BLOCKS);
    } else {
        allocator_get_stats(&stats);
        ext_frag = external_fragmentation(&stats);
        int_frag = internal_fragmentation(&stats);
        block_count = allocator_get_blocks(blocks, MAX_BLOCKS);
    }

    RenderTexture2D target = LoadRenderTexture(WINDOW_WIDTH, WINDOW_HEIGHT);
    BeginTextureMode(target);
    render_scene(strat, use_buddy, 0, step, &stats, blocks, block_count, ext_frag, int_frag);
    EndTextureMode();

    Image img = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&img);
    ExportImage(img, filename);
    UnloadImage(img);
    UnloadRenderTexture(target);

    for (int i = 0; i < SLOT_COUNT; i++) {
        if (use_buddy) buddy_free(slots[i]);
        else my_free(slots[i]);
    }
}

int main(int argc, char **argv)
{
    void *slots[SLOT_COUNT] = {0};
    alloc_block_info_t blocks[MAX_BLOCKS];
    size_t step = 0;
    float timer = 0.0f;
    const float step_seconds = 0.8f;
    strategy_t current_strategy = FIRST_FIT;
    int use_buddy = 0;
    int paused = 0;

    if (allocator_init(current_strategy) != 0) return 1;
    if (argc > 1 && strcmp(argv[1], "--export-screenshots") == 0) {
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Custom Heap Allocator Simulator");
        export_mode_screenshot(FIRST_FIT, 0, 11, "fig_gui_first_fit.png");
        export_mode_screenshot(EXPLICIT_FIT, 0, 11, "fig_gui_explicit_fit.png");
        export_mode_screenshot(FIRST_FIT, 1, 11, "fig_gui_buddy.png");
        buddy_destroy();
        allocator_destroy();
        CloseWindow();
        return 0;
    }

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Custom Heap Allocator Simulator");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        float frame_time = GetFrameTime();
        if (!paused) timer += frame_time;

        if (IsKeyPressed(KEY_P)) paused = !paused;
        if (IsKeyPressed(KEY_ONE)) {
            use_buddy = 0; current_strategy = FIRST_FIT;
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        } else if (IsKeyPressed(KEY_TWO)) {
            use_buddy = 0; current_strategy = NEXT_FIT;
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        } else if (IsKeyPressed(KEY_THREE)) {
            use_buddy = 0; current_strategy = BEST_FIT;
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        } else if (IsKeyPressed(KEY_FOUR)) {
            use_buddy = 0; current_strategy = WORST_FIT;
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        } else if (IsKeyPressed(KEY_FIVE)) {
            use_buddy = 0; current_strategy = EXPLICIT_FIT;
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        } else if (IsKeyPressed(KEY_SIX)) {
            use_buddy = 1;
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        }

        if (((!paused && timer >= step_seconds) || IsKeyPressed(KEY_SPACE)) &&
            step < sizeof(operations) / sizeof(operations[0])) {
            const gui_operation_t *operation = &operations[step];
            if (use_buddy) {
                if (operation->kind == GUI_ALLOC) {
                    slots[operation->slot] = buddy_malloc(operation->size);
                } else if (operation->kind == GUI_FREE) {
                    buddy_free(slots[operation->slot]);
                    slots[operation->slot] = NULL;
                } else {
                    slots[operation->slot] =
                        buddy_realloc(slots[operation->slot], operation->size);
                }
            } else {
                if (operation->kind == GUI_ALLOC) {
                    slots[operation->slot] = my_malloc(operation->size);
                } else if (operation->kind == GUI_FREE) {
                    my_free(slots[operation->slot]);
                    slots[operation->slot] = NULL;
                } else {
                    slots[operation->slot] =
                        my_realloc(slots[operation->slot], operation->size);
                }
            }
            step++;
            timer = 0.0f;
        }
        if (IsKeyPressed(KEY_R)) {
            restart_simulation(current_strategy, use_buddy, slots, &step, &timer);
        }

        alloc_stats_t stats = {0};
        size_t block_count = 0;
        double ext_frag = 0.0, int_frag = 0.0;

        if (use_buddy) {
            buddy_stats_t bstats;
            buddy_get_stats(&bstats);
            stats.heap_size = bstats.heap_size;
            stats.free_bytes = bstats.free_bytes;
            stats.largest_free = bstats.largest_free;
            stats.used_block_bytes = bstats.used_block_bytes;
            stats.requested_bytes = bstats.requested_bytes;
            stats.n_alloc_blocks = bstats.n_alloc_blocks;
            stats.n_free_blocks = bstats.n_free_blocks;
            ext_frag = buddy_external_fragmentation(&bstats);
            int_frag = buddy_internal_fragmentation(&bstats);
            block_count = buddy_get_blocks((buddy_block_info_t *)blocks, MAX_BLOCKS);
        } else {
            allocator_get_stats(&stats);
            ext_frag = external_fragmentation(&stats);
            int_frag = internal_fragmentation(&stats);
            block_count = allocator_get_blocks(blocks, MAX_BLOCKS);
        }

        BeginDrawing();
        render_scene(current_strategy, use_buddy, paused, step, &stats, blocks,
                     block_count, ext_frag, int_frag);
        EndDrawing();
    }

    for (int i = 0; i < SLOT_COUNT; i++) {
        if (use_buddy) buddy_free(slots[i]);
        else my_free(slots[i]);
    }
    buddy_destroy();
    allocator_destroy();
    CloseWindow();
    return 0;
}

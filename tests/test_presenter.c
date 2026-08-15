#include "mediaplayer/screen/presenter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define EXPECT(condition, message)                                             \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL: %s\n", message);                           \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

struct backend_state {
    bool fail;
    size_t calls;
    size_t last_viewers;
    uint32_t last_x;
    uint32_t last_y;
    uint64_t last_generation;
    uint8_t last_byte;
    uint64_t last_viewer_id;
};

static bool capture_send(void *context, const struct presenter_tile *tile)
{
    struct backend_state *state = context;
    state->calls++;
    state->last_viewers = tile->viewer_count;
    state->last_x = tile->tile_x;
    state->last_y = tile->tile_y;
    state->last_generation = tile->generation;
    state->last_byte = tile->pixels[0];
    state->last_viewer_id = tile->viewer_count == 0
                                ? 0
                                : tile->viewers[0].stable_id;
    return !state->fail;
}

static bool make_geometry(int width, int height, struct screen_geom *out)
{
    struct screen_pos c1 = {0, height - 1, 0};
    struct screen_pos c2 = {width - 1, 0, 0};
    return screen_geom_validate(c1, c2, "overworld", "overworld",
                                SCREEN_FACE_SOUTH, out) == SCREEN_GEOM_OK;
}

static bool commit_tile(struct surface *surface, uint32_t x, uint32_t y,
                        uint8_t value)
{
    uint8_t *pixels = calloc(1, SURFACE_TILE_BYTES);
    if (pixels == nullptr)
        return false;
    pixels[0] = value;
    struct surface_frame *frame = nullptr;
    bool ok = surface_frame_begin(surface, &frame) == SURFACE_OK &&
              surface_frame_update_tile(frame, x, y, pixels,
                                         SURFACE_TILE_BYTES,
                                         SURFACE_TILE_ROW_BYTES,
                                         SURFACE_FORMAT_RGBA8888) ==
                  SURFACE_OK &&
              surface_frame_commit(frame) == SURFACE_OK;
    if (!ok && frame != nullptr)
        surface_frame_abort(frame);
    free(pixels);
    return ok;
}

static struct presenter_viewer viewer_at(double x, double y, double z,
                                         const char *dimension,
                                         uint64_t id)
{
    struct presenter_viewer viewer = {
        .player = (void *)(uintptr_t)(id + 1),
        .context = (void *)(uintptr_t)(id + 1000),
        .stable_id = id,
        .x = x,
        .y = y,
        .z = z,
    };
    strncpy(viewer.dimension, dimension, sizeof(viewer.dimension) - 1);
    return viewer;
}

static bool create_presenter(struct surface *surface,
                             const struct screen_geom *geometry,
                             struct backend_state *state,
                             struct presenter *out)
{
    struct presenter_backend backend = {
        .context = state,
        .send = capture_send,
    };
    return presenter_init(out, surface, geometry, 16.0, backend) ==
           PRESENTER_OK;
}

static void test_dirty_budget_and_latest(void)
{
    struct surface *surface = nullptr;
    struct screen_geom geometry;
    struct backend_state state = {0};
    struct presenter presenter;
    struct presenter_viewer viewer = viewer_at(0.5, 0.5, 0.5,
                                               "overworld", 11);
    struct presenter_stats stats;
    EXPECT(surface_create(2, 1, 2, 2, false, &surface) == SURFACE_OK &&
               make_geometry(2, 1, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter),
           "create dirty presenter");
    EXPECT(presenter_set_viewers(&presenter, &viewer, 1) == PRESENTER_OK,
           "set dirty viewer");
    EXPECT(commit_tile(surface, 0, 0, 1) && commit_tile(surface, 1, 0, 2),
           "commit two dirty tiles");
    EXPECT(presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.examined == 1 && stats.sent == 1 &&
               stats.skipped == 0 && state.calls == 1,
           "dirty tick honors budget");
    struct surface_stats surface_stats;
    surface_get_stats(surface, &surface_stats);
    EXPECT(surface_stats.pending_tiles == 1,
           "budget leaves dirty carryover");
    EXPECT(presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.examined == 1 && stats.sent == 1 &&
               surface_get_stats(surface, &surface_stats) == SURFACE_OK &&
               surface_stats.pending_tiles == 0,
           "next tick drains carryover");

    EXPECT(commit_tile(surface, 0, 0, 3) && commit_tile(surface, 0, 0, 4),
           "commit latest state twice");
    EXPECT(presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.sent == 1 && state.last_byte == 4 &&
               state.last_generation == 4,
           "dirty callback sees latest resident state");
    EXPECT(commit_tile(surface, 0, 0, 4) &&
               presenter_tick(&presenter, 4, &stats) == PRESENTER_OK &&
               stats.examined == 0 && stats.sent == 0,
           "identical frame performs zero dirty work");
    surface_destroy(surface);
}

static void test_failure_and_no_viewer_resend(void)
{
    struct surface *surface = nullptr;
    struct screen_geom geometry;
    struct backend_state state = {0};
    struct presenter presenter;
    struct presenter_viewer viewer = viewer_at(0.5, 0.5, 0.5,
                                               "overworld", 21);
    struct presenter_stats stats;
    EXPECT(surface_create(1, 1, 1, 1, false, &surface) == SURFACE_OK &&
               make_geometry(1, 1, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter) &&
               presenter_set_viewers(&presenter, &viewer, 1) == PRESENTER_OK &&
               commit_tile(surface, 0, 0, 7),
           "create failure presenter");
    state.fail = true;
    EXPECT(presenter_tick(&presenter, 1, &stats) == PRESENTER_ERR_BACKEND &&
               stats.examined == 1 && stats.failures == 1 &&
               surface_get_stats(surface, &(struct surface_stats){0}) ==
                   SURFACE_OK,
           "backend failure is reported");
    struct surface_stats surface_stats;
    surface_get_stats(surface, &surface_stats);
    EXPECT(surface_stats.pending_tiles == 1,
           "backend failure retains dirty entry");
    state.fail = false;
    EXPECT(presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.sent == 1 && surface_get_stats(surface, &surface_stats) ==
                   SURFACE_OK && surface_stats.pending_tiles == 0,
           "retained dirty entry retries");
    surface_destroy(surface);

    surface = nullptr;
    state = (struct backend_state){0};
    EXPECT(surface_create(1, 1, 1, 1, false, &surface) == SURFACE_OK &&
               make_geometry(1, 1, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter) &&
               commit_tile(surface, 0, 0, 9),
           "create no-viewer presenter");
    EXPECT(presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.examined == 1 && stats.skipped == 1 && stats.sent == 0,
           "no eligible viewer pops dirty entry");
    struct presenter_resident_cursor cursor = {0};
    bool done = false;
    EXPECT(presenter_resend(&presenter, &viewer, &cursor, 1, &done, &stats) ==
               PRESENTER_OK && stats.examined == 1 && stats.sent == 1 &&
               done && state.last_byte == 9,
           "resident resend reaches re-entering viewer");
    EXPECT(surface_resident_count(surface) == 1 &&
               surface_read_resident(surface, 0,
                                      &(struct surface_resident_view){0}) ==
                   SURFACE_OK &&
               surface_read_resident(surface, 1,
                                      &(struct surface_resident_view){0}) ==
                   SURFACE_ERR_INDEX,
           "resident enumeration is sparse and index bounded");
    surface_destroy(surface);
}

static void test_filtering_and_distance(void)
{
    struct screen_pos block = {0, 0, 0};
    EXPECT(presenter_point_aabb_distance_squared(2.0, 0.5, 0.5, block) ==
               1.0 && presenter_point_aabb_distance_squared(0.5, 0.5, 0.5,
                                                             block) == 0.0,
           "point-to-block distance uses unit block bounds");
    struct surface *surface = nullptr;
    struct screen_geom geometry;
    struct backend_state state = {0};
    struct presenter presenter;
    struct presenter_stats stats;
    struct presenter_viewer far = viewer_at(100.0, 100.0, 100.0,
                                            "overworld", 31);
    EXPECT(surface_create(2, 1, 2, 2, false, &surface) == SURFACE_OK &&
               make_geometry(2, 1, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter) &&
               presenter_set_viewers(&presenter, &far, 1) == PRESENTER_OK &&
               commit_tile(surface, 0, 0, 1) &&
               presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.skipped == 1 && state.calls == 0,
           "far viewer is excluded");
    surface_destroy(surface);

    surface = nullptr;
    state = (struct backend_state){0};
    struct presenter_viewer mismatch = viewer_at(0.5, 0.5, 0.5,
                                                 "nether", 32);
    EXPECT(surface_create(2, 1, 2, 2, false, &surface) == SURFACE_OK &&
               make_geometry(2, 1, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter) &&
               presenter_set_viewers(&presenter, &mismatch, 1) ==
                   PRESENTER_OK &&
               commit_tile(surface, 0, 0, 1) &&
               presenter_tick(&presenter, 1, &stats) == PRESENTER_OK &&
               stats.skipped == 1 && state.calls == 0,
           "dimension mismatch is excluded");
    surface_destroy(surface);
}

static void test_resend_budget_and_large_surface(void)
{
    struct surface *surface = nullptr;
    struct screen_geom geometry;
    struct backend_state state = {0};
    struct presenter presenter;
    struct presenter_viewer viewer = viewer_at(0.5, 0.5, 0.5,
                                               "overworld", 41);
    struct presenter_stats stats;
    EXPECT(surface_create(2, 2, 4, 4, false, &surface) == SURFACE_OK &&
               make_geometry(2, 2, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter) &&
               commit_tile(surface, 0, 0, 1) && commit_tile(surface, 1, 0, 2) &&
               commit_tile(surface, 0, 1, 3),
           "create resend surface");
    struct presenter_resident_cursor cursor = {0};
    bool done = false;
    EXPECT(presenter_resend(&presenter, &viewer, &cursor, 1, &done, &stats) ==
               PRESENTER_OK && stats.examined == 1 && stats.sent == 1 &&
               !done && cursor.index == 1,
           "resend budget advances cursor once");
    EXPECT(presenter_resend(&presenter, &viewer, &cursor, 2, &done, &stats) ==
               PRESENTER_OK && stats.examined == 2 && stats.sent == 2 && done &&
               cursor.index == 3,
           "resend converges across ticks");
    surface_destroy(surface);

    surface = nullptr;
    state = (struct backend_state){0};
    EXPECT(surface_create(1024, 1024, 4, 4, false, &surface) == SURFACE_OK &&
               make_geometry(1024, 1024, &geometry) &&
               create_presenter(surface, &geometry, &state, &presenter),
           "create large sparse surface");
    struct presenter_viewer edge = viewer_at(1023.5, 0.5, 0.5,
                                             "overworld", 42);
    EXPECT(presenter_set_viewers(&presenter, &edge, 1) == PRESENTER_OK &&
               commit_tile(surface, 0, 0, 4) && commit_tile(surface, 1023, 1023,
                                                             5) &&
               presenter_tick(&presenter, 2, &stats) == PRESENTER_OK &&
               stats.examined == 2 && stats.sent == 1 && stats.skipped == 1 &&
               state.last_x == 1023 && state.last_y == 1023,
           "large logical surface schedules only nearby sparse tile");
    surface_destroy(surface);
}

struct stream_state {
    size_t reads;
    uint64_t fail_index;
    bool fail;
    uint8_t pixels[SURFACE_TILE_BYTES];
};

static bool stream_read(void *context, uint64_t tile_index, uint32_t tile_x,
                        uint32_t tile_y, struct presenter_stream_tile *out)
{
    (void)tile_x;
    (void)tile_y;
    struct stream_state *state = context;
    state->reads++;
    if (state->fail && tile_index == state->fail_index)
        return false;
    state->pixels[0] = (uint8_t)tile_index;
    out->pixels = state->pixels;
    out->bytes = sizeof(state->pixels);
    out->generation = tile_index + 1;
    return true;
}

static void test_stream_budget_and_source_order(void)
{
    struct surface *surface = nullptr;
    struct screen_geom geometry;
    struct backend_state backend = {0};
    struct stream_state source = {0};
    struct presenter presenter;
    struct presenter_stats stats;
    struct presenter_viewer viewer = viewer_at(0.5, 1023.5, 0.5,
                                               "overworld", 51);
    uint64_t cursor = 0;
    bool done = false;
    EXPECT(surface_create(1024, 1024, 1, 1, false, &surface) == SURFACE_OK &&
               make_geometry(1024, 1024, &geometry) &&
               create_presenter(surface, &geometry, &backend, &presenter) &&
               presenter_set_viewers(&presenter, nullptr, 0) == PRESENTER_OK &&
               presenter_stream(&presenter, UINT64_C(1024) * 1024, &cursor,
                                2, stream_read, &source, &done, &stats) ==
                   PRESENTER_OK &&
               stats.examined == 2 && stats.skipped == 2 && source.reads == 0 &&
               cursor == 2 && !done,
           "stream skips invisible tiles without reading source");
    EXPECT(presenter_set_viewers(&presenter, &viewer, 1) == PRESENTER_OK,
           "set stream viewer");
    source.fail = true;
    source.fail_index = 2;
    enum presenter_error stream_error = presenter_stream(
        &presenter, UINT64_C(1024) * 1024, &cursor, 2, stream_read, &source,
        &done, &stats);
    EXPECT(stream_error == PRESENTER_ERR_SOURCE && cursor == 2 &&
               stats.examined == 1 && stats.failures == 1 && !done,
           "source failure retains stream cursor");
    source.fail = false;
    EXPECT(presenter_stream(&presenter, UINT64_C(1024) * 1024, &cursor, 2,
                            stream_read, &source, &done, &stats) ==
               PRESENTER_OK && cursor == 4 && !done && stats.sent == 2 &&
               source.reads == 3,
           "stream resumes after source failure within budget");
    backend.fail = true;
    stream_error = presenter_stream(&presenter, UINT64_C(1024) * 1024,
                                    &cursor, 1, stream_read, &source, &done,
                                    &stats);
    EXPECT(stream_error == PRESENTER_ERR_BACKEND && cursor == 4 && !done &&
               stats.examined == 1 && stats.failures == 1,
           "backend failure retains stream cursor and clears done");
    backend.fail = false;
    viewer.x = 1023.5;
    viewer.y = 0.5;
    presenter_set_viewers(&presenter, &viewer, 1);
    cursor = UINT64_C(1024) * 1024 - 1;
    stream_error = presenter_stream(&presenter, UINT64_C(1024) * 1024,
                                    &cursor, 1, stream_read, &source, &done,
                                    &stats);
    EXPECT(stream_error == PRESENTER_OK && cursor == UINT64_C(1024) * 1024 &&
               done && stats.sent == 1,
           "completed stream reports done after final send");
    surface_destroy(surface);
}

int main(void)
{
    test_dirty_budget_and_latest();
    test_failure_and_no_viewer_resend();
    test_filtering_and_distance();
    test_resend_budget_and_large_surface();
    test_stream_budget_and_source_order();
    if (g_failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    puts("presenter tests passed");
    return EXIT_SUCCESS;
}

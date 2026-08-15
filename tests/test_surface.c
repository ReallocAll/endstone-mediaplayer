#include "mediaplayer/screen/surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        g_failures++; \
    } \
} while (0)

static void fill(uint8_t *pixels, size_t bytes, uint8_t value)
{
    memset(pixels, value, bytes);
}

static void test_create_and_bounds(void)
{
    struct surface *surface = nullptr;
    EXPECT(surface_create(1, 1, 2, 2, false, &surface) == SURFACE_OK,
           "create 1x1");
    struct surface_stats stats;
    EXPECT(surface_get_stats(surface, &stats) == SURFACE_OK &&
               stats.width_tiles == 1 && stats.height_tiles == 1 &&
               stats.pixel_width == 128 && stats.pixel_height == 128 &&
               stats.resident_tiles == 0 && stats.allocated_pixel_bytes == 0,
           "1x1 starts sparse");
    surface_destroy(surface);
    EXPECT(surface_create(1024, 1024, 1, 1, false, &surface) == SURFACE_OK,
           "large sparse surface");
    EXPECT(surface_get_stats(surface, &stats) == SURFACE_OK &&
               stats.allocated_pixel_bytes == 0,
           "large logical surface has no framebuffer allocation");
    surface_destroy(surface);
    EXPECT(surface_create(0, 1, 1, 1, false, &surface) == SURFACE_ERR_DIMENSION,
           "zero width rejected");
    EXPECT(surface_create(1025, 1, 1, 1, false, &surface) == SURFACE_ERR_DIMENSION,
           "width bound rejected");
    EXPECT(surface_create(1, 1, 0, 1, false, &surface) == SURFACE_ERR_CAPACITY,
           "zero resident cap rejected");
    EXPECT(surface_create(1, 1, 1, 0, false, &surface) == SURFACE_ERR_CAPACITY,
           "zero pending cap rejected");
    EXPECT(surface_create(1024, 1024, SURFACE_MAX_RESIDENT_TILES + 1,
                          1, false, &surface) == SURFACE_ERR_CAPACITY,
           "hard resident bound rejected");
}

static int update_one(struct surface *surface, enum surface_pixel_format format,
                      const uint8_t pixel[4])
{
    struct surface_frame *frame = nullptr;
    if (surface_frame_begin(surface, &frame) != SURFACE_OK)
        return 0;
    uint8_t tile[SURFACE_TILE_BYTES];
    memset(tile, 0, sizeof(tile));
    memcpy(tile, pixel, 4);
    if (surface_frame_update_tile(frame, 0, 0, tile, sizeof(tile),
                                  SURFACE_TILE_ROW_BYTES, format) != SURFACE_OK)
        return 0;
    return surface_frame_commit(frame) == SURFACE_OK;
}

static void test_formats_and_generation(void)
{
    struct surface *surface = nullptr;
    EXPECT(surface_create(1, 1, 2, 4, false, &surface) == SURFACE_OK,
           "format surface create");
    uint8_t rgba[4] = {1, 2, 3, 4};
    EXPECT(update_one(surface, SURFACE_FORMAT_RGBA8888, rgba), "RGBA update");
    struct surface_tile_view view;
    EXPECT(surface_read_tile(surface, 0, 0, &view) == SURFACE_OK &&
               memcmp(view.pixels, rgba, 4) == 0 && view.generation == 1 &&
               view.content_hash != 0,
           "RGBA canonical bytes, generation, and identity");
    struct surface_stats stats;
    surface_get_stats(surface, &stats);
    EXPECT(stats.pending_tiles == 1 && stats.generation == 1,
           "first update dirty");

    struct surface_frame *frame = nullptr;
    EXPECT(surface_frame_begin(surface, &frame) == SURFACE_OK,
           "unchanged frame begin");
    uint8_t same[SURFACE_TILE_BYTES] = {0};
    memcpy(same, rgba, 4);
    EXPECT(surface_frame_update_tile(frame, 0, 0, same, sizeof(same),
                                     SURFACE_TILE_ROW_BYTES,
                                     SURFACE_FORMAT_ABGR8888) == SURFACE_OK &&
               surface_frame_commit(frame) == SURFACE_OK,
           "unchanged commit");
    surface_get_stats(surface, &stats);
    EXPECT(stats.generation == 1 && stats.pending_tiles == 1,
           "unchanged commit suppressed");

    uint8_t bgra[4] = {30, 20, 10, 40};
    EXPECT(update_one(surface, SURFACE_FORMAT_BGRA8888, bgra), "BGRA update");
    EXPECT(surface_read_tile(surface, 0, 0, &view) == SURFACE_OK &&
               view.pixels[0] == 10 && view.pixels[1] == 20 &&
               view.pixels[2] == 30 && view.pixels[3] == 40 &&
               view.generation == 2,
           "BGRA normalized");
    surface_frame_begin(surface, &frame);
    uint8_t one[SURFACE_TILE_BYTES] = {0};
    one[0] = 11;
    surface_frame_update_tile(frame, 0, 0, one, sizeof(one),
                              SURFACE_TILE_ROW_BYTES, SURFACE_FORMAT_RGBA8888);
    surface_frame_abort(frame);
    surface_get_stats(surface, &stats);
    EXPECT(stats.generation == 2 && stats.pending_tiles == 1,
           "abort changes nothing");
    surface_destroy(surface);
}

static void test_full_7x4_and_hash_suppression(void)
{
    struct surface *surface = nullptr;
    EXPECT(surface_create(7, 4, 28, 28, false, &surface) == SURFACE_OK,
           "7x4 surface create");
    size_t frame_bytes = (size_t)896 * 512 * 4;
    uint8_t *pixels = calloc(1, frame_bytes);
    EXPECT(pixels != nullptr, "allocate bounded 7x4 producer frame");
    if (pixels == nullptr) {
        surface_destroy(surface);
        return;
    }
    pixels[0] = 9;
    struct surface_frame *frame = nullptr;
    EXPECT(surface_frame_begin(surface, &frame) == SURFACE_OK &&
               surface_frame_update_region(
                   frame, 0, 0, 896, 512, pixels, frame_bytes,
                   (size_t)896 * 4, SURFACE_FORMAT_RGBA8888) == SURFACE_OK &&
               surface_frame_commit(frame) == SURFACE_OK,
           "full 7x4 commit");
    struct surface_stats stats;
    surface_get_stats(surface, &stats);
    EXPECT(stats.resident_tiles == 1 && stats.pending_tiles == 1 &&
               stats.generation == 1,
           "zero tiles stay sparse while changed tile becomes resident");
    struct surface_tile_view before;
    surface_read_tile(surface, 0, 0, &before);
    uint32_t first_hash = before.content_hash;

    EXPECT(surface_frame_begin(surface, &frame) == SURFACE_OK &&
               surface_frame_update_region(
                   frame, 0, 0, 896, 512, pixels, frame_bytes,
                   (size_t)896 * 4, SURFACE_FORMAT_RGBA8888) == SURFACE_OK &&
               surface_frame_commit(frame) == SURFACE_OK,
           "identical full frame commit");
    surface_get_stats(surface, &stats);
    EXPECT(stats.generation == 1 && stats.pending_tiles == 1,
           "identical full frame is suppressed");

    pixels[(size_t)128 * 4] = 7; // First pixel of tile (1,0).
    EXPECT(surface_frame_begin(surface, &frame) == SURFACE_OK &&
               surface_frame_update_region(
                   frame, 0, 0, 896, 512, pixels, frame_bytes,
                   (size_t)896 * 4, SURFACE_FORMAT_RGBA8888) == SURFACE_OK &&
               surface_frame_commit(frame) == SURFACE_OK,
           "one-tile change commit");
    struct surface_tile_view changed;
    surface_read_tile(surface, 1, 0, &changed);
    surface_read_tile(surface, 0, 0, &before);
    EXPECT(changed.generation == 2 && changed.content_hash != 0 &&
               before.content_hash == first_hash,
           "one-tile change preserves unchanged tile identity");
    free(pixels);
    surface_destroy(surface);
}

static void test_region_and_staging(void)
{
    struct surface *surface = nullptr;
    EXPECT(surface_create(2, 2, 8, 8, false, &surface) == SURFACE_OK,
           "region surface create");
    struct surface_frame *frame = nullptr;
    EXPECT(surface_frame_begin(surface, &frame) == SURFACE_OK,
           "region frame begin");
    const uint32_t width = 4;
    const uint32_t height = 3;
    uint8_t region[width * height * 4];
    for (size_t i = 0; i < sizeof(region); i += 4) {
        region[i] = 100;
        region[i + 1] = 101;
        region[i + 2] = 102;
        region[i + 3] = 103;
    }
    EXPECT(surface_frame_update_region(frame, 127, 127, width, height, region,
                                       sizeof(region), width * 4,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_OK,
           "cross-tile region");
    uint8_t one[4] = {7, 8, 9, 10};
    EXPECT(surface_frame_update_region(frame, 0, 0, 1, 1, one, sizeof(one), 4,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_OK &&
               surface_frame_commit(frame) == SURFACE_OK,
           "multiple staging updates");
    struct surface_tile_view view;
    EXPECT(surface_read_tile(surface, 0, 0, &view) == SURFACE_OK &&
               memcmp(view.pixels, one, 4) == 0 &&
               memcmp(view.pixels + (127 * SURFACE_TILE_ROW_BYTES + 127 * 4),
                      region, 4) == 0,
           "top-left tile staged content");
    EXPECT(surface_read_tile(surface, 1, 1, &view) == SURFACE_OK &&
               memcmp(view.pixels, region + 2 * 4 * width, 4) == 0,
           "bottom-right tile staged content");
    surface_destroy(surface);
}

static void test_capacity_and_latest_wins(void)
{
    struct surface *surface = nullptr;
    EXPECT(surface_create(2, 1, 1, 1, false, &surface) == SURFACE_OK,
           "static capacity create");
    uint8_t tile[SURFACE_TILE_BYTES] = {0};
    tile[0] = 1;
    struct surface_frame *frame = nullptr;
    surface_frame_begin(surface, &frame);
    surface_frame_update_tile(frame, 0, 0, tile, sizeof(tile),
                              SURFACE_TILE_ROW_BYTES, SURFACE_FORMAT_RGBA8888);
    EXPECT(surface_frame_commit(frame) == SURFACE_OK, "first static commit");
    tile[0] = 2;
    surface_frame_begin(surface, &frame);
    surface_frame_update_tile(frame, 1, 0, tile, sizeof(tile),
                              SURFACE_TILE_ROW_BYTES, SURFACE_FORMAT_RGBA8888);
    EXPECT(surface_frame_commit(frame) == SURFACE_ERR_CAPACITY,
           "resident capacity rejects atomically");
    surface_frame_abort(frame);
    struct surface_stats stats;
    surface_get_stats(surface, &stats);
    EXPECT(stats.resident_tiles == 1 && stats.generation == 1,
           "resident rejection did not mutate");

    size_t region_bytes = (size_t)256 * 128 * 4;
    uint8_t *region = calloc(1, region_bytes);
    surface_frame_begin(surface, &frame);
    EXPECT(region != nullptr &&
               surface_frame_update_region(
                   frame, 0, 0, 256, 128, region, region_bytes,
                   (size_t)256 * 4, SURFACE_FORMAT_RGBA8888) ==
                   SURFACE_ERR_CAPACITY,
           "oversized transaction is rejected before staging");
    surface_get_stats(surface, &stats);
    EXPECT(stats.staged_tiles == 0 &&
               stats.allocated_staging_pixel_bytes == 0,
           "failed oversized update stages no partial tiles");
    surface_frame_abort(frame);
    free(region);
    surface_destroy(surface);

    EXPECT(surface_create(3, 1, 3, 2, true, &surface) == SURFACE_OK,
           "latest wins create");
    for (uint32_t x = 0; x < 3; x++) {
        memset(tile, 0, sizeof(tile));
        tile[0] = (uint8_t)(x + 1);
        surface_frame_begin(surface, &frame);
        surface_frame_update_tile(frame, x, 0, tile, sizeof(tile),
                                  SURFACE_TILE_ROW_BYTES,
                                  SURFACE_FORMAT_RGBA8888);
        EXPECT(surface_frame_commit(frame) == SURFACE_OK, "latest commit");
    }
    surface_get_stats(surface, &stats);
    EXPECT(stats.pending_tiles == 2 && stats.dropped_tiles == 1,
           "latest wins drops oldest pending");
    struct surface_dirty_view dirty;
    EXPECT(surface_dirty_pop(surface, &dirty) == SURFACE_OK &&
               dirty.tile_x == 1 && dirty.pixels[0] == 2 &&
               dirty.content_hash != 0,
           "dirty pop returns newest state in FIFO");
    EXPECT(surface_dirty_pop(surface, &dirty) == SURFACE_OK &&
               dirty.tile_x == 2 && dirty.pixels[0] == 3,
           "second dirty survives");
    surface_destroy(surface);
}

static void test_validation(void)
{
    struct surface *surface = nullptr;
    EXPECT(surface_create(1, 1, 2, 2, false, &surface) == SURFACE_OK,
           "validation surface create");
    struct surface_frame *frame = nullptr;
    surface_frame_begin(surface, &frame);
    uint8_t pixel[4] = {0};
    EXPECT(surface_frame_update_region(frame, 0, 0, 0, 1, pixel, 4, 4,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_ERR_RECTANGLE,
           "zero rectangle rejected");
    EXPECT(surface_frame_update_region(frame, 128, 0, 1, 1, pixel, 4, 4,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_ERR_RECTANGLE,
           "out of bounds rejected");
    EXPECT(surface_frame_update_region(frame, 0, 0, 1, 1, pixel, 4, 3,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_ERR_STRIDE,
           "short stride rejected");
    EXPECT(surface_frame_update_region(frame, 0, 0, 1, 2, pixel, 4, 4,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_ERR_BUFFER,
           "short buffer rejected");
    EXPECT(surface_frame_update_region(frame, 0, 0, 1, 1, pixel, 4, 4,
                                       (enum surface_pixel_format)99) == SURFACE_ERR_FORMAT,
           "bad format rejected");
    EXPECT(surface_frame_update_region(frame, 0, 0, 1, 1, nullptr, 4, 4,
                                       SURFACE_FORMAT_RGBA8888) == SURFACE_ERR_ARGUMENT,
           "null pixels rejected");
    surface_frame_abort(frame);
    surface_destroy(surface);
}

int main(void)
{
    test_create_and_bounds();
    test_formats_and_generation();
    test_full_7x4_and_hash_suppression();
    test_region_and_staging();
    test_capacity_and_latest_wins();
    test_validation();
    if (g_failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    puts("surface tests passed");
    return EXIT_SUCCESS;
}

#ifndef ENDSTONE_MEDIAPLAYER_MAP_MAP_RENDER_H
#define ENDSTONE_MEDIAPLAYER_MAP_MAP_RENDER_H

#include "mediaplayer/screen/screen_registry.h"
#include "mediaplayer/screen/presenter.h"
#include "mediaplayer/video/video_session.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct map_render_ctx {
    void *server; // Server used for map creation and lookup.
};

enum map_test_pattern {
    MAP_TEST_NONE = 0,
    MAP_TEST_RED,
    MAP_TEST_GREEN,
    MAP_TEST_BLUE,
    MAP_TEST_CHECKER,
    MAP_TEST_QUADRANTS,
};

enum map_render_error {
    MAP_RENDER_OK = 0,
    MAP_RENDER_ERR_INIT = -1,
    MAP_RENDER_ERR_UNSUPPORTED = -2,
    MAP_RENDER_ERR_MAP_NOT_FOUND = -3,
    MAP_RENDER_ERR_SURFACE = -4,
    MAP_RENDER_ERR_PRESENTER = -5,
};

struct map_renderer_stats {
    uint64_t initialize_count;
    uint64_t render_count;
    uint64_t destroy_count;
    void *last_canvas;
    void *last_map_view;
    void *last_player;
    void *renderer;
    void *renderer_vptr;
    int strong_references;
    bool last_canvas_valid;
    bool last_write_verified;
    size_t last_canvas_pixels;
    uint32_t last_first_pixel;
    uint32_t last_last_pixel;
};

void map_render_init(struct map_render_ctx *ctx, void *server, void *plugin);

// Creates, locks, and attaches renderers to every map tile.
enum map_render_error map_render_init_screen(
    struct map_render_ctx *ctx, struct screen_entry *screen, void *player);

// Restores a persisted screen using only its complete set of existing map IDs.
enum map_render_error map_render_restore_screen(
    struct map_render_ctx *ctx, struct screen_entry *screen, void *player);

// Detaches and destroys a screen's renderers.
void map_render_destroy_screen(struct map_render_ctx *ctx, struct screen_entry *screen);

// Commits a producer frame into the screen Surface.
enum map_render_error map_render_submit_frame(
    struct screen_entry *screen, const uint8_t *frame_buf,
    size_t frame_bytes, enum surface_pixel_format format);

// Presents at most budget dirty Surface tiles to eligible viewers.
enum map_render_error map_render_present(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewers, size_t viewer_count,
    size_t budget, struct presenter_stats *stats);

// Clears a screen's maps to black.
void map_render_clear(struct map_render_ctx *ctx, struct screen_entry *screen);

uint32_t map_render_pattern_pixel(enum map_test_pattern pattern,
                                  int x, int y, int width, int height);

int map_render_set_test_pattern(struct map_render_ctx *ctx,
                                struct screen_entry *screen,
                                enum map_test_pattern pattern,
                                void **players, const char **player_ids,
                                int player_count);

int map_render_resend(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewer,
    struct presenter_resident_cursor *cursor, size_t budget,
    bool *done, struct presenter_stats *stats);

// Presents a resumable source stream through the existing synchronous map
// backend. The source callback's tile pointer is borrowed for this call only.
enum map_render_error map_render_present_stream(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewers, size_t viewer_count,
    uint64_t tile_count, uint64_t *cursor, size_t budget,
    bool (*read_tile)(void *context, uint64_t tile_index, uint32_t tile_x,
                      uint32_t tile_y, struct presenter_stream_tile *out),
    void *read_context, bool *done, struct presenter_stats *stats);

enum map_render_error map_render_resend_stream(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewer, uint64_t tile_count,
    uint64_t *cursor, size_t budget,
    bool (*read_tile)(void *context, uint64_t tile_index, uint32_t tile_x,
                      uint32_t tile_y, struct presenter_stream_tile *out),
    void *read_context, bool *done, struct presenter_stats *stats);

int map_render_hide_viewer(struct map_render_ctx *ctx,
                           const struct screen_entry *screen,
                           void *player, const char *player_id);

bool map_render_get_stats(const struct screen_entry *screen, int tile,
                          struct map_renderer_stats *stats);

#endif // ENDSTONE_MEDIAPLAYER_MAP_MAP_RENDER_H

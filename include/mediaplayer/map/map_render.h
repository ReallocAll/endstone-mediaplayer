#ifndef ENDSTONE_MEDIAPLAYER_MAP_MAP_RENDER_H
#define ENDSTONE_MEDIAPLAYER_MAP_MAP_RENDER_H

#include "mediaplayer/screen/screen_registry.h"
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

// Detaches and destroys a screen's renderers.
void map_render_destroy_screen(struct map_render_ctx *ctx, struct screen_entry *screen);

// Sends changed tiles to eligible public viewers.
void map_render_send_frame(struct map_render_ctx *ctx,
                           struct screen_entry *screen,
                           const uint8_t *frame_buf,
                           void **players, const char **player_ids,
                           int player_count);

// Clears a screen's maps to black.
void map_render_clear(struct map_render_ctx *ctx, struct screen_entry *screen);

uint32_t map_render_pattern_pixel(enum map_test_pattern pattern,
                                  int x, int y, int width, int height);

int map_render_set_test_pattern(struct map_render_ctx *ctx,
                                struct screen_entry *screen,
                                enum map_test_pattern pattern,
                                void **players, const char **player_ids,
                                int player_count);

int map_render_resend(struct map_render_ctx *ctx,
                      const struct screen_entry *screen,
                      void **players, const char **player_ids,
                      int player_count);

int map_render_hide_viewer(struct map_render_ctx *ctx,
                           const struct screen_entry *screen,
                           void *player, const char *player_id);

bool map_render_get_stats(const struct screen_entry *screen, int tile,
                          struct map_renderer_stats *stats);

#endif // ENDSTONE_MEDIAPLAYER_MAP_MAP_RENDER_H

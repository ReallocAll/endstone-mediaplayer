#include "mediaplayer/map/map_render.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/bedrock/world_bridge.h"
#include "endstone_abi.h"
#include "abi_helpers.h"
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *g_map_log_plugin = nullptr;

static void map_log(const char *fmt, ...)
{
    if (!g_map_log_plugin) {
        return;
    }
    char buffer[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    PLUGIN_LOG(g_map_log_plugin, ES_LOG_INFO, buffer);
}

#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)

// MapRenderer has a 16-byte base on both supported ABIs.
struct tile_renderer {
    void **vtable;
    bool is_contextual;
    unsigned char base_padding[7];
    int tile_col;
    int tile_row;
    char screen_name[SCREEN_NAME_MAX];
    const uint8_t *frame_ptr;
    int pixel_width;
    int pixel_height;
    enum map_test_pattern pattern;
    uint64_t initialize_count;
    uint64_t render_count;
    uint64_t destroy_count;
    void *last_canvas;
    void *last_map_view;
    void *last_player;
    bool last_canvas_valid;
    bool last_write_verified;
    size_t last_canvas_pixels;
    uint32_t last_first_pixel;
    uint32_t last_last_pixel;
};

_Static_assert(offsetof(struct tile_renderer, is_contextual) ==
                   ES_MAPRENDERER_OFF_IS_CONTEXTUAL,
               "MapRenderer contextual flag offset mismatch");
_Static_assert(offsetof(struct tile_renderer, tile_col) == ES_MAPRENDERER_SIZE,
               "MapRenderer base size mismatch");

struct ref_count_block {
    void **vtable;
#if defined(ES_PLATFORM_WINDOWS)
    int uses;
    int weaks;
#else
    long uses;
    long weaks;
#endif
};

_Static_assert(sizeof(struct ref_count_block) == ES_REFCOUNT_SIZE,
               "shared_ptr control block size mismatch");
_Static_assert(offsetof(struct ref_count_block, uses) == ES_REFCOUNT_OFF_USES,
               "shared_ptr strong count offset mismatch");
_Static_assert(offsetof(struct ref_count_block, weaks) == ES_REFCOUNT_OFF_WEAKS,
               "shared_ptr weak count offset mismatch");

struct renderer_alloc {
    struct tile_renderer renderer;
    struct ref_count_block control;
};


static void renderer_destroy(void *self);
static bool renderer_is_endstone(void *self);
static void renderer_initialize(void *self, void *map_view);
static void renderer_render(void *self, void *map_view, void *canvas,
                            void *player);

#if defined(ES_PLATFORM_WINDOWS)
static void renderer_dtor(void *self, unsigned int flags)
{
    (void)flags;
    renderer_destroy(self);
}

static void *g_renderer_vtable[] = {
    (void *)renderer_dtor,
    (void *)renderer_is_endstone,
    (void *)renderer_initialize,
    (void *)renderer_render,
};
#else
static void renderer_complete_dtor(void *self)
{
    renderer_destroy(self);
}

static void renderer_deleting_dtor(void *self)
{
    renderer_destroy(self);
}

static void *g_renderer_vtable[] = {
    nullptr,
    nullptr,
    (void *)renderer_complete_dtor,
    (void *)renderer_deleting_dtor,
    (void *)renderer_is_endstone,
    (void *)renderer_initialize,
    (void *)renderer_render,
};
#endif

static void renderer_destroy(void *self)
{
    struct tile_renderer *renderer = self;
    renderer->destroy_count++;
}

static bool renderer_is_endstone(void *self)
{
    (void)self;
    return false;
}

static void renderer_initialize(void *self, void *map_view)
{
    struct tile_renderer *renderer = self;
    renderer->initialize_count++;
    renderer->last_map_view = map_view;
}

uint32_t map_render_pattern_pixel(enum map_test_pattern pattern,
                                  int x, int y, int width, int height)
{
    switch (pattern) {
    case MAP_TEST_RED:
        return UINT32_C(0xff0000ff);
    case MAP_TEST_GREEN:
        return UINT32_C(0xff00ff00);
    case MAP_TEST_BLUE:
        return UINT32_C(0xffff0000);
    case MAP_TEST_CHECKER:
        return (((x / 16) + (y / 16)) & 1) ? UINT32_C(0xffffffff)
                                            : UINT32_C(0xff000000);
    case MAP_TEST_QUADRANTS:
        if (y < height / 2) {
            return x < width / 2 ? UINT32_C(0xff0000ff)
                                 : UINT32_C(0xff00ff00);
        }
        return x < width / 2 ? UINT32_C(0xffff0000)
                             : UINT32_C(0xffffffff);
    case MAP_TEST_NONE:
    default:
        return 0;
    }
}

static bool canvas_buffer(void *canvas, uint32_t **out, size_t *pixel_count)
{
    uint32_t *begin = *(uint32_t **)((char *)canvas +
                                     ES_MAPCANVAS_OFF_BUFFER_BEGIN);
    uint32_t *end = *(uint32_t **)((char *)canvas +
                                   ES_MAPCANVAS_OFF_BUFFER_END);
    if (!begin || !end || end < begin) {
        return false;
    }
    size_t count = (size_t)(end - begin);
    if (count < SCREEN_TILE_SIZE * SCREEN_TILE_SIZE) return false;
    *out = begin;
    *pixel_count = count;
    return true;
}

static void renderer_render(void *self, void *map_view, void *canvas,
                            void *player)
{
    struct tile_renderer *renderer = self;
    renderer->render_count++;
    renderer->last_map_view = map_view;
    renderer->last_canvas = canvas;
    renderer->last_player = player;
    renderer->last_canvas_valid = false;
    renderer->last_write_verified = false;
    renderer->last_canvas_pixels = 0;

    uint32_t *destination = nullptr;
    size_t pixel_count = 0;
    if (!canvas_buffer(canvas, &destination, &pixel_count)) {
        static int invalid_canvas_logs;
        if (invalid_canvas_logs++ < 4) {
            map_log("map renderer rejected canvas=%p: invalid 128x128 buffer", canvas);
        }
        return;
    }
    renderer->last_canvas_valid = true;
    renderer->last_canvas_pixels = pixel_count;

    if (renderer->pattern != MAP_TEST_NONE) {
        int origin_x = renderer->tile_col * SCREEN_TILE_SIZE;
        int origin_y = renderer->tile_row * SCREEN_TILE_SIZE;
        for (int y = 0; y < SCREEN_TILE_SIZE; y++) {
            for (int x = 0; x < SCREEN_TILE_SIZE; x++) {
                destination[y * SCREEN_TILE_SIZE + x] = map_render_pattern_pixel(
                    renderer->pattern, origin_x + x, origin_y + y,
                    renderer->pixel_width, renderer->pixel_height);
            }
        }
    } else if (renderer->frame_ptr) {
        int start_x = renderer->tile_col * SCREEN_TILE_SIZE;
        int start_y = renderer->tile_row * SCREEN_TILE_SIZE;
        const uint32_t *source = (const uint32_t *)renderer->frame_ptr;
        for (int row = 0; row < SCREEN_TILE_SIZE; row++) {
            const uint32_t *source_row =
                source + (size_t)(start_y + row) * renderer->pixel_width +
                start_x;
            uint32_t *destination_row =
                destination + (size_t)row * SCREEN_TILE_SIZE;
            memcpy(destination_row, source_row, SCREEN_TILE_SIZE * sizeof(uint32_t));
        }
    }

    renderer->last_first_pixel = destination[0];
    renderer->last_last_pixel =
        destination[SCREEN_TILE_SIZE * SCREEN_TILE_SIZE - 1];
    if (renderer->pattern != MAP_TEST_NONE) {
        int origin_x = renderer->tile_col * SCREEN_TILE_SIZE;
        int origin_y = renderer->tile_row * SCREEN_TILE_SIZE;
        uint32_t expected_first = map_render_pattern_pixel(
            renderer->pattern, origin_x, origin_y,
            renderer->pixel_width, renderer->pixel_height);
        uint32_t expected_last = map_render_pattern_pixel(
            renderer->pattern, origin_x + SCREEN_TILE_SIZE - 1,
            origin_y + SCREEN_TILE_SIZE - 1,
            renderer->pixel_width, renderer->pixel_height);
        renderer->last_write_verified =
            renderer->last_first_pixel == expected_first &&
            renderer->last_last_pixel == expected_last;
    } else if (renderer->frame_ptr) {
        renderer->last_write_verified = true;
    }

}

static void control_destroy_resource(void *self)
{
    struct ref_count_block *control = self;
    struct renderer_alloc *allocation =
        (struct renderer_alloc *)((char *)control -
                                  offsetof(struct renderer_alloc, control));
    renderer_destroy(&allocation->renderer);
}

static void control_delete_this(void *self)
{
    struct ref_count_block *control = self;
    struct renderer_alloc *allocation =
        (struct renderer_alloc *)((char *)control -
                                  offsetof(struct renderer_alloc, control));
    free(allocation);
}

#if defined(ES_PLATFORM_WINDOWS)
static void *g_control_vtable[] = {
    (void *)control_destroy_resource,
    (void *)control_delete_this,
};
#else
static void control_complete_dtor(void *self)
{
    (void)self;
}

static void control_deleting_dtor(void *self)
{
    control_delete_this(self);
}

static void *control_get_deleter(void *self, const void *type_info)
{
    (void)self;
    (void)type_info;
    return nullptr;
}

static void *g_control_vtable[] = {
    nullptr,
    nullptr,
    (void *)control_complete_dtor,
    (void *)control_deleting_dtor,
    (void *)control_destroy_resource,
    (void *)control_get_deleter,
    (void *)control_delete_this,
};
#endif

static struct renderer_alloc *renderer_create(int col, int row,
                                              const char *screen_name,
                                              int pixel_width,
                                              int pixel_height)
{
    struct renderer_alloc *allocation = calloc(1, sizeof(*allocation));
    if (!allocation) {
        return nullptr;
    }
#if defined(ES_PLATFORM_WINDOWS)
    allocation->renderer.vtable = g_renderer_vtable;
#else
    allocation->renderer.vtable = &g_renderer_vtable[2];
#endif
    allocation->renderer.is_contextual = false;
    allocation->renderer.tile_col = col;
    allocation->renderer.tile_row = row;
    snprintf(allocation->renderer.screen_name,
             sizeof(allocation->renderer.screen_name), "%s", screen_name);
    allocation->renderer.pixel_width = pixel_width;
    allocation->renderer.pixel_height = pixel_height;
#if defined(ES_PLATFORM_WINDOWS)
    allocation->control.vtable = g_control_vtable;

    // Retain one owner after addRenderer consumes its parameter.
    allocation->control.uses = 2;
    allocation->control.weaks = 1;
#else
    allocation->control.vtable = &g_control_vtable[2];
    allocation->control.uses = 1;
    allocation->control.weaks = 0;
#endif
    return allocation;
}

#if defined(ES_PLATFORM_WINDOWS)
static struct es_msvc_shared_ptr renderer_shared(struct renderer_alloc *allocation)
{
    struct es_msvc_shared_ptr shared = {
        .ptr = &allocation->renderer,
        .control = &allocation->control,
    };
    return shared;
}
#else
static struct es_libcxx_shared_ptr renderer_shared(struct renderer_alloc *allocation)
{
    struct es_libcxx_shared_ptr shared = {
        .ptr = &allocation->renderer,
        .control = &allocation->control,
    };
    return shared;
}
#endif

// tile_dirty selects which tiles to transmit; nullptr sends every tile,
// which is what a new viewer, a test pattern or a forced resend needs.
static void send_tiles(const struct screen_entry *screen,
                       void **players, const char **player_ids,
                       int player_count, const bool *tile_dirty)
{
    int tile_count = screen_geom_tile_count(&screen->geom);
    for (int p = 0; p < player_count; p++) {
        void *player = players[p];
        if (!player) {
            continue;
        }
        for (int tile = 0; tile < tile_count; tile++) {
            if (tile_dirty && !tile_dirty[tile]) {
                continue;
            }
            if (screen->tiles[tile].valid && screen->tiles[tile].map_view) {
                es_player_send_map(player, screen->tiles[tile].map_view);
            }
        }
    }
}

#endif

void map_render_init(struct map_render_ctx *ctx, void *server, void *plugin)
{
    ctx->server = server;
    g_map_log_plugin = plugin;
    if (!es_map_abi_supported()) {
        map_log("map video is disabled: no verified map ABI for this platform");
    }
}

enum map_render_error map_render_init_screen(struct map_render_ctx *ctx,
                                             struct screen_entry *screen,
                                             void *player)
{
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!player || screen->tiles_initialized) {
        return MAP_RENDER_ERR_INIT;
    }

    int tile_count = screen_geom_tile_count(&screen->geom);
    int pixel_width = screen_geom_pixel_width(&screen->geom);
    int pixel_height = screen_geom_pixel_height(&screen->geom);

    for (int i = 0; i < tile_count; i++) {
        void *map_view = screen->tiles[i].map_id_valid
            ? mp_world_get_map(ctx->server, screen->tiles[i].map_id)
            : mp_world_create_map_for_player(ctx->server, player,
                                             screen->geom.dimension,
                                             nullptr, 0);
        if (!map_view) {
            map_render_destroy_screen(ctx, screen);
            return screen->tiles[i].map_id_valid
                ? MAP_RENDER_ERR_MAP_NOT_FOUND
                : MAP_RENDER_ERR_INIT;
        }

        struct renderer_alloc *allocation = renderer_create(
            i % screen->geom.width, i / screen->geom.width, screen->name,
            pixel_width, pixel_height);
        if (!allocation) {
            map_render_destroy_screen(ctx, screen);
            return MAP_RENDER_ERR_INIT;
        }

#if defined(ES_PLATFORM_WINDOWS)
        struct es_msvc_shared_ptr parameter = renderer_shared(allocation);
#else
        struct es_libcxx_shared_ptr parameter = renderer_shared(allocation);
#endif
        es_map_view_add_renderer(map_view, &parameter);
        // parameter has been destroyed by addRenderer; do not release it here.
        es_map_view_set_locked(map_view, true);

        screen->tiles[i].renderer = allocation;
        screen->tiles[i].map_view = map_view;
        screen->tiles[i].map_id = es_map_view_get_id(map_view);
        screen->tiles[i].map_id_valid = 1;
        screen->tiles[i].valid = 1;
    }

    screen->tiles_initialized = 1;
    return MAP_RENDER_OK;
#else
    (void)ctx;
    (void)screen;
    (void)player;
    return MAP_RENDER_ERR_UNSUPPORTED;
#endif
}

void map_render_destroy_screen(struct map_render_ctx *ctx,
                               struct screen_entry *screen)
{
    (void)ctx;
    int tile_count = screen_geom_tile_count(&screen->geom);
    for (int i = 0; i < tile_count; i++) {
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (allocation) {
            // Prevent callbacks from observing a session buffer after stop.
            allocation->renderer.frame_ptr = nullptr;
#if defined(ES_PLATFORM_WINDOWS)
            struct es_msvc_shared_ptr owner = renderer_shared(allocation);
#else
            struct es_libcxx_shared_ptr owner = renderer_shared(allocation);
#endif
            if (screen->tiles[i].map_view) {
                es_map_view_remove_renderer(screen->tiles[i].map_view, &owner);
            }
#if defined(ES_PLATFORM_WINDOWS)
            es_msvc_shared_ptr_release(&owner);
#else
            es_libcxx_shared_ptr_release(&owner);
#endif
        }
#endif
        screen->tiles[i].renderer = nullptr;
        screen->tiles[i].valid = 0;
        screen->tiles[i].map_view = nullptr;
        free(screen->tiles[i].last_sent);
        screen->tiles[i].last_sent = nullptr;
        screen->tiles[i].last_sent_valid = 0;
    }
    screen->tiles_initialized = 0;
}

#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)

#define MAP_TILE_BYTES ((size_t)SCREEN_TILE_SIZE * SCREEN_TILE_SIZE * 4)
#define MAP_TILE_ROW_BYTES ((size_t)SCREEN_TILE_SIZE * 4)

// Updates the tile cache and returns whether the tile changed.
static bool tile_changed(struct screen_tile_rt *tile, int tile_col,
                         int tile_row, const uint8_t *frame_buf,
                         int pixel_width)
{
    if (!tile->last_sent) {
        tile->last_sent = malloc(MAP_TILE_BYTES);
        if (!tile->last_sent) {
            return true; // Cannot track this tile: always send it.
        }
        tile->last_sent_valid = 0;
    }

    bool changed = !tile->last_sent_valid;
    for (int row = 0; row < SCREEN_TILE_SIZE; row++) {
        size_t source_offset =
            (((size_t)(tile_row * SCREEN_TILE_SIZE + row) * (size_t)pixel_width) +
             (size_t)tile_col * SCREEN_TILE_SIZE) * 4;
        const uint8_t *source = frame_buf + source_offset;
        uint8_t *stored = tile->last_sent + (size_t)row * MAP_TILE_ROW_BYTES;
        // Once a difference is found the remaining rows only need copying.
        if (!changed && memcmp(stored, source, MAP_TILE_ROW_BYTES) != 0) {
            changed = true;
        }
        if (changed) {
            memcpy(stored, source, MAP_TILE_ROW_BYTES);
        }
    }
    tile->last_sent_valid = 1;
    return changed;
}

static void invalidate_sent_tiles(struct screen_entry *screen)
{
    int tile_count = screen_geom_tile_count(&screen->geom);
    for (int i = 0; i < tile_count; i++) {
        screen->tiles[i].last_sent_valid = 0;
    }
}

#endif

void map_render_send_frame(struct map_render_ctx *ctx,
                           struct screen_entry *screen,
                           const uint8_t *frame_buf,
                           void **players, const char **player_ids,
                           int player_count)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!frame_buf || !screen->tiles_initialized) {
        return;
    }
    int pixel_width = screen_geom_pixel_width(&screen->geom);
    int tile_count = screen_geom_tile_count(&screen->geom);
    bool tile_dirty[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT] = {false};
    bool any_dirty = false;

    for (int i = 0; i < tile_count; i++) {
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (allocation) {
            allocation->renderer.pattern = MAP_TEST_NONE;
            allocation->renderer.frame_ptr = frame_buf;
            allocation->renderer.pixel_width = pixel_width;
            allocation->renderer.pixel_height =
                screen_geom_pixel_height(&screen->geom);
            tile_dirty[i] = tile_changed(&screen->tiles[i],
                                         allocation->renderer.tile_col,
                                         allocation->renderer.tile_row,
                                         frame_buf, pixel_width);
        } else {
            tile_dirty[i] = true;
        }
        any_dirty = any_dirty || tile_dirty[i];
    }
    if (!any_dirty) {
        return;
    }
    send_tiles(screen, players, player_ids, player_count, tile_dirty);
#else
    (void)screen; (void)frame_buf; (void)players; (void)player_ids;
    (void)player_count;
#endif
}

void map_render_clear(struct map_render_ctx *ctx, struct screen_entry *screen)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    int tile_count = screen_geom_tile_count(&screen->geom);
    for (int i = 0; i < tile_count; i++) {
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (allocation) {
            allocation->renderer.frame_ptr = nullptr;
            allocation->renderer.pattern = MAP_TEST_NONE;
        }
    }
    // The next playback starts from an unknown client state.
    invalidate_sent_tiles(screen);
#else
    (void)screen;
#endif
}

int map_render_set_test_pattern(struct map_render_ctx *ctx,
                                struct screen_entry *screen,
                                enum map_test_pattern pattern,
                                void **players, const char **player_ids,
                                int player_count)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen->tiles_initialized || pattern <= MAP_TEST_NONE ||
        pattern > MAP_TEST_QUADRANTS) {
        return -1;
    }
    int tile_count = screen_geom_tile_count(&screen->geom);
    for (int i = 0; i < tile_count; i++) {
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (allocation) {
            allocation->renderer.frame_ptr = nullptr;
            allocation->renderer.pattern = pattern;
        }
    }
    // The pattern replaces the video content, so the stored copies no
    // longer describe what viewers see.
    invalidate_sent_tiles(screen);
    send_tiles(screen, players, player_ids, player_count, nullptr);
    return 0;
#else
    (void)screen; (void)pattern; (void)players; (void)player_ids;
    (void)player_count;
    return -2;
#endif
}

int map_render_resend(struct map_render_ctx *ctx,
                      const struct screen_entry *screen,
                      void **players, const char **player_ids,
                      int player_count)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen->tiles_initialized) {
        return -1;
    }
    // A resend targets a viewer who has no client-side state yet, so it
    // always transmits every tile regardless of change tracking.
    send_tiles(screen, players, player_ids, player_count, nullptr);
    return 0;
#else
    (void)screen; (void)players; (void)player_ids; (void)player_count;
    return -2;
#endif
}

bool map_render_get_stats(const struct screen_entry *screen, int tile,
                          struct map_renderer_stats *stats)
{
    if (!stats) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen || tile < 0 || tile >= screen_geom_tile_count(&screen->geom)) {
        return false;
    }
    struct renderer_alloc *allocation = screen->tiles[tile].renderer;
    if (!allocation) {
        return false;
    }
    stats->initialize_count = allocation->renderer.initialize_count;
    stats->render_count = allocation->renderer.render_count;
    stats->destroy_count = allocation->renderer.destroy_count;
    stats->last_canvas = allocation->renderer.last_canvas;
    stats->last_map_view = allocation->renderer.last_map_view;
    stats->last_player = allocation->renderer.last_player;
    stats->renderer = &allocation->renderer;
    stats->renderer_vptr = allocation->renderer.vtable;
#if defined(ES_PLATFORM_WINDOWS)
    stats->strong_references = allocation->control.uses;
#else
    stats->strong_references = allocation->control.uses + 1;
#endif
    stats->last_canvas_valid = allocation->renderer.last_canvas_valid;
    stats->last_write_verified = allocation->renderer.last_write_verified;
    stats->last_canvas_pixels = allocation->renderer.last_canvas_pixels;
    stats->last_first_pixel = allocation->renderer.last_first_pixel;
    stats->last_last_pixel = allocation->renderer.last_last_pixel;
    return true;
#else
    (void)screen; (void)tile;
    return false;
#endif
}

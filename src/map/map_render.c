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

struct tile_renderer {
    _Alignas(void *) unsigned char base[ES_MAPRENDERER_SIZE];
    int tile_col;
    int tile_row;
    char screen_name[SCREEN_NAME_MAX];
    const uint8_t *tile_pixels;
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

static_assert(offsetof(struct tile_renderer, tile_col) == ES_MAPRENDERER_SIZE,
              "MapRenderer base size mismatch");

struct ref_count_block {
    _Alignas(void *) unsigned char bytes[ES_REFCOUNT_SIZE];
};

static_assert(sizeof(struct ref_count_block) == ES_REFCOUNT_SIZE,
              "shared_ptr control block size mismatch");

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

static void *g_renderer_vtable[
    ES_MAPRENDERER_VTABLE_PREFIX_SLOTS +
    ES_MAPRENDERER_VTABLE_SLOT_COUNT] = {
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_DTOR] =
        (void *)renderer_dtor,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_IS_ENDSTONE] =
        (void *)renderer_is_endstone,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_INIT] =
        (void *)renderer_initialize,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_RENDER] =
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

static void *g_renderer_vtable[
    ES_MAPRENDERER_VTABLE_PREFIX_SLOTS +
    ES_MAPRENDERER_VTABLE_SLOT_COUNT] = {
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS +
        ES_MAPRENDERER_SLOT_DTOR_COMPLETE] =
            (void *)renderer_complete_dtor,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS +
        ES_MAPRENDERER_SLOT_DTOR_DELETING] =
            (void *)renderer_deleting_dtor,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_IS_ENDSTONE] =
        (void *)renderer_is_endstone,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_INIT] =
        (void *)renderer_initialize,
    [ES_MAPRENDERER_VTABLE_PREFIX_SLOTS + ES_MAPRENDERER_SLOT_RENDER] =
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
    renderer->last_player = player
        ? es_shared_object((char *)player + ES_NOTNULL_PLAYER_OFF_SHARED_PTR)
        : nullptr;
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
    } else if (renderer->tile_pixels) {
        memcpy(destination, renderer->tile_pixels, SURFACE_TILE_BYTES);
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
    } else if (renderer->tile_pixels) {
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
static void *g_control_vtable[
    ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_VTABLE_SLOT_COUNT] = {
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_DESTROY_RESOURCE] =
        (void *)control_destroy_resource,
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_DELETE_THIS] =
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

static void *g_control_vtable[
    ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_VTABLE_SLOT_COUNT] = {
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_DTOR_COMPLETE] =
        (void *)control_complete_dtor,
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_DTOR_DELETING] =
        (void *)control_deleting_dtor,
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_GET_DELETER] =
        (void *)control_get_deleter,
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_DESTROY_RESOURCE] =
        (void *)control_destroy_resource,
    [ES_REFCOUNT_VTABLE_PREFIX_SLOTS + ES_REFCOUNT_SLOT_DELETE_THIS] =
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
    es_store_pointer(allocation->renderer.base, 0,
        g_renderer_vtable + ES_MAPRENDERER_VTABLE_PREFIX_SLOTS);
    allocation->renderer.base[ES_MAPRENDERER_OFF_IS_CONTEXTUAL] = false;
    allocation->renderer.tile_col = col;
    allocation->renderer.tile_row = row;
    snprintf(allocation->renderer.screen_name,
             sizeof(allocation->renderer.screen_name), "%s", screen_name);
    allocation->renderer.pixel_width = pixel_width;
    allocation->renderer.pixel_height = pixel_height;
    es_store_pointer(allocation->control.bytes, 0,
        g_control_vtable + ES_REFCOUNT_VTABLE_PREFIX_SLOTS);
    // Pre-account for the caller's retained owner and the by-value parameter.
    es_counter_store(allocation->control.bytes + ES_REFCOUNT_OFF_USES,
                     ES_REFCOUNT_ONE_OWNER_VALUE + 1);
    es_counter_store(allocation->control.bytes + ES_REFCOUNT_OFF_WEAKS,
                     ES_REFCOUNT_IMPLICIT_WEAK_VALUE);
    return allocation;
}

static struct es_shared_handle renderer_shared(
    struct renderer_alloc *allocation)
{
    struct es_shared_handle shared;
    es_shared_init(&shared, &allocation->renderer, &allocation->control);
    return shared;
}

// tile_dirty selects which tiles to transmit; nullptr sends every tile,
// which is what a new viewer, a test pattern or a forced resend needs.
static void send_tiles(const struct screen_entry *screen,
                       void **players, const char **player_ids,
                       int player_count, const bool *tile_dirty)
{
    if (!screen || !screen->tiles)
        return;
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

    if (screen_entry_materialize_tiles(screen) != SCREEN_OK)
        return MAP_RENDER_ERR_INIT;

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

        struct es_shared_handle parameter = renderer_shared(allocation);
        es_map_view_add_renderer(map_view, &parameter);
        if (ES_C_ABI_NOTNULL_CALLEE_DESTROYS) {
            memset(&parameter, 0, sizeof(parameter));
        }
        else {
            es_shared_release(&parameter);
        }
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

enum map_render_error map_render_restore_screen(struct map_render_ctx *ctx,
                                                struct screen_entry *screen,
                                                void *player)
{
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!ctx || !screen || !player) {
        return MAP_RENDER_ERR_INIT;
    }
    if (screen->tiles_initialized) {
        return MAP_RENDER_OK;
    }
    if (!screen->tiles) {
        return MAP_RENDER_ERR_INIT;
    }

    int tile_count = screen_geom_tile_count(&screen->geom);
    if (tile_count <= 0 || screen->tiles_capacity < (size_t)tile_count) {
        return MAP_RENDER_ERR_INIT;
    }
    for (int i = 0; i < tile_count; i++) {
        if (!screen->tiles[i].map_id_valid) {
            return MAP_RENDER_ERR_INIT;
        }
    }
    return map_render_init_screen(ctx, screen, player);
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
    if (!screen->tiles)
        return;
    for (int i = 0; i < tile_count; i++) {
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (allocation) {
            // Prevent callbacks from observing a session buffer after stop.
            allocation->renderer.tile_pixels = nullptr;
            struct es_shared_handle owner = renderer_shared(allocation);
            if (screen->tiles[i].map_view) {
                es_map_view_remove_renderer(screen->tiles[i].map_view, &owner);
            }
            es_shared_release(&owner);
        }
#endif
        screen->tiles[i].renderer = nullptr;
        screen->tiles[i].valid = 0;
        screen->tiles[i].map_view = nullptr;
    }
    screen->tiles_initialized = 0;
}

#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)

struct map_presenter_context {
    struct screen_entry *screen;
};

static bool presenter_send_tile(void *context,
                                const struct presenter_tile *tile)
{
    struct map_presenter_context *backend = context;
    struct screen_entry *screen = backend->screen;
    int index = screen_geom_tile_index(
        &screen->geom, (int)tile->tile_x, (int)tile->tile_y);
    if (index < 0 || !screen->tiles ||
        (size_t)index >= screen->tiles_capacity)
        return false;
    if (!screen->tiles[index].valid || !screen->tiles[index].map_view)
        return true;
    struct renderer_alloc *allocation = screen->tiles[index].renderer;
    if (!allocation || tile->bytes != SURFACE_TILE_BYTES)
        return false;

    // Surface tile views are borrowed only for synchronous sendMap calls.
    allocation->renderer.pattern = MAP_TEST_NONE;
    allocation->renderer.tile_pixels = tile->pixels;
    for (size_t i = 0; i < tile->viewer_count; i++) {
        if (tile->viewers[i].player)
            es_player_send_map(tile->viewers[i].player,
                               screen->tiles[index].map_view);
    }
    allocation->renderer.tile_pixels = nullptr;
    return true;
}

#endif

enum map_render_error map_render_submit_frame(
    struct screen_entry *screen, const uint8_t *frame_buf,
    size_t frame_bytes, enum surface_pixel_format format)
{
    if (!screen || !screen->surface || !frame_buf)
        return MAP_RENDER_ERR_SURFACE;
    struct surface_frame *frame = nullptr;
    if (surface_frame_begin(screen->surface, &frame) != SURFACE_OK)
        return MAP_RENDER_ERR_SURFACE;
    enum surface_error error = surface_frame_update_region(
        frame, 0, 0, (uint32_t)screen_geom_pixel_width(&screen->geom),
        (uint32_t)screen_geom_pixel_height(&screen->geom), frame_buf,
        frame_bytes,
        (size_t)screen_geom_pixel_width(&screen->geom) * 4, format);
    if (error == SURFACE_OK)
        error = surface_frame_commit(frame);
    if (error != SURFACE_OK) {
        surface_frame_abort(frame);
        return MAP_RENDER_ERR_SURFACE;
    }
    return MAP_RENDER_OK;
}

enum map_render_error map_render_present(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewers, size_t viewer_count,
    size_t budget, struct presenter_stats *stats)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen || !screen->surface || !screen->tiles_initialized || !stats)
        return MAP_RENDER_ERR_PRESENTER;
    struct map_presenter_context backend_context = {.screen = screen};
    struct presenter presenter;
    struct presenter_backend backend = {
        .context = &backend_context,
        .send = presenter_send_tile,
    };
    if (presenter_init(&presenter, screen->surface, &screen->geom, 16.0,
                       backend) != PRESENTER_OK ||
        presenter_set_viewers(&presenter, viewers, viewer_count) !=
            PRESENTER_OK ||
        presenter_tick(&presenter, budget, stats) != PRESENTER_OK)
        return MAP_RENDER_ERR_PRESENTER;
    return MAP_RENDER_OK;
#else
    (void)screen; (void)viewers; (void)viewer_count; (void)budget;
    (void)stats;
    return MAP_RENDER_ERR_UNSUPPORTED;
#endif
}

void map_render_clear(struct map_render_ctx *ctx, struct screen_entry *screen)
{
    (void)ctx;
    if (!screen)
        return;
    surface_reset(screen->surface);
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen->tiles) {
        return;
    }
    int tile_count = screen_geom_tile_count(&screen->geom);
    for (int i = 0; i < tile_count; i++) {
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (allocation) {
            allocation->renderer.tile_pixels = nullptr;
            allocation->renderer.pattern = MAP_TEST_NONE;
        }
    }
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
            allocation->renderer.tile_pixels = nullptr;
            allocation->renderer.pattern = pattern;
        }
    }
    surface_reset(screen->surface);
    send_tiles(screen, players, player_ids, player_count, nullptr);
    return 0;
#else
    (void)screen; (void)pattern; (void)players; (void)player_ids;
    (void)player_count;
    return -2;
#endif
}

int map_render_resend(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewer,
    struct presenter_resident_cursor *cursor, size_t budget,
    bool *done, struct presenter_stats *stats)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen || !screen->surface || !screen->tiles_initialized ||
        !viewer || !cursor || !done || !stats) {
        return -1;
    }
    struct map_presenter_context backend_context = {.screen = screen};
    struct presenter presenter;
    struct presenter_backend backend = {
        .context = &backend_context,
        .send = presenter_send_tile,
    };
    if (presenter_init(&presenter, screen->surface, &screen->geom, 16.0,
                       backend) != PRESENTER_OK ||
        presenter_resend(&presenter, viewer, cursor, budget, done, stats) !=
            PRESENTER_OK)
        return -1;
    return 0;
#else
    (void)screen; (void)viewer; (void)cursor; (void)budget; (void)done;
    (void)stats;
    return -2;
#endif
}

enum map_render_error map_render_present_stream(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewers, size_t viewer_count,
    uint64_t tile_count, uint64_t *cursor, size_t budget,
    bool (*read_tile)(void *context, uint64_t tile_index, uint32_t tile_x,
                      uint32_t tile_y, struct presenter_stream_tile *out),
    void *read_context, bool *done, struct presenter_stats *stats)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen || !screen->surface || !screen->tiles_initialized ||
        !stats)
        return MAP_RENDER_ERR_PRESENTER;
    struct map_presenter_context backend_context = {.screen = screen};
    struct presenter presenter;
    struct presenter_backend backend = {
        .context = &backend_context,
        .send = presenter_send_tile,
    };
    if (presenter_init(&presenter, screen->surface, &screen->geom, 16.0,
                       backend) != PRESENTER_OK ||
        presenter_set_viewers(&presenter, viewers, viewer_count) !=
            PRESENTER_OK ||
        presenter_stream(&presenter, tile_count, cursor, budget, read_tile,
                         read_context, done, stats) != PRESENTER_OK)
        return MAP_RENDER_ERR_PRESENTER;
    return MAP_RENDER_OK;
#else
    (void)screen; (void)viewers; (void)viewer_count; (void)tile_count;
    (void)cursor; (void)budget; (void)read_tile; (void)read_context;
    (void)done; (void)stats;
    return MAP_RENDER_ERR_UNSUPPORTED;
#endif
}

enum map_render_error map_render_resend_stream(
    struct map_render_ctx *ctx, struct screen_entry *screen,
    const struct presenter_viewer *viewer, uint64_t tile_count,
    uint64_t *cursor, size_t budget,
    bool (*read_tile)(void *context, uint64_t tile_index, uint32_t tile_x,
                      uint32_t tile_y, struct presenter_stream_tile *out),
    void *read_context, bool *done, struct presenter_stats *stats)
{
    return map_render_present_stream(ctx, screen, viewer, 1, tile_count,
                                     cursor, budget, read_tile, read_context,
                                     done, stats);
}

int map_render_hide_viewer(struct map_render_ctx *ctx,
                           const struct screen_entry *screen,
                           void *player, const char *player_id)
{
    (void)ctx;
#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)
    if (!screen || !screen->tiles_initialized || !player)
        return -1;

    int tile_count = screen_geom_tile_count(&screen->geom);
    static const uint8_t black_tile[SURFACE_TILE_BYTES] = {0};
    for (int i = 0; i < tile_count; i++) {
        struct renderer_alloc *allocation = screen->tiles[i].renderer;
        if (!allocation || !screen->tiles[i].valid ||
            !screen->tiles[i].map_view)
            continue;
        enum map_test_pattern pattern = allocation->renderer.pattern;
        allocation->renderer.tile_pixels = black_tile;
        allocation->renderer.pattern = MAP_TEST_NONE;
        es_player_send_map(player, screen->tiles[i].map_view);
        allocation->renderer.tile_pixels = nullptr;
        allocation->renderer.pattern = pattern;
    }
    (void)player_id;
    return 0;
#else
    (void)screen;
    (void)player;
    (void)player_id;
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
    stats->renderer_vptr = es_pointer_at(allocation->renderer.base, 0);
    int64_t stored = es_counter_load(
        allocation->control.bytes + ES_REFCOUNT_OFF_USES);
    stats->strong_references =
        (int)(stored - ES_REFCOUNT_ONE_OWNER_VALUE + 1);
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

#include "mediaplayer/api_provider.h"

#include "mediaplayer/map/map_render.h"
#include "mediaplayer/screen/screen_geometry.h"
#include "mediaplayer/screen/screen_registry.h"
#include "mediaplayer/screen/surface.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MP_PROVIDER_MAX_HANDLES 64u
#define MP_PROVIDER_CREATE_PREFIX \
    (offsetof(mp_screen_create_info, backend) + sizeof(mp_backend))
#define MP_PROVIDER_INFO_PREFIX \
    (offsetof(mp_screen_info, surface_generation) + sizeof(uint64_t))
#define MP_PROVIDER_STATS_PREFIX \
    (offsetof(mp_stats, allocated_staging_pixel_bytes) + sizeof(uint64_t))
#define MP_PROVIDER_CAPS_PREFIX \
    (offsetof(mp_capabilities, backend_bits) + sizeof(uint32_t))
#define MP_PROVIDER_FRAME_ID_INVALID UINT64_C(0)

struct provider_screen_slot {
    mp_screen_handle handle;
    uint64_t runtime_id;
    int active;
};

struct provider_frame_slot {
    mp_frame_handle handle;
    mp_screen_handle screen_handle;
    uint64_t runtime_id;
    struct surface *surface;
    struct surface_frame *frame;
    int active;
};

static struct video_ctx *g_provider_ctx;
static uint64_t g_provider_next_instance = 1;
static uint64_t g_provider_next_object = 1;
static uint64_t g_provider_instance;
static struct provider_screen_slot g_provider_screens[MP_PROVIDER_MAX_HANDLES];
static struct provider_frame_slot g_provider_frames[MP_PROVIDER_MAX_HANDLES];

static const char *MP_CALL provider_result_string(mp_result result)
{
    switch (result) {
    case MP_OK: return "ok";
    case MP_ERR_UNAVAILABLE: return "unavailable";
    case MP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MP_ERR_INVALID_HANDLE: return "invalid handle";
    case MP_ERR_NOT_FOUND: return "not found";
    case MP_ERR_ALREADY_EXISTS: return "already exists";
    case MP_ERR_CAPACITY: return "capacity exceeded";
    case MP_ERR_NO_MEMORY: return "out of memory";
    case MP_ERR_UNSUPPORTED: return "unsupported";
    case MP_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    case MP_ERR_BUSY: return "busy";
    case MP_ERR_IO: return "I/O error";
    case MP_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

static int provider_active(void)
{
    return g_provider_ctx != nullptr && g_provider_ctx->active;
}

static mp_result map_screen_error(enum screen_error error)
{
    switch (error) {
    case SCREEN_OK: return MP_OK;
    case SCREEN_ERR_NOT_FOUND: return MP_ERR_NOT_FOUND;
    case SCREEN_ERR_NAME_EXISTS: return MP_ERR_ALREADY_EXISTS;
    case SCREEN_ERR_NAME_INVALID: return MP_ERR_INVALID_ARGUMENT;
    case SCREEN_ERR_FULL: return MP_ERR_CAPACITY;
    case SCREEN_ERR_DIMENSION_INVALID: return MP_ERR_INVALID_ARGUMENT;
    case SCREEN_ERR_MATERIALIZATION_LIMIT: return MP_ERR_CAPACITY;
    case SCREEN_ERR_NO_MEMORY: return MP_ERR_NO_MEMORY;
    case SCREEN_ERR_RUNTIME_ID_EXHAUSTED: return MP_ERR_CAPACITY;
    default: return MP_ERR_INTERNAL;
    }
}

static mp_result map_surface_error(enum surface_error error)
{
    switch (error) {
    case SURFACE_OK: return MP_OK;
    case SURFACE_ERR_NO_MEMORY: return MP_ERR_NO_MEMORY;
    case SURFACE_ERR_CAPACITY: return MP_ERR_CAPACITY;
    case SURFACE_ERR_FRAME_ACTIVE: return MP_ERR_BUSY;
    case SURFACE_ERR_NO_FRAME: return MP_ERR_INVALID_HANDLE;
    case SURFACE_ERR_ARGUMENT:
    case SURFACE_ERR_DIMENSION:
    case SURFACE_ERR_BUFFER:
    case SURFACE_ERR_STRIDE:
    case SURFACE_ERR_FORMAT:
    case SURFACE_ERR_RECTANGLE:
    case SURFACE_ERR_EMPTY:
    case SURFACE_ERR_INDEX:
    case SURFACE_ERR_OVERFLOW:
    default: return MP_ERR_INVALID_ARGUMENT;
    }
}

static void invalidate_screen(mp_screen_handle *out)
{
    if (out) *out = MP_INVALID_SCREEN_HANDLE;
}

static void invalidate_frame(mp_frame_handle *out)
{
    if (out) *out = MP_INVALID_FRAME_HANDLE;
}

static uint64_t allocate_object_id(void)
{
    uint64_t value = g_provider_next_object++;
    if (value == 0) value = g_provider_next_object++;
    if (g_provider_next_object == 0) g_provider_next_object = 1;
    return value;
}

static struct provider_screen_slot *find_screen_slot(mp_screen_handle handle)
{
    if (handle == MP_INVALID_SCREEN_HANDLE) return nullptr;
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        if (g_provider_screens[i].active &&
            g_provider_screens[i].handle == handle)
            return &g_provider_screens[i];
    }
    return nullptr;
}

static struct screen_entry *find_live_screen(mp_screen_handle handle,
                                             struct provider_screen_slot **slot_out)
{
    struct provider_screen_slot *slot = find_screen_slot(handle);
    if (!slot || !provider_active()) return nullptr;
    if (slot_out) *slot_out = slot;
    for (int i = 0; i < g_provider_ctx->registry.count; i++) {
        struct screen_entry *screen = g_provider_ctx->registry.screens[i];
        if (screen && screen->runtime_id == slot->runtime_id)
            return screen;
    }
    // A command-side delete may compact and free the registry entry without
    // notifying the provider.  Retire the opaque slot without touching the
    // stale entry or any stale frame pointer.
    memset(slot, 0, sizeof(*slot));
    return nullptr;
}

static struct provider_screen_slot *find_screen_for_runtime(uint64_t runtime_id)
{
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        if (g_provider_screens[i].active &&
            g_provider_screens[i].runtime_id == runtime_id)
            return &g_provider_screens[i];
    }
    return nullptr;
}

static struct provider_screen_slot *allocate_screen_slot(void)
{
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        if (!g_provider_screens[i].active)
            return &g_provider_screens[i];
    }
    return nullptr;
}

static mp_result make_screen_handle(struct screen_entry *screen,
                                    mp_screen_handle *out)
{
    invalidate_screen(out);
    if (!screen || !out) return MP_ERR_INVALID_ARGUMENT;
    struct provider_screen_slot *existing =
        find_screen_for_runtime(screen->runtime_id);
    if (existing) {
        *out = existing->handle;
        return MP_OK;
    }
    struct provider_screen_slot *slot = allocate_screen_slot();
    if (!slot) return MP_ERR_CAPACITY;
    memset(slot, 0, sizeof(*slot));
    slot->handle = allocate_object_id();
    slot->runtime_id = screen->runtime_id;
    slot->active = 1;
    *out = slot->handle;
    return MP_OK;
}

static mp_backend screen_backend(const struct screen_entry *screen)
{
    if (!screen) return MP_BACKEND_LOGICAL;
    if (screen->plugin_managed) return MP_BACKEND_PLUGIN_MANAGED;
    if (strcmp(screen->geom.dimension, "logical") == 0)
        return MP_BACKEND_LOGICAL;
    return MP_BACKEND_WORLD;
}

static void stop_screen_state(struct screen_entry *screen)
{
    if (!screen || !g_provider_ctx) return;
    map_render_clear(&g_provider_ctx->render, screen);
    screen_audio_stop(&g_provider_ctx->audio, screen->runtime_id);
    video_engine_release(&g_provider_ctx->engine, screen->runtime_id);
    mps_source_release(&g_provider_ctx->image_engine, screen->runtime_id);
    memset(&screen->playback, 0, sizeof(screen->playback));
    screen->playback.state = SCREEN_PLAYBACK_STOPPED;
    screen->playing = 0;
}

static void retire_frames_for_screen(struct screen_entry *screen)
{
    if (!screen) return;
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        struct provider_frame_slot *frame = &g_provider_frames[i];
        if (frame->active && frame->runtime_id == screen->runtime_id) {
            // The screen and Surface are still live at every call site.
            surface_frame_abort(frame->frame);
            memset(frame, 0, sizeof(*frame));
        }
    }
}

static mp_result provider_save_registry(const struct screen_registry *registry)
{
    if (!g_provider_ctx || g_provider_ctx->save_path[0] == '\0')
        return MP_OK;
    return screen_persistence_save(registry, g_provider_ctx->save_path) == 0
               ? MP_OK
               : MP_ERR_IO;
}

static void prepare_external_surface(struct screen_entry *screen)
{
    if (!screen || !g_provider_ctx) return;
    if (screen->playing ||
        video_engine_find(&g_provider_ctx->engine, screen->runtime_id) ||
        mps_source_find(&g_provider_ctx->image_engine, screen->runtime_id) ||
        screen_audio_find(&g_provider_ctx->audio, screen->runtime_id)) {
        retire_frames_for_screen(screen);
        stop_screen_state(screen);
    }
}

static int output_size_valid(uint32_t size, size_t required)
{
    return size >= sizeof(uint32_t) * 2 && size >= required;
}

static void copy_info_out(mp_screen_info *out, const mp_screen_info *value)
{
    size_t n = out->struct_size < sizeof(*value) ? out->struct_size : sizeof(*value);
    memcpy(out, value, n);
}

static void copy_stats_out(mp_stats *out, const mp_stats *value)
{
    size_t n = out->struct_size < sizeof(*value) ? out->struct_size : sizeof(*value);
    memcpy(out, value, n);
}

static void copy_caps_out(mp_capabilities *out, const mp_capabilities *value)
{
    size_t n = out->struct_size < sizeof(*value) ? out->struct_size : sizeof(*value);
    memcpy(out, value, n);
}

static mp_result MP_CALL provider_capabilities_get(mp_capabilities *out)
{
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!out || !output_size_valid(out->struct_size, MP_PROVIDER_CAPS_PREFIX) ||
        out->flags != 0)
        return MP_ERR_INVALID_ARGUMENT;
    mp_capabilities value = {
        .struct_size = out->struct_size,
        .flags = 0,
        .abi_version = MP_API_V1,
        .max_width_tiles = SCREEN_LOGICAL_MAX_WIDTH,
        .max_height_tiles = SCREEN_LOGICAL_MAX_HEIGHT,
        .tile_size = SURFACE_TILE_SIZE,
        .max_screens = SCREEN_REGISTRY_MAX,
        .max_resident_tiles = SURFACE_MAX_RESIDENT_TILES,
        .max_pending_tiles = 256,
        .pixel_format_bits = MP_PIXEL_FORMAT_BIT(MP_PIXEL_FORMAT_ABGR) |
            MP_PIXEL_FORMAT_BIT(MP_PIXEL_FORMAT_BGRA) |
            MP_PIXEL_FORMAT_BIT(MP_PIXEL_FORMAT_RGBA),
        .backend_bits = MP_BACKEND_BIT(MP_BACKEND_LOGICAL) |
            MP_BACKEND_BIT(MP_BACKEND_PLUGIN_MANAGED) |
            MP_BACKEND_BIT(MP_BACKEND_WORLD),
    };
    copy_caps_out(out, &value);
    return MP_OK;
}

static mp_result MP_CALL provider_screen_create(
    const mp_screen_create_info *info, mp_screen_handle *out_handle)
{
    invalidate_screen(out_handle);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!info || !out_handle || info->struct_size < MP_PROVIDER_CREATE_PREFIX ||
        info->flags != 0 || !info->name ||
        info->backend != MP_BACKEND_LOGICAL ||
        info->width_tiles < 1 || info->width_tiles > SCREEN_LOGICAL_MAX_WIDTH ||
        info->height_tiles < 1 || info->height_tiles > SCREEN_LOGICAL_MAX_HEIGHT ||
        !screen_name_valid(info->name))
        return MP_ERR_INVALID_ARGUMENT;
    if (g_provider_ctx->registry.count >= SCREEN_REGISTRY_MAX ||
        !allocate_screen_slot())
        return MP_ERR_CAPACITY;

    struct screen_geom geom = {0};
    geom.corner1.x = 0;
    geom.corner1.y = (int)info->height_tiles - 1;
    geom.corner1.z = 0;
    geom.corner2.x = (int)info->width_tiles - 1;
    geom.corner2.y = 0;
    geom.corner2.z = 0;
    geom.facing = SCREEN_FACE_SOUTH;
    geom.width = (int)info->width_tiles;
    geom.height = (int)info->height_tiles;
    memcpy(geom.dimension, "logical", sizeof("logical"));

    int index = -1;
    enum screen_error error = screen_registry_create(
        &g_provider_ctx->registry, info->name, "", &geom, &index);
    if (error != SCREEN_OK) return map_screen_error(error);
    mp_result result = make_screen_handle(g_provider_ctx->registry.screens[index],
                                          out_handle);
    if (result != MP_OK) {
        screen_registry_delete(&g_provider_ctx->registry, info->name);
        return result;
    }
    result = provider_save_registry(&g_provider_ctx->registry);
    if (result != MP_OK) {
        struct provider_screen_slot *slot = find_screen_slot(*out_handle);
        if (slot) memset(slot, 0, sizeof(*slot));
        screen_registry_delete(&g_provider_ctx->registry, info->name);
        invalidate_screen(out_handle);
        return result;
    }
    return result;
}

static mp_result MP_CALL provider_screen_delete(mp_screen_handle handle)
{
    struct provider_screen_slot *slot = nullptr;
    struct screen_entry *screen = find_live_screen(handle, &slot);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen || !slot) return MP_ERR_INVALID_HANDLE;

    struct screen_registry staged = g_provider_ctx->registry;
    int index = screen_registry_find(&staged, screen->name);
    if (index < 0) return MP_ERR_INVALID_HANDLE;
    for (int i = index; i < staged.count - 1; i++)
        staged.screens[i] = staged.screens[i + 1];
    staged.count--;
    staged.screens[staged.count] = nullptr;
    mp_result result = provider_save_registry(&staged);
    if (result != MP_OK) return result;

    retire_frames_for_screen(screen);
    stop_screen_state(screen);
    if (screen->tiles_initialized)
        map_render_destroy_screen(&g_provider_ctx->render, screen);
    result = map_screen_error(screen_registry_delete(
        &g_provider_ctx->registry, screen->name));
    memset(slot, 0, sizeof(*slot));
    return result;
}

static mp_result MP_CALL provider_screen_find(const char *name,
                                              mp_screen_handle *out_handle)
{
    invalidate_screen(out_handle);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!name || !out_handle || !screen_name_valid(name))
        return MP_ERR_INVALID_ARGUMENT;
    int index = screen_registry_find(&g_provider_ctx->registry, name);
    if (index < 0) return MP_ERR_NOT_FOUND;
    return make_screen_handle(g_provider_ctx->registry.screens[index], out_handle);
}

static mp_result MP_CALL provider_screen_list(mp_screen_handle *handles,
                                              uint32_t capacity,
                                              uint32_t *out_count)
{
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!out_count) return MP_ERR_INVALID_ARGUMENT;
    uint32_t required = (uint32_t)g_provider_ctx->registry.count;
    if (!handles && capacity != 0) {
        *out_count = required;
        return MP_ERR_BUFFER_TOO_SMALL;
    }
    if (capacity == 0) {
        *out_count = required;
        return MP_OK;
    }
    uint32_t written = capacity < required ? capacity : required;
    for (uint32_t i = 0; i < written; i++) {
        mp_screen_handle handle = MP_INVALID_SCREEN_HANDLE;
        mp_result result = make_screen_handle(g_provider_ctx->registry.screens[i],
                                              &handle);
        handles[i] = handle;
        if (result != MP_OK) {
            *out_count = required;
            return result;
        }
    }
    *out_count = required;
    return capacity < required ? MP_ERR_BUFFER_TOO_SMALL : MP_OK;
}

static mp_result MP_CALL provider_screen_get_info(mp_screen_handle handle,
                                                  mp_screen_info *out)
{
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen) return MP_ERR_INVALID_HANDLE;
    if (!out || !output_size_valid(out->struct_size, MP_PROVIDER_INFO_PREFIX) ||
        out->flags != 0)
        return MP_ERR_INVALID_ARGUMENT;
    struct surface_stats stats = {0};
    if (surface_get_stats(screen->surface, &stats) != SURFACE_OK)
        return MP_ERR_INTERNAL;
    mp_screen_info value = {0};
    value.struct_size = out->struct_size;
    value.flags = 0;
    memcpy(value.name, screen->name, sizeof(value.name));
    value.handle = handle;
    value.instance_id = g_provider_instance;
    value.width_tiles = (uint32_t)screen->geom.width;
    value.height_tiles = (uint32_t)screen->geom.height;
    value.pixel_width = stats.pixel_width;
    value.pixel_height = stats.pixel_height;
    value.backend = screen_backend(screen);
    value.playing = screen->playing ? 1u : 0u;
    value.surface_generation = stats.generation;
    copy_info_out(out, &value);
    return MP_OK;
}

static mp_result MP_CALL provider_screen_rename(mp_screen_handle handle,
                                                const char *name)
{
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen || !name) return screen ? MP_ERR_INVALID_ARGUMENT : MP_ERR_INVALID_HANDLE;
    if (!screen_name_valid(name)) return MP_ERR_INVALID_ARGUMENT;
    int existing = screen_registry_find(&g_provider_ctx->registry, name);
    if (existing >= 0 && g_provider_ctx->registry.screens[existing] != screen)
        return MP_ERR_ALREADY_EXISTS;
    size_t length = strlen(name);
    if (length >= SCREEN_NAME_MAX) return MP_ERR_INVALID_ARGUMENT;
    char old_name[SCREEN_NAME_MAX];
    memcpy(old_name, screen->name, sizeof(old_name));
    memset(screen->name, 0, sizeof(screen->name));
    memcpy(screen->name, name, length);
    mp_result result = provider_save_registry(&g_provider_ctx->registry);
    if (result != MP_OK)
        memcpy(screen->name, old_name, sizeof(old_name));
    return result;
}

static mp_result MP_CALL provider_screen_clear(mp_screen_handle handle)
{
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen) return MP_ERR_INVALID_HANDLE;
    retire_frames_for_screen(screen);
    stop_screen_state(screen);
    return MP_OK;
}

static mp_result MP_CALL provider_screen_get_stats(mp_screen_handle handle,
                                                   mp_stats *out)
{
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen) return MP_ERR_INVALID_HANDLE;
    if (!out || !output_size_valid(out->struct_size, MP_PROVIDER_STATS_PREFIX) ||
        out->flags != 0)
        return MP_ERR_INVALID_ARGUMENT;
    struct surface_stats stats = {0};
    enum surface_error error = surface_get_stats(screen->surface, &stats);
    if (error != SURFACE_OK) return map_surface_error(error);
    mp_stats value = {
        .struct_size = out->struct_size,
        .flags = 0,
        .width_tiles = stats.width_tiles,
        .height_tiles = stats.height_tiles,
        .pixel_width = stats.pixel_width,
        .pixel_height = stats.pixel_height,
        .generation = stats.generation,
        .resident_tiles = stats.resident_tiles,
        .pending_tiles = stats.pending_tiles,
        .max_resident_tiles = stats.max_resident_tiles,
        .max_pending_tiles = stats.max_pending_tiles,
        .dropped_tiles = stats.dropped_tiles,
        .allocated_pixel_bytes = stats.allocated_pixel_bytes,
        .staged_tiles = stats.staged_tiles,
        .allocated_staging_pixel_bytes = stats.allocated_staging_pixel_bytes,
    };
    copy_stats_out(out, &value);
    return MP_OK;
}

static mp_result validate_buffer_sizes(uint64_t bytes, uint64_t stride,
                                       size_t *out_bytes,
                                       size_t *out_stride)
{
    if (bytes > SIZE_MAX || stride > SIZE_MAX) return MP_ERR_INVALID_ARGUMENT;
    *out_bytes = (size_t)bytes;
    *out_stride = (size_t)stride;
    return MP_OK;
}

static mp_result MP_CALL provider_screen_update_tile(
    mp_screen_handle handle, uint32_t x, uint32_t y, const void *pixels,
    uint64_t bytes, uint64_t stride, mp_pixel_format format)
{
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen) return MP_ERR_INVALID_HANDLE;
    prepare_external_surface(screen);
    size_t byte_count, row_stride;
    mp_result result = validate_buffer_sizes(bytes, stride, &byte_count, &row_stride);
    if (result != MP_OK) return result;
    struct surface_frame *frame = nullptr;
    enum surface_error error = surface_frame_begin(screen->surface, &frame);
    if (error != SURFACE_OK) return map_surface_error(error);
    error = surface_frame_update_tile(frame, x, y, pixels, byte_count,
                                      row_stride, (enum surface_pixel_format)format);
    if (error != SURFACE_OK) {
        surface_frame_abort(frame);
        return map_surface_error(error);
    }
    error = surface_frame_commit(frame);
    if (error != SURFACE_OK) surface_frame_abort(frame);
    return map_surface_error(error);
}

static mp_result MP_CALL provider_screen_update_region(
    mp_screen_handle handle, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const void *pixels, uint64_t bytes, uint64_t stride,
    mp_pixel_format format)
{
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen) return MP_ERR_INVALID_HANDLE;
    prepare_external_surface(screen);
    size_t byte_count, row_stride;
    mp_result result = validate_buffer_sizes(bytes, stride, &byte_count, &row_stride);
    if (result != MP_OK) return result;
    struct surface_frame *frame = nullptr;
    enum surface_error error = surface_frame_begin(screen->surface, &frame);
    if (error != SURFACE_OK) return map_surface_error(error);
    error = surface_frame_update_region(frame, x, y, width, height, pixels,
                                        byte_count, row_stride,
                                        (enum surface_pixel_format)format);
    if (error != SURFACE_OK) {
        surface_frame_abort(frame);
        return map_surface_error(error);
    }
    error = surface_frame_commit(frame);
    if (error != SURFACE_OK) surface_frame_abort(frame);
    return map_surface_error(error);
}

static struct provider_frame_slot *find_frame_slot(mp_frame_handle handle)
{
    if (handle == MP_INVALID_FRAME_HANDLE) return nullptr;
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        if (g_provider_frames[i].active &&
            g_provider_frames[i].handle == handle)
            return &g_provider_frames[i];
    }
    return nullptr;
}

int mp_api_provider_screen_has_active_frame(uint64_t runtime_id)
{
    if (runtime_id == 0) return 0;
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        if (g_provider_frames[i].active &&
            g_provider_frames[i].runtime_id == runtime_id)
            return 1;
    }
    return 0;
}

static struct provider_frame_slot *allocate_frame_slot(void)
{
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++)
        if (!g_provider_frames[i].active) return &g_provider_frames[i];
    return nullptr;
}

static struct screen_entry *find_frame_screen(struct provider_frame_slot *slot)
{
    if (!slot || !slot->active) return nullptr;
    struct screen_entry *screen = find_live_screen(slot->screen_handle, nullptr);
    if (!screen || screen->runtime_id != slot->runtime_id ||
        screen->surface != slot->surface)
        return nullptr;
    return screen;
}

static mp_result MP_CALL provider_frame_begin(mp_screen_handle handle,
                                              mp_frame_handle *out_frame)
{
    invalidate_frame(out_frame);
    struct screen_entry *screen = find_live_screen(handle, nullptr);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!screen || !out_frame) return screen ? MP_ERR_INVALID_ARGUMENT : MP_ERR_INVALID_HANDLE;
    prepare_external_surface(screen);
    struct provider_frame_slot *slot = allocate_frame_slot();
    if (!slot) return MP_ERR_CAPACITY;
    struct surface_frame *frame = nullptr;
    enum surface_error error = surface_frame_begin(screen->surface, &frame);
    if (error != SURFACE_OK) return map_surface_error(error);
    memset(slot, 0, sizeof(*slot));
    slot->handle = allocate_object_id();
    slot->screen_handle = handle;
    slot->runtime_id = screen->runtime_id;
    slot->surface = screen->surface;
    slot->frame = frame;
    slot->active = 1;
    *out_frame = slot->handle;
    return MP_OK;
}

static mp_result MP_CALL provider_frame_update_tile(
    mp_frame_handle handle, uint32_t x, uint32_t y, const void *pixels,
    uint64_t bytes, uint64_t stride, mp_pixel_format format)
{
    struct provider_frame_slot *slot = find_frame_slot(handle);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    struct screen_entry *screen = find_frame_screen(slot);
    if (!slot || !screen) {
        if (slot) memset(slot, 0, sizeof(*slot));
        return MP_ERR_INVALID_HANDLE;
    }
    size_t byte_count, row_stride;
    mp_result result = validate_buffer_sizes(bytes, stride, &byte_count, &row_stride);
    if (result != MP_OK) return result;
    return map_surface_error(surface_frame_update_tile(
        slot->frame, x, y, pixels, byte_count, row_stride,
        (enum surface_pixel_format)format));
}

static mp_result MP_CALL provider_frame_update_region(
    mp_frame_handle handle, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const void *pixels, uint64_t bytes, uint64_t stride,
    mp_pixel_format format)
{
    struct provider_frame_slot *slot = find_frame_slot(handle);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    struct screen_entry *screen = find_frame_screen(slot);
    if (!slot || !screen) {
        if (slot) memset(slot, 0, sizeof(*slot));
        return MP_ERR_INVALID_HANDLE;
    }
    size_t byte_count, row_stride;
    mp_result result = validate_buffer_sizes(bytes, stride, &byte_count, &row_stride);
    if (result != MP_OK) return result;
    return map_surface_error(surface_frame_update_region(
        slot->frame, x, y, width, height, pixels, byte_count, row_stride,
        (enum surface_pixel_format)format));
}

static mp_result MP_CALL provider_frame_commit(mp_frame_handle handle)
{
    struct provider_frame_slot *slot = find_frame_slot(handle);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    struct screen_entry *screen = find_frame_screen(slot);
    if (!slot || !screen) {
        if (slot) memset(slot, 0, sizeof(*slot));
        return MP_ERR_INVALID_HANDLE;
    }
    enum surface_error error = surface_frame_commit(slot->frame);
    if (error == SURFACE_OK) memset(slot, 0, sizeof(*slot));
    // A failed Surface commit intentionally leaves the frame active so the
    // caller can abort it.
    return map_surface_error(error);
}

static mp_result MP_CALL provider_frame_abort(mp_frame_handle handle)
{
    struct provider_frame_slot *slot = find_frame_slot(handle);
    if (!provider_active()) return MP_ERR_UNAVAILABLE;
    if (!slot) return MP_ERR_INVALID_HANDLE;
    struct screen_entry *screen = find_frame_screen(slot);
    if (!screen) {
        memset(slot, 0, sizeof(*slot));
        return MP_ERR_INVALID_HANDLE;
    }
    surface_frame_abort(slot->frame);
    memset(slot, 0, sizeof(*slot));
    return MP_OK;
}

static mp_api_v1 g_provider_api = {
    .abi_version = MP_API_V1,
    .struct_size = sizeof(mp_api_v1),
    .instance_id = 0,
    .result_string = provider_result_string,
    .capabilities_get = provider_capabilities_get,
    .screen_create = provider_screen_create,
    .screen_delete = provider_screen_delete,
    .screen_find = provider_screen_find,
    .screen_list = provider_screen_list,
    .screen_get_info = provider_screen_get_info,
    .screen_rename = provider_screen_rename,
    .screen_clear = provider_screen_clear,
    .screen_get_stats = provider_screen_get_stats,
    .screen_update_tile = provider_screen_update_tile,
    .screen_update_region = provider_screen_update_region,
    .frame_begin = provider_frame_begin,
    .frame_update_tile = provider_frame_update_tile,
    .frame_update_region = provider_frame_update_region,
    .frame_commit = provider_frame_commit,
    .frame_abort = provider_frame_abort,
};

MP_EXPORT const mp_api_v1 *MP_CALL endstone_mediaplayer_get_api(uint32_t version)
{
    return version == MP_API_V1 ? &g_provider_api : nullptr;
}

void mp_api_provider_activate(struct video_ctx *ctx)
{
    if (!ctx) return;
    g_provider_ctx = ctx;
    g_provider_instance = g_provider_next_instance++;
    if (g_provider_instance == 0) g_provider_instance = g_provider_next_instance++;
    if (g_provider_next_instance == 0) g_provider_next_instance = 1;
    g_provider_api.instance_id = g_provider_instance;
    memset(g_provider_screens, 0, sizeof(g_provider_screens));
    memset(g_provider_frames, 0, sizeof(g_provider_frames));
}

void mp_api_provider_deactivate(struct video_ctx *ctx)
{
    if (!ctx || g_provider_ctx != ctx) return;
    for (size_t i = 0; i < MP_PROVIDER_MAX_HANDLES; i++) {
        struct provider_frame_slot *frame = &g_provider_frames[i];
        if (frame->active) {
            // A command-side delete may already have destroyed this Surface.
            // Abort only while the runtime identity and Surface pointer still
            // match a live registry entry; never dereference a stale frame.
            int live = 0;
            for (int s = 0; s < ctx->registry.count; s++) {
                struct screen_entry *screen = ctx->registry.screens[s];
                if (screen && screen->runtime_id == frame->runtime_id &&
                    screen->surface == frame->surface) {
                    live = 1;
                    break;
                }
            }
            if (live) surface_frame_abort(frame->frame);
            memset(frame, 0, sizeof(*frame));
        }
    }
    memset(g_provider_screens, 0, sizeof(g_provider_screens));
    g_provider_ctx = nullptr;
    g_provider_api.instance_id = 0;
}

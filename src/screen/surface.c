#include "mediaplayer/screen/surface.h"

#include "miniz.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct surface_tile {
    uint32_t x;
    uint32_t y;
    uint8_t *pixels;
    uint64_t generation;
    uint32_t content_hash;
    bool pending;
};

struct surface_dirty_entry {
    size_t resident_index;
    uint64_t generation;
};

struct surface_stage_tile {
    uint32_t x;
    uint32_t y;
    uint8_t *pixels;
};

struct surface {
    uint32_t width_tiles;
    uint32_t height_tiles;
    uint32_t pixel_width;
    uint32_t pixel_height;
    size_t max_resident_tiles;
    size_t max_pending_tiles;
    bool latest_wins;

    uint64_t generation;
    uint64_t dropped_tiles;
    uint32_t zero_hash;

    struct surface_tile *residents;
    size_t resident_count;
    size_t resident_capacity;

    struct surface_dirty_entry *dirty;
    size_t dirty_count;
    size_t dirty_capacity;

    struct surface_frame *active_frame;
};

struct surface_frame {
    struct surface *surface;
    struct surface_stage_tile *tiles;
    size_t tile_count;
    size_t tile_capacity;
};

struct surface_change {
    struct surface_stage_tile *stage;
    size_t resident_index;
    uint32_t new_hash;
    bool is_new;
    bool adds_pending;
};

static const uint8_t g_zero_tile[SURFACE_TILE_BYTES] = {0};

static bool size_mul(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b)
        return false;
    *out = a * b;
    return true;
}

static bool size_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static bool valid_format(enum surface_pixel_format format)
{
    return format == SURFACE_FORMAT_ABGR8888 ||
           format == SURFACE_FORMAT_BGRA8888 ||
           format == SURFACE_FORMAT_RGBA8888;
}

static bool valid_tile(const struct surface *surface, uint32_t x, uint32_t y)
{
    return surface != nullptr && x < surface->width_tiles &&
           y < surface->height_tiles;
}

static size_t find_resident(const struct surface *surface, uint32_t x,
                            uint32_t y)
{
    for (size_t i = 0; i < surface->resident_count; i++) {
        if (surface->residents[i].x == x && surface->residents[i].y == y)
            return i;
    }
    return SIZE_MAX;
}

static size_t find_stage(const struct surface_frame *frame, uint32_t x,
                         uint32_t y)
{
    for (size_t i = 0; i < frame->tile_count; i++) {
        if (frame->tiles[i].x == x && frame->tiles[i].y == y)
            return i;
    }
    return SIZE_MAX;
}

static size_t find_dirty(const struct surface *surface, size_t resident_index)
{
    for (size_t i = 0; i < surface->dirty_count; i++) {
        if (surface->dirty[i].resident_index == resident_index)
            return i;
    }
    return SIZE_MAX;
}

static bool validate_buffer(uint32_t width, uint32_t height,
                            const void *pixels, size_t buffer_bytes,
                            size_t stride, enum surface_pixel_format format)
{
    size_t row_bytes;
    size_t rows_offset;
    size_t required;

    if (pixels == nullptr || width == 0 || height == 0)
        return false;
    if (!valid_format(format) || !size_mul((size_t)width,
                                           SURFACE_TILE_CHANNELS, &row_bytes))
        return false;
    if (stride < row_bytes ||
        !size_mul((size_t)(height - 1), stride, &rows_offset) ||
        !size_add(rows_offset, row_bytes, &required))
        return false;
    return buffer_bytes >= required;
}

static enum surface_error reserve_stages(struct surface_frame *frame,
                                         size_t needed)
{
    if (needed <= frame->tile_capacity)
        return SURFACE_OK;
    size_t capacity = frame->tile_capacity == 0 ? 4 : frame->tile_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    size_t bytes;
    if (!size_mul(capacity, sizeof(*frame->tiles), &bytes))
        return SURFACE_ERR_OVERFLOW;
    struct surface_stage_tile *tiles = calloc(1, bytes);
    if (tiles == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    if (frame->tiles != nullptr) {
        memcpy(tiles, frame->tiles,
               frame->tile_count * sizeof(*frame->tiles));
        free(frame->tiles);
    }
    frame->tiles = tiles;
    frame->tile_capacity = capacity;
    return SURFACE_OK;
}

static enum surface_error stage_tile(struct surface_frame *frame,
                                     uint32_t x, uint32_t y,
                                     struct surface_stage_tile **out)
{
    size_t existing = find_stage(frame, x, y);
    if (existing != SIZE_MAX) {
        *out = &frame->tiles[existing];
        return SURFACE_OK;
    }

    if (frame->tile_count >= frame->surface->max_resident_tiles)
        return SURFACE_ERR_CAPACITY;

    size_t needed;
    if (!size_add(frame->tile_count, 1, &needed))
        return SURFACE_ERR_OVERFLOW;
    enum surface_error error = reserve_stages(frame, needed);
    if (error != SURFACE_OK)
        return error;
    uint8_t *pixels = calloc(1, SURFACE_TILE_BYTES);
    if (pixels == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    size_t resident = find_resident(frame->surface, x, y);
    if (resident != SIZE_MAX)
        memcpy(pixels, frame->surface->residents[resident].pixels,
               SURFACE_TILE_BYTES);

    struct surface_stage_tile *stage = &frame->tiles[frame->tile_count++];
    stage->x = x;
    stage->y = y;
    stage->pixels = pixels;
    *out = stage;
    return SURFACE_OK;
}

static void free_stages(struct surface_frame *frame)
{
    if (frame == nullptr)
        return;
    for (size_t i = 0; i < frame->tile_count; i++)
        free(frame->tiles[i].pixels);
    free(frame->tiles);
    frame->tiles = nullptr;
    frame->tile_count = 0;
    frame->tile_capacity = 0;
}

static void convert_pixel(uint8_t *destination, const uint8_t *source,
                          enum surface_pixel_format format)
{
    if (format == SURFACE_FORMAT_BGRA8888) {
        destination[0] = source[2];
        destination[1] = source[1];
        destination[2] = source[0];
        destination[3] = source[3];
    } else {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        destination[3] = source[3];
    }
}

static enum surface_error reserve_residents(struct surface *surface,
                                            size_t needed)
{
    if (needed <= surface->resident_capacity)
        return SURFACE_OK;
    size_t capacity = surface->resident_capacity == 0
                          ? 4
                          : surface->resident_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > surface->max_resident_tiles)
        capacity = surface->max_resident_tiles;
    size_t bytes;
    if (!size_mul(capacity, sizeof(*surface->residents), &bytes))
        return SURFACE_ERR_OVERFLOW;
    struct surface_tile *residents = calloc(1, bytes);
    if (residents == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    if (surface->residents != nullptr) {
        memcpy(residents, surface->residents,
               surface->resident_count * sizeof(*surface->residents));
        free(surface->residents);
    }
    surface->residents = residents;
    surface->resident_capacity = capacity;
    return SURFACE_OK;
}

static enum surface_error reserve_dirty(struct surface *surface,
                                        size_t needed)
{
    if (needed <= surface->dirty_capacity)
        return SURFACE_OK;
    size_t capacity = surface->dirty_capacity == 0 ? 4 : surface->dirty_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > surface->max_pending_tiles)
        capacity = surface->max_pending_tiles;
    size_t bytes;
    if (!size_mul(capacity, sizeof(*surface->dirty), &bytes))
        return SURFACE_ERR_OVERFLOW;
    struct surface_dirty_entry *dirty = calloc(1, bytes);
    if (dirty == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    if (surface->dirty != nullptr) {
        memcpy(dirty, surface->dirty,
               surface->dirty_count * sizeof(*surface->dirty));
        free(surface->dirty);
    }
    surface->dirty = dirty;
    surface->dirty_capacity = capacity;
    return SURFACE_OK;
}

enum surface_error surface_create(uint32_t width_tiles, uint32_t height_tiles,
                                  size_t max_resident_tiles,
                                  size_t max_pending_tiles,
                                  bool latest_wins,
                                  struct surface **out)
{
    if (out == nullptr)
        return SURFACE_ERR_ARGUMENT;
    *out = nullptr;
    if (width_tiles == 0 || height_tiles == 0 || width_tiles > 1024 ||
        height_tiles > 1024 || max_resident_tiles == 0 ||
        max_pending_tiles == 0 ||
        max_resident_tiles > SURFACE_MAX_RESIDENT_TILES ||
        max_pending_tiles > SURFACE_MAX_RESIDENT_TILES)
        return width_tiles == 0 || height_tiles == 0 || width_tiles > 1024 ||
                       height_tiles > 1024
                   ? SURFACE_ERR_DIMENSION
                   : SURFACE_ERR_CAPACITY;

    size_t pixel_width;
    size_t pixel_height;
    if (!size_mul((size_t)width_tiles, SURFACE_TILE_SIZE, &pixel_width) ||
        !size_mul((size_t)height_tiles, SURFACE_TILE_SIZE, &pixel_height) ||
        pixel_width > UINT32_MAX || pixel_height > UINT32_MAX)
        return SURFACE_ERR_OVERFLOW;

    struct surface *surface = calloc(1, sizeof(*surface));
    if (surface == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    surface->width_tiles = width_tiles;
    surface->height_tiles = height_tiles;
    surface->pixel_width = (uint32_t)pixel_width;
    surface->pixel_height = (uint32_t)pixel_height;
    surface->max_resident_tiles = max_resident_tiles;
    surface->max_pending_tiles = max_pending_tiles;
    surface->latest_wins = latest_wins;
    surface->zero_hash = (uint32_t)mz_crc32(
        MZ_CRC32_INIT, g_zero_tile, SURFACE_TILE_BYTES);
    *out = surface;
    return SURFACE_OK;
}

void surface_destroy(struct surface *surface)
{
    if (surface == nullptr)
        return;
    surface_reset(surface);
    free(surface->residents);
    free(surface->dirty);
    free(surface);
}

enum surface_error surface_frame_begin(struct surface *surface,
                                       struct surface_frame **out)
{
    if (out == nullptr || surface == nullptr)
        return SURFACE_ERR_ARGUMENT;
    *out = nullptr;
    if (surface->active_frame != nullptr)
        return SURFACE_ERR_FRAME_ACTIVE;
    struct surface_frame *frame = calloc(1, sizeof(*frame));
    if (frame == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    frame->surface = surface;
    surface->active_frame = frame;
    *out = frame;
    return SURFACE_OK;
}

enum surface_error surface_frame_update_tile(
    struct surface_frame *frame, uint32_t tile_x, uint32_t tile_y,
    const void *pixels, size_t buffer_bytes, size_t stride,
    enum surface_pixel_format format)
{
    if (frame == nullptr || frame->surface == nullptr)
        return SURFACE_ERR_ARGUMENT;
    if (!valid_tile(frame->surface, tile_x, tile_y))
        return SURFACE_ERR_RECTANGLE;
    if (!validate_buffer(SURFACE_TILE_SIZE, SURFACE_TILE_SIZE, pixels,
                         buffer_bytes, stride, format))
        return pixels == nullptr ? SURFACE_ERR_ARGUMENT
            : !valid_format(format) ? SURFACE_ERR_FORMAT
                                      : (stride < SURFACE_TILE_ROW_BYTES
                                             ? SURFACE_ERR_STRIDE
                                             : SURFACE_ERR_BUFFER);
    return surface_frame_update_region(
        frame, tile_x * SURFACE_TILE_SIZE, tile_y * SURFACE_TILE_SIZE,
        SURFACE_TILE_SIZE, SURFACE_TILE_SIZE, pixels, buffer_bytes, stride,
        format);
}

enum surface_error surface_frame_update_region(
    struct surface_frame *frame, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, const void *pixels,
    size_t buffer_bytes, size_t stride, enum surface_pixel_format format)
{
    if (frame == nullptr || frame->surface == nullptr)
        return SURFACE_ERR_ARGUMENT;
    struct surface *surface = frame->surface;
    if (width == 0 || height == 0 || x >= surface->pixel_width ||
        y >= surface->pixel_height || width > surface->pixel_width - x ||
        height > surface->pixel_height - y)
        return SURFACE_ERR_RECTANGLE;
    if (!valid_format(format))
        return SURFACE_ERR_FORMAT;
    size_t row_bytes;
    if (!size_mul((size_t)width, SURFACE_TILE_CHANNELS, &row_bytes))
        return SURFACE_ERR_OVERFLOW;
    if (stride < row_bytes)
        return SURFACE_ERR_STRIDE;
    size_t required_offset;
    size_t required;
    if (!size_mul((size_t)(height - 1), stride, &required_offset) ||
        !size_add(required_offset, row_bytes, &required))
        return SURFACE_ERR_OVERFLOW;
    if (pixels == nullptr)
        return SURFACE_ERR_ARGUMENT;
    if (buffer_bytes < required)
        return SURFACE_ERR_BUFFER;

    const uint8_t *source = pixels;
    uint32_t first_tile_x = x / SURFACE_TILE_SIZE;
    uint32_t last_tile_x = (x + width - 1) / SURFACE_TILE_SIZE;
    uint32_t first_tile_y = y / SURFACE_TILE_SIZE;
    uint32_t last_tile_y = (y + height - 1) / SURFACE_TILE_SIZE;
    size_t touched_width = (size_t)(last_tile_x - first_tile_x) + 1;
    size_t touched_height = (size_t)(last_tile_y - first_tile_y) + 1;
    size_t touched_tiles;
    if (!size_mul(touched_width, touched_height, &touched_tiles))
        return SURFACE_ERR_OVERFLOW;
    if (touched_tiles > surface->max_resident_tiles)
        return SURFACE_ERR_CAPACITY;
    size_t new_stages = 0;
    for (uint32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++) {
        for (uint32_t tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++) {
            if (find_stage(frame, tile_x, tile_y) == SIZE_MAX)
                new_stages++;
        }
    }
    if (new_stages > surface->max_resident_tiles - frame->tile_count)
        return SURFACE_ERR_CAPACITY;
    for (uint32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++) {
        for (uint32_t tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++) {
            uint32_t tile_left = tile_x * SURFACE_TILE_SIZE;
            uint32_t tile_top = tile_y * SURFACE_TILE_SIZE;
            uint32_t copy_left = x > tile_left ? x : tile_left;
            uint32_t copy_top = y > tile_top ? y : tile_top;
            uint32_t right = x + width;
            uint32_t bottom = y + height;
            uint32_t copy_right = right < tile_left + SURFACE_TILE_SIZE
                                      ? right
                                      : tile_left + SURFACE_TILE_SIZE;
            uint32_t copy_bottom = bottom < tile_top + SURFACE_TILE_SIZE
                                       ? bottom
                                       : tile_top + SURFACE_TILE_SIZE;
            struct surface_stage_tile *stage;
            enum surface_error error = stage_tile(frame, tile_x, tile_y, &stage);
            if (error != SURFACE_OK)
                return error;
            for (uint32_t py = copy_top; py < copy_bottom; py++) {
                uint32_t source_y = py - y;
                uint32_t local_y = py - tile_top;
                const uint8_t *source_row = source +
                    (size_t)source_y * stride +
                    (size_t)(copy_left - x) * SURFACE_TILE_CHANNELS;
                uint8_t *destination_row = stage->pixels +
                    (size_t)local_y * SURFACE_TILE_ROW_BYTES +
                    (size_t)(copy_left - tile_left) * SURFACE_TILE_CHANNELS;
                for (uint32_t px = copy_left; px < copy_right; px++) {
                    convert_pixel(destination_row, source_row, format);
                    source_row += SURFACE_TILE_CHANNELS;
                    destination_row += SURFACE_TILE_CHANNELS;
                }
            }
        }
    }
    return SURFACE_OK;
}

static bool tile_changed(uint32_t old_hash, const uint8_t *old_pixels,
                         const uint8_t *new_pixels, uint32_t *new_hash)
{
    *new_hash = (uint32_t)mz_crc32(
        MZ_CRC32_INIT, new_pixels, SURFACE_TILE_BYTES);
    return old_hash != *new_hash ||
           memcmp(old_pixels, new_pixels, SURFACE_TILE_BYTES) != 0;
}

static void discard_frame(struct surface_frame *frame)
{
    struct surface *surface = frame->surface;
    free_stages(frame);
    if (surface->active_frame == frame)
        surface->active_frame = nullptr;
    free(frame);
}

enum surface_error surface_frame_commit(struct surface_frame *frame)
{
    if (frame == nullptr || frame->surface == nullptr)
        return SURFACE_ERR_ARGUMENT;
    struct surface *surface = frame->surface;
    size_t changed_count = 0;
    size_t new_residents = 0;
    size_t new_pending = 0;
    for (size_t i = 0; i < frame->tile_count; i++) {
        struct surface_stage_tile *stage = &frame->tiles[i];
        size_t resident = find_resident(surface, stage->x, stage->y);
        const uint8_t *current = resident == SIZE_MAX
                                     ? g_zero_tile
                                     : surface->residents[resident].pixels;
        uint32_t current_hash = resident == SIZE_MAX
                                    ? surface->zero_hash
                                    : surface->residents[resident].content_hash;
        uint32_t new_hash;
        if (!tile_changed(current_hash, current, stage->pixels, &new_hash))
            continue;
        changed_count++;
        if (resident == SIZE_MAX) {
            new_residents++;
            new_pending++;
        } else if (!surface->residents[resident].pending) {
            new_pending++;
        }
    }
    if (changed_count == 0) {
        discard_frame(frame);
        return SURFACE_OK;
    }
    size_t required_residents;
    if (!size_add(surface->resident_count, new_residents,
                  &required_residents))
        return SURFACE_ERR_OVERFLOW;
    if (surface->generation == UINT64_MAX ||
        required_residents > surface->max_resident_tiles)
        return required_residents > surface->max_resident_tiles
                   ? SURFACE_ERR_CAPACITY
                   : SURFACE_ERR_OVERFLOW;

    size_t required_pending;
    if (!size_add(surface->dirty_count, new_pending, &required_pending))
        return SURFACE_ERR_OVERFLOW;
    size_t evictions = 0;
    if (surface->latest_wins) {
        if (required_pending > surface->max_pending_tiles)
            evictions = required_pending - surface->max_pending_tiles;
        if (evictions > UINT64_MAX - surface->dropped_tiles ||
            changed_count > UINT64_MAX - surface->dropped_tiles)
            return SURFACE_ERR_OVERFLOW;
    } else if (required_pending > surface->max_pending_tiles) {
        return SURFACE_ERR_CAPACITY;
    }

    size_t change_bytes;
    if (!size_mul(changed_count, sizeof(struct surface_change), &change_bytes))
        return SURFACE_ERR_OVERFLOW;
    struct surface_change *changes = calloc(1, change_bytes);
    if (changes == nullptr)
        return SURFACE_ERR_NO_MEMORY;
    size_t change_index = 0;
    for (size_t i = 0; i < frame->tile_count; i++) {
        struct surface_stage_tile *stage = &frame->tiles[i];
        size_t resident = find_resident(surface, stage->x, stage->y);
        const uint8_t *current = resident == SIZE_MAX
                                     ? g_zero_tile
                                     : surface->residents[resident].pixels;
        uint32_t current_hash = resident == SIZE_MAX
                                    ? surface->zero_hash
                                    : surface->residents[resident].content_hash;
        uint32_t new_hash;
        if (!tile_changed(current_hash, current, stage->pixels, &new_hash))
            continue;
        changes[change_index].stage = stage;
        changes[change_index].resident_index = resident;
        changes[change_index].is_new = resident == SIZE_MAX;
        changes[change_index].adds_pending = resident == SIZE_MAX ||
            !surface->residents[resident].pending;
        changes[change_index].new_hash = new_hash;
        change_index++;
    }

    enum surface_error error = reserve_residents(surface, required_residents);
    if (error != SURFACE_OK) {
        free(changes);
        return error;
    }
    size_t dirty_target = required_pending;
    if (dirty_target > surface->max_pending_tiles)
        dirty_target = surface->max_pending_tiles;
    error = reserve_dirty(surface, dirty_target);
    if (error != SURFACE_OK) {
        free(changes);
        return error;
    }

    uint64_t generation = surface->generation + 1;
    for (size_t i = 0; i < changed_count; i++) {
        struct surface_change *change = &changes[i];
        struct surface_stage_tile *stage = change->stage;
        size_t resident = change->resident_index;
        if (change->is_new) {
            resident = surface->resident_count;
            surface->resident_count++;
            struct surface_tile *tile = &surface->residents[resident];
            tile->x = stage->x;
            tile->y = stage->y;
            tile->pixels = stage->pixels;
            stage->pixels = nullptr;
            tile->generation = generation;
            tile->content_hash = change->new_hash;
            tile->pending = false;
        } else {
            struct surface_tile *tile = &surface->residents[resident];
            memcpy(tile->pixels, stage->pixels, SURFACE_TILE_BYTES);
            tile->generation = generation;
            tile->content_hash = change->new_hash;
        }
        struct surface_tile *tile = &surface->residents[resident];
        size_t dirty = find_dirty(surface, resident);
        if (dirty != SIZE_MAX) {
            surface->dirty[dirty].generation = generation;
        } else {
            while (surface->dirty_count >= surface->max_pending_tiles) {
                size_t evicted = surface->dirty[0].resident_index;
                surface->residents[evicted].pending = false;
                memmove(surface->dirty, surface->dirty + 1,
                        (surface->dirty_count - 1) * sizeof(*surface->dirty));
                surface->dirty_count--;
                surface->dropped_tiles++;
            }
            size_t dirty_index = surface->dirty_count;
            surface->dirty[dirty_index].resident_index = resident;
            surface->dirty[dirty_index].generation = generation;
            surface->dirty_count++;
            tile->pending = true;
        }
    }
    surface->generation = generation;
    free(changes);
    discard_frame(frame);
    return SURFACE_OK;
}

void surface_frame_abort(struct surface_frame *frame)
{
    if (frame == nullptr)
        return;
    discard_frame(frame);
}

enum surface_error surface_read_tile(const struct surface *surface,
                                     uint32_t tile_x, uint32_t tile_y,
                                     struct surface_tile_view *out)
{
    if (surface == nullptr || out == nullptr)
        return SURFACE_ERR_ARGUMENT;
    if (!valid_tile(surface, tile_x, tile_y))
        return SURFACE_ERR_RECTANGLE;
    size_t resident = find_resident(surface, tile_x, tile_y);
    if (resident == SIZE_MAX) {
        out->pixels = g_zero_tile;
        out->generation = 0;
        out->content_hash = surface->zero_hash;
    } else {
        out->pixels = surface->residents[resident].pixels;
        out->generation = surface->residents[resident].generation;
        out->content_hash = surface->residents[resident].content_hash;
    }
    out->bytes = SURFACE_TILE_BYTES;
    return SURFACE_OK;
}

static void fill_dirty_view(const struct surface *surface,
                            const struct surface_dirty_entry *entry,
                            struct surface_dirty_view *out)
{
    const struct surface_tile *tile = &surface->residents[entry->resident_index];
    out->tile_x = tile->x;
    out->tile_y = tile->y;
    out->pixels = tile->pixels;
    out->bytes = SURFACE_TILE_BYTES;
    out->generation = entry->generation;
    out->content_hash = tile->content_hash;
}

enum surface_error surface_dirty_peek(const struct surface *surface,
                                      struct surface_dirty_view *out)
{
    if (surface == nullptr || out == nullptr)
        return SURFACE_ERR_ARGUMENT;
    if (surface->dirty_count == 0)
        return SURFACE_ERR_EMPTY;
    fill_dirty_view(surface, &surface->dirty[0], out);
    return SURFACE_OK;
}

enum surface_error surface_dirty_pop(struct surface *surface,
                                     struct surface_dirty_view *out)
{
    if (surface == nullptr || out == nullptr)
        return SURFACE_ERR_ARGUMENT;
    if (surface->dirty_count == 0)
        return SURFACE_ERR_EMPTY;
    fill_dirty_view(surface, &surface->dirty[0], out);
    size_t resident = surface->dirty[0].resident_index;
    surface->residents[resident].pending = false;
    memmove(surface->dirty, surface->dirty + 1,
            (surface->dirty_count - 1) * sizeof(*surface->dirty));
    surface->dirty_count--;
    return SURFACE_OK;
}

size_t surface_resident_count(const struct surface *surface)
{
    return surface == nullptr ? 0 : surface->resident_count;
}

enum surface_error surface_read_resident(
    const struct surface *surface, size_t index,
    struct surface_resident_view *out)
{
    if (surface == nullptr || out == nullptr)
        return SURFACE_ERR_ARGUMENT;
    if (index >= surface->resident_count)
        return SURFACE_ERR_INDEX;
    const struct surface_tile *tile = &surface->residents[index];
    out->tile_x = tile->x;
    out->tile_y = tile->y;
    out->pixels = tile->pixels;
    out->bytes = SURFACE_TILE_BYTES;
    out->generation = tile->generation;
    out->content_hash = tile->content_hash;
    return SURFACE_OK;
}

enum surface_error surface_get_stats(const struct surface *surface,
                                     struct surface_stats *out)
{
    if (surface == nullptr || out == nullptr)
        return SURFACE_ERR_ARGUMENT;
    out->width_tiles = surface->width_tiles;
    out->height_tiles = surface->height_tiles;
    out->pixel_width = surface->pixel_width;
    out->pixel_height = surface->pixel_height;
    out->generation = surface->generation;
    out->resident_tiles = surface->resident_count;
    out->pending_tiles = surface->dirty_count;
    out->max_resident_tiles = surface->max_resident_tiles;
    out->max_pending_tiles = surface->max_pending_tiles;
    out->dropped_tiles = surface->dropped_tiles;
    if (!size_mul(surface->resident_count, SURFACE_TILE_BYTES,
                  &out->allocated_pixel_bytes))
        return SURFACE_ERR_OVERFLOW;
    out->staged_tiles = surface->active_frame == nullptr
                            ? 0
                            : surface->active_frame->tile_count;
    if (!size_mul(out->staged_tiles, SURFACE_TILE_BYTES,
                  &out->allocated_staging_pixel_bytes))
        return SURFACE_ERR_OVERFLOW;
    return SURFACE_OK;
}

void surface_reset(struct surface *surface)
{
    if (surface == nullptr)
        return;
    if (surface->active_frame != nullptr)
        surface_frame_abort(surface->active_frame);
    for (size_t i = 0; i < surface->resident_count; i++)
        free(surface->residents[i].pixels);
    free(surface->residents);
    free(surface->dirty);
    surface->residents = nullptr;
    surface->dirty = nullptr;
    surface->resident_count = 0;
    surface->resident_capacity = 0;
    surface->dirty_count = 0;
    surface->dirty_capacity = 0;
    surface->generation = 0;
    surface->dropped_tiles = 0;
}

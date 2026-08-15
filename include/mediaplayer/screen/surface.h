#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_SURFACE_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SURFACE_TILE_SIZE 128u
#define SURFACE_TILE_CHANNELS 4u
#define SURFACE_TILE_ROW_BYTES \
    ((size_t)SURFACE_TILE_SIZE * SURFACE_TILE_CHANNELS)
#define SURFACE_TILE_BYTES \
    (SURFACE_TILE_ROW_BYTES * (size_t)SURFACE_TILE_SIZE)
#define SURFACE_MAX_RESIDENT_TILES 4096u

// The packed ABGR8888 representation used by the existing video path has
// R,G,B,A bytes in memory on the supported little-endian targets.
enum surface_pixel_format {
    SURFACE_FORMAT_ABGR8888 = 0,
    SURFACE_FORMAT_BGRA8888,
    SURFACE_FORMAT_RGBA8888,
};

enum surface_error {
    SURFACE_OK = 0,
    SURFACE_ERR_ARGUMENT,
    SURFACE_ERR_DIMENSION,
    SURFACE_ERR_CAPACITY,
    SURFACE_ERR_BUFFER,
    SURFACE_ERR_STRIDE,
    SURFACE_ERR_FORMAT,
    SURFACE_ERR_RECTANGLE,
    SURFACE_ERR_FRAME_ACTIVE,
    SURFACE_ERR_NO_FRAME,
    SURFACE_ERR_EMPTY,
    SURFACE_ERR_INDEX,
    SURFACE_ERR_NO_MEMORY,
    SURFACE_ERR_OVERFLOW,
};

struct surface;
struct surface_frame;

struct surface_tile_view {
    const uint8_t *pixels;
    size_t bytes;
    uint64_t generation;
    uint32_t content_hash;
};

struct surface_dirty_view {
    uint32_t tile_x;
    uint32_t tile_y;
    const uint8_t *pixels;
    size_t bytes;
    uint64_t generation;
    uint32_t content_hash;
};

struct surface_resident_view {
    uint32_t tile_x;
    uint32_t tile_y;
    const uint8_t *pixels;
    size_t bytes;
    uint64_t generation;
    uint32_t content_hash;
};

struct surface_stats {
    uint32_t width_tiles;
    uint32_t height_tiles;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint64_t generation;
    size_t resident_tiles;
    size_t pending_tiles;
    size_t max_resident_tiles;
    size_t max_pending_tiles;
    uint64_t dropped_tiles;
    size_t allocated_pixel_bytes;
    size_t staged_tiles;
    size_t allocated_staging_pixel_bytes;
};

// Creates a sparse logical surface. No pixel blocks are allocated here.
enum surface_error surface_create(uint32_t width_tiles, uint32_t height_tiles,
                                  size_t max_resident_tiles,
                                  size_t max_pending_tiles,
                                  bool latest_wins,
                                  struct surface **out);

void surface_destroy(struct surface *surface);

// Starts the only frame transaction permitted by the single-main-thread
// contract. A failed commit leaves the frame active so the caller can abort.
enum surface_error surface_frame_begin(struct surface *surface,
                                       struct surface_frame **out);

enum surface_error surface_frame_update_tile(
    struct surface_frame *frame, uint32_t tile_x, uint32_t tile_y,
    const void *pixels, size_t buffer_bytes, size_t stride,
    enum surface_pixel_format format);

// x/y and width/height are pixel coordinates in the logical surface.
enum surface_error surface_frame_update_region(
    struct surface_frame *frame, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, const void *pixels,
    size_t buffer_bytes, size_t stride, enum surface_pixel_format format);

enum surface_error surface_frame_commit(struct surface_frame *frame);
void surface_frame_abort(struct surface_frame *frame);

// The returned pixel view remains valid until the next operation on surface.
enum surface_error surface_read_tile(const struct surface *surface,
                                     uint32_t tile_x, uint32_t tile_y,
                                     struct surface_tile_view *out);

// Dirty entries are FIFO. Their pixel pointer always names the newest state
// committed for that coordinate, including when a pending tile is updated.
enum surface_error surface_dirty_peek(const struct surface *surface,
                                      struct surface_dirty_view *out);
enum surface_error surface_dirty_pop(struct surface *surface,
                                     struct surface_dirty_view *out);

// Returns the number of actual sparse residents, never the logical tile
// count. The result is stable until the next surface mutation.
size_t surface_resident_count(const struct surface *surface);

// Reads one actual sparse resident by dense resident index. The returned
// pixel view remains valid until the next operation on surface.
enum surface_error surface_read_resident(
    const struct surface *surface, size_t index,
    struct surface_resident_view *out);

enum surface_error surface_get_stats(const struct surface *surface,
                                     struct surface_stats *out);

// Drops all resident and pending state and ends an active frame.
void surface_reset(struct surface *surface);

#endif // ENDSTONE_MEDIAPLAYER_SCREEN_SURFACE_H

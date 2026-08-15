#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_PRESENTER_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_PRESENTER_H

#include "mediaplayer/screen/screen_geometry.h"
#include "mediaplayer/screen/surface.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRESENTER_MAX_VIEWERS 128u
#define PRESENTER_MAX_BUDGET 4096u

enum presenter_error {
    PRESENTER_OK = 0,
    PRESENTER_ERR_ARGUMENT,
    PRESENTER_ERR_REACH,
    PRESENTER_ERR_VIEWER_COUNT,
    PRESENTER_ERR_BUDGET,
    PRESENTER_ERR_BACKEND,
    PRESENTER_ERR_SURFACE,
    PRESENTER_ERR_SOURCE,
};

// A copied snapshot of one viewer. The player and context pointers are
// borrowed and are returned to the backend only during its synchronous call.
struct presenter_viewer {
    void *player;
    void *context;
    uint64_t stable_id;
    double x;
    double y;
    double z;
    char dimension[64];
};

struct presenter_tile {
    uint32_t tile_x;
    uint32_t tile_y;
    const uint8_t *pixels;
    size_t bytes;
    uint64_t generation;
    uint32_t content_hash;
    const struct presenter_viewer *viewers;
    size_t viewer_count;
};

struct presenter_backend {
    void *context;
    bool (*send)(void *context, const struct presenter_tile *tile);
};

struct presenter_stream_tile {
    const uint8_t *pixels;
    size_t bytes;
    uint64_t generation;
    uint32_t content_hash;
};

struct presenter_stats {
    size_t examined;
    size_t sent;
    size_t skipped;
    size_t failures;
};

struct presenter_resident_cursor {
    size_t index;
};

struct presenter {
    struct surface *surface;
    const struct screen_geom *geometry;
    double reach;
    struct presenter_backend backend;
    struct presenter_viewer viewers[PRESENTER_MAX_VIEWERS];
    size_t viewer_count;
    // The backend receives this synchronous scratch array. It is not state.
    struct presenter_viewer eligible[PRESENTER_MAX_VIEWERS];
};

enum presenter_error presenter_init(
    struct presenter *presenter, struct surface *surface,
    const struct screen_geom *geometry, double reach,
    struct presenter_backend backend);

enum presenter_error presenter_set_viewers(
    struct presenter *presenter, const struct presenter_viewer *viewers,
    size_t viewer_count);

// Examines at most budget FIFO dirty entries. A successful backend call or a
// no-eligible-viewer entry consumes the current dirty entry; backend failure
// retains it and stops the tick.
enum presenter_error presenter_tick(struct presenter *presenter,
                                    size_t budget,
                                    struct presenter_stats *out);

// Resends sparse residents to one viewer. The caller owns cursor and may call
// this repeatedly; it advances only after skipped or successfully sent
// residents and leaves the current index untouched after backend failure.
enum presenter_error presenter_resend(
    struct presenter *presenter, const struct presenter_viewer *viewer,
    struct presenter_resident_cursor *cursor, size_t budget, bool *done,
    struct presenter_stats *out);

// Presents a resumable row-major logical tile stream. The cursor advances
// only after a skipped entry or a successful backend send.
enum presenter_error presenter_stream(
    struct presenter *presenter, uint64_t tile_count, uint64_t *cursor,
    size_t budget,
    bool (*read_tile)(void *context, uint64_t tile_index, uint32_t tile_x,
                      uint32_t tile_y, struct presenter_stream_tile *out),
    void *read_context, bool *done, struct presenter_stats *out);

// Returns squared point-to-block distance. Each block occupies the closed
// world interval [coord, coord + 1] on each axis. Non-finite inputs return
// INFINITY.
double presenter_point_aabb_distance_squared(
    double x, double y, double z, struct screen_pos block);

bool presenter_viewer_reaches_tile(
    const struct presenter_viewer *viewer,
    const struct screen_geom *geometry, uint32_t tile_x, uint32_t tile_y,
    double reach);

#endif // ENDSTONE_MEDIAPLAYER_SCREEN_PRESENTER_H

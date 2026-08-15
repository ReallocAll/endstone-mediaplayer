#include "mediaplayer/screen/presenter.h"

#include <math.h>
#include <string.h>

static void clear_stats(struct presenter_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
}

static bool valid_reach(double reach)
{
    return isfinite(reach) && reach >= 0.0;
}

static bool valid_geometry(const struct screen_geom *geometry)
{
    return geometry != nullptr &&
           screen_geom_validate_dimensions(geometry->width,
                                           geometry->height) ==
               SCREEN_GEOM_OK;
}

double presenter_point_aabb_distance_squared(
    double x, double y, double z, struct screen_pos block)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(z))
        return INFINITY;

    double lo_x = (double)block.x;
    double lo_y = (double)block.y;
    double lo_z = (double)block.z;
    double hi_x = lo_x + 1.0;
    double hi_y = lo_y + 1.0;
    double hi_z = lo_z + 1.0;
    double dx = x < lo_x ? lo_x - x : x > hi_x ? x - hi_x : 0.0;
    double dy = y < lo_y ? lo_y - y : y > hi_y ? y - hi_y : 0.0;
    double dz = z < lo_z ? lo_z - z : z > hi_z ? z - hi_z : 0.0;
    return dx * dx + dy * dy + dz * dz;
}

bool presenter_viewer_reaches_tile(
    const struct presenter_viewer *viewer,
    const struct screen_geom *geometry, uint32_t tile_x, uint32_t tile_y,
    double reach)
{
    if (viewer == nullptr || !valid_geometry(geometry) ||
        !valid_reach(reach) || tile_x >= (uint32_t)geometry->width ||
        tile_y >= (uint32_t)geometry->height ||
        strncmp(viewer->dimension, geometry->dimension,
                sizeof(viewer->dimension)) != 0)
        return false;
    struct screen_pos block = screen_geom_tile_pos(
        geometry, (int)tile_x, (int)tile_y);
    double distance = presenter_point_aabb_distance_squared(
        viewer->x, viewer->y, viewer->z, block);
    return isfinite(distance) && distance <= reach * reach;
}

enum presenter_error presenter_init(
    struct presenter *presenter, struct surface *surface,
    const struct screen_geom *geometry, double reach,
    struct presenter_backend backend)
{
    if (presenter == nullptr || surface == nullptr ||
        !valid_geometry(geometry) || backend.send == nullptr)
        return PRESENTER_ERR_ARGUMENT;
    if (!valid_reach(reach))
        return PRESENTER_ERR_REACH;
    memset(presenter, 0, sizeof(*presenter));
    presenter->surface = surface;
    presenter->geometry = geometry;
    presenter->reach = reach;
    presenter->backend = backend;
    return PRESENTER_OK;
}

enum presenter_error presenter_set_viewers(
    struct presenter *presenter, const struct presenter_viewer *viewers,
    size_t viewer_count)
{
    if (presenter == nullptr || (viewer_count != 0 && viewers == nullptr))
        return PRESENTER_ERR_ARGUMENT;
    if (viewer_count > PRESENTER_MAX_VIEWERS)
        return PRESENTER_ERR_VIEWER_COUNT;
    if (viewer_count != 0)
        memcpy(presenter->viewers, viewers,
               viewer_count * sizeof(*presenter->viewers));
    if (viewer_count < presenter->viewer_count)
        memset(&presenter->viewers[viewer_count], 0,
               (presenter->viewer_count - viewer_count) *
                   sizeof(*presenter->viewers));
    presenter->viewer_count = viewer_count;
    return PRESENTER_OK;
}

static bool viewer_eligible(const struct presenter *presenter,
                            const struct presenter_viewer *viewer,
                            uint32_t tile_x, uint32_t tile_y)
{
    return viewer->player != nullptr && presenter_viewer_reaches_tile(
        viewer, presenter->geometry, tile_x, tile_y, presenter->reach);
}

static size_t collect_eligible(struct presenter *presenter,
                              uint32_t tile_x, uint32_t tile_y)
{
    size_t count = 0;
    for (size_t i = 0; i < presenter->viewer_count; i++) {
        if (!viewer_eligible(presenter, &presenter->viewers[i],
                             tile_x, tile_y))
            continue;
        presenter->eligible[count++] = presenter->viewers[i];
    }
    return count;
}

static enum presenter_error send_tile(
    struct presenter *presenter, uint32_t tile_x, uint32_t tile_y,
    const uint8_t *pixels, size_t bytes, uint64_t generation,
    uint32_t content_hash, const struct presenter_viewer *viewers,
    size_t viewer_count)
{
    struct presenter_tile tile = {
        .tile_x = tile_x,
        .tile_y = tile_y,
        .pixels = pixels,
        .bytes = bytes,
        .generation = generation,
        .content_hash = content_hash,
        .viewers = viewers,
        .viewer_count = viewer_count,
    };
    return presenter->backend.send(presenter->backend.context, &tile)
               ? PRESENTER_OK
               : PRESENTER_ERR_BACKEND;
}

enum presenter_error presenter_tick(struct presenter *presenter,
                                    size_t budget,
                                    struct presenter_stats *out)
{
    if (presenter == nullptr || presenter->surface == nullptr ||
        !valid_geometry(presenter->geometry) ||
        presenter->backend.send == nullptr || out == nullptr)
        return PRESENTER_ERR_ARGUMENT;
    if (budget == 0 || budget > PRESENTER_MAX_BUDGET)
        return PRESENTER_ERR_BUDGET;
    clear_stats(out);

    while (out->examined < budget) {
        struct surface_dirty_view dirty;
        enum surface_error surface_error = surface_dirty_peek(
            presenter->surface, &dirty);
        if (surface_error == SURFACE_ERR_EMPTY)
            return PRESENTER_OK;
        if (surface_error != SURFACE_OK)
            return PRESENTER_ERR_SURFACE;
        out->examined++;
        size_t eligible = collect_eligible(
            presenter, dirty.tile_x, dirty.tile_y);
        if (eligible == 0) {
            // The view is deliberately not retained after the pop. Re-entry
            // uses the resident tile through presenter_resend.
            struct surface_dirty_view discarded;
            if (surface_dirty_pop(presenter->surface, &discarded) !=
                SURFACE_OK)
                return PRESENTER_ERR_SURFACE;
            out->skipped++;
            continue;
        }

        enum presenter_error error = send_tile(
            presenter, dirty.tile_x, dirty.tile_y, dirty.pixels, dirty.bytes,
            dirty.generation, dirty.content_hash, presenter->eligible,
            eligible);
        if (error != PRESENTER_OK) {
            out->failures++;
            return error;
        }
        struct surface_dirty_view discarded;
        if (surface_dirty_pop(presenter->surface, &discarded) != SURFACE_OK)
            return PRESENTER_ERR_SURFACE;
        out->sent++;
    }
    return PRESENTER_OK;
}

enum presenter_error presenter_resend(
    struct presenter *presenter, const struct presenter_viewer *viewer,
    struct presenter_resident_cursor *cursor, size_t budget, bool *done,
    struct presenter_stats *out)
{
    if (presenter == nullptr || presenter->surface == nullptr ||
        !valid_geometry(presenter->geometry) ||
        presenter->backend.send == nullptr || viewer == nullptr ||
        cursor == nullptr || done == nullptr || out == nullptr)
        return PRESENTER_ERR_ARGUMENT;
    if (budget == 0 || budget > PRESENTER_MAX_BUDGET)
        return PRESENTER_ERR_BUDGET;
    clear_stats(out);
    *done = false;
    size_t resident_count = surface_resident_count(presenter->surface);
    while (out->examined < budget && cursor->index < resident_count) {
        size_t index = cursor->index;
        struct surface_resident_view resident;
        if (surface_read_resident(presenter->surface, index, &resident) !=
            SURFACE_OK)
            return PRESENTER_ERR_SURFACE;
        out->examined++;
        if (!viewer_eligible(presenter, viewer, resident.tile_x,
                             resident.tile_y)) {
            cursor->index++;
            out->skipped++;
            continue;
        }
        enum presenter_error error = send_tile(
            presenter, resident.tile_x, resident.tile_y, resident.pixels,
            resident.bytes, resident.generation, resident.content_hash,
            viewer, 1);
        if (error != PRESENTER_OK) {
            out->failures++;
            return error;
        }
        cursor->index++;
        out->sent++;
    }
    *done = cursor->index >= resident_count;
    return PRESENTER_OK;
}

enum presenter_error presenter_stream(
    struct presenter *presenter, uint64_t tile_count, uint64_t *cursor,
    size_t budget,
    bool (*read_tile)(void *context, uint64_t tile_index, uint32_t tile_x,
                      uint32_t tile_y, struct presenter_stream_tile *out),
    void *read_context, bool *done, struct presenter_stats *out)
{
    if (presenter == nullptr || presenter->surface == nullptr ||
        !valid_geometry(presenter->geometry) ||
        presenter->backend.send == nullptr || cursor == nullptr ||
        read_tile == nullptr || done == nullptr || out == nullptr)
        return PRESENTER_ERR_ARGUMENT;
    if (budget == 0 || budget > PRESENTER_MAX_BUDGET)
        return PRESENTER_ERR_BUDGET;

    uint64_t expected_count = (uint64_t)(uint32_t)presenter->geometry->width *
                              (uint64_t)(uint32_t)presenter->geometry->height;
    if (tile_count != expected_count)
        return PRESENTER_ERR_ARGUMENT;

    clear_stats(out);
    *done = false;
    while (out->examined < budget && *cursor < tile_count) {
        uint64_t index = *cursor;
        uint32_t width = (uint32_t)presenter->geometry->width;
        uint32_t tile_x = (uint32_t)(index % width);
        uint32_t tile_y = (uint32_t)(index / width);
        out->examined++;

        size_t eligible = collect_eligible(presenter, tile_x, tile_y);
        if (eligible == 0) {
            (*cursor)++;
            out->skipped++;
            continue;
        }

        struct presenter_stream_tile source = {0};
        if (!read_tile(read_context, index, tile_x, tile_y, &source) ||
            source.pixels == nullptr || source.bytes == 0) {
            out->failures++;
            return PRESENTER_ERR_SOURCE;
        }
        enum presenter_error error = send_tile(
            presenter, tile_x, tile_y, source.pixels, source.bytes,
            source.generation, source.content_hash, presenter->eligible,
            eligible);
        if (error != PRESENTER_OK) {
            out->failures++;
            return error;
        }
        (*cursor)++;
        out->sent++;
    }
    *done = *cursor >= tile_count;
    return PRESENTER_OK;
}

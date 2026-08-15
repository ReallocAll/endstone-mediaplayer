#include "mediaplayer/screen/screen_geometry.h"
#include <limits.h>
#include <string.h>

// Viewer-relative tile ordering for each facing.
struct facing_info {
    int col_axis;
    int col_start;
    int col_dir;
};

static const struct facing_info s_facing[4] = {
    // SOUTH: columns increase with X.
    { 0, 0, +1 },
    // NORTH: columns decrease with X.
    { 0, 1, -1 },
    // EAST: columns decrease with Z.
    { 1, 1, -1 },
    // WEST: columns increase with Z.
    { 1, 0, +1 },
};

enum screen_geom_err screen_geom_validate_dimensions(int width, int height)
{
    if (width < 1)
        return SCREEN_GEOM_ERR_WIDTH_INVALID;
    if (width > SCREEN_LOGICAL_MAX_WIDTH)
        return SCREEN_GEOM_ERR_WIDTH_EXCEED;
    if (height < 1)
        return SCREEN_GEOM_ERR_HEIGHT_INVALID;
    if (height > SCREEN_LOGICAL_MAX_HEIGHT)
        return SCREEN_GEOM_ERR_HEIGHT_EXCEED;
    return SCREEN_GEOM_OK;
}

enum screen_geom_err screen_geom_validate(
    struct screen_pos c1, struct screen_pos c2,
    const char *dim1, const char *dim2,
    enum screen_facing facing,
    struct screen_geom *out)
{
    if (!dim1 || !dim2 || strcmp(dim1, dim2) != 0)
        return SCREEN_GEOM_ERR_DIMENSION_MISMATCH;

    if (facing < 0 || facing > 3)
        return SCREEN_GEOM_ERR_INVALID_FACING;

    int64_t dx = (int64_t)c2.x - (int64_t)c1.x;
    int64_t dy = (int64_t)c2.y - (int64_t)c1.y;
    int64_t dz = (int64_t)c2.z - (int64_t)c1.z;

    int is_xy_plane = (dz == 0);
    int is_zy_plane = (dx == 0);

    if (!is_xy_plane && !is_zy_plane)
        return SCREEN_GEOM_ERR_NOT_VERTICAL;

    // A point or vertical line accepts every facing.
    if (is_xy_plane && !is_zy_plane &&
        (facing == SCREEN_FACE_EAST || facing == SCREEN_FACE_WEST))
        return SCREEN_GEOM_ERR_INVALID_FACING;
    if (is_zy_plane && !is_xy_plane &&
        (facing == SCREEN_FACE_SOUTH || facing == SCREEN_FACE_NORTH))
        return SCREEN_GEOM_ERR_INVALID_FACING;

    int64_t width64, height64;
    if (is_xy_plane) {
        width64 = (dx < 0 ? -dx : dx) + 1;
        height64 = (dy < 0 ? -dy : dy) + 1;
    } else {
        width64 = (dz < 0 ? -dz : dz) + 1;
        height64 = (dy < 0 ? -dy : dy) + 1;
    }

    if (width64 > INT_MAX)
        return SCREEN_GEOM_ERR_WIDTH_EXCEED;
    if (height64 > INT_MAX)
        return SCREEN_GEOM_ERR_HEIGHT_EXCEED;
    enum screen_geom_err dimension_error =
        screen_geom_validate_dimensions((int)width64, (int)height64);
    if (dimension_error != SCREEN_GEOM_OK)
        return dimension_error;

    // Normalize horizontal bounds and vertical order.
    struct screen_pos lo, hi;
    if (is_xy_plane) {
        lo.x = c1.x < c2.x ? c1.x : c2.x;
        lo.z = c1.z;
        hi.x = c1.x > c2.x ? c1.x : c2.x;
        hi.z = c1.z;
    } else {
        lo.z = c1.z < c2.z ? c1.z : c2.z;
        lo.x = c1.x;
        hi.z = c1.z > c2.z ? c1.z : c2.z;
        hi.x = c1.x;
    }
    lo.y = c1.y > c2.y ? c1.y : c2.y;
    hi.y = c1.y < c2.y ? c1.y : c2.y;

    memset(out, 0, sizeof(*out));
    out->corner1 = lo;
    out->corner2 = hi;
    out->facing = facing;
    out->width = (int)width64;
    out->height = (int)height64;

    size_t dlen = strlen(dim1);
    if (dlen >= sizeof(out->dimension))
        dlen = sizeof(out->dimension) - 1;
    memcpy(out->dimension, dim1, dlen);
    out->dimension[dlen] = '\0';

    return SCREEN_GEOM_OK;
}

const char *screen_geom_err_name(enum screen_geom_err error)
{
    switch (error) {
    case SCREEN_GEOM_OK: return "ok";
    case SCREEN_GEOM_ERR_DIMENSION_MISMATCH: return "dimension mismatch";
    case SCREEN_GEOM_ERR_NOT_VERTICAL: return "screen is not vertical";
    case SCREEN_GEOM_ERR_WIDTH_EXCEED: return "screen width exceeds limit";
    case SCREEN_GEOM_ERR_HEIGHT_EXCEED: return "screen height exceeds limit";
    case SCREEN_GEOM_ERR_INVALID_FACING: return "invalid screen facing";
    case SCREEN_GEOM_ERR_WIDTH_INVALID: return "screen width is invalid";
    case SCREEN_GEOM_ERR_HEIGHT_INVALID: return "screen height is invalid";
    }
    return "unknown screen geometry error";
}

struct screen_pos screen_geom_tile_pos(const struct screen_geom *g, int col, int row)
{
    const struct facing_info *fi = &s_facing[g->facing];
    struct screen_pos p;

    int h_base;
    if (fi->col_axis == 0) {
        h_base = fi->col_start == 0 ? g->corner1.x : g->corner2.x;
    } else {
        h_base = fi->col_start == 0 ? g->corner1.z : g->corner2.z;
    }
    int64_t h64 = (int64_t)h_base + (int64_t)fi->col_dir * (int64_t)col;
    int64_t v64 = (int64_t)g->corner1.y - (int64_t)row;
    int h = (int)h64;
    int v = (int)v64;

    if (fi->col_axis == 0) {
        p.x = h;
        p.y = v;
        p.z = g->corner1.z;
    } else {
        p.x = g->corner1.x;
        p.y = v;
        p.z = h;
    }

    return p;
}

struct screen_pos screen_geom_backing_pos(const struct screen_geom *g, int col, int row)
{
    struct screen_pos p = screen_geom_tile_pos(g, col, row);
    switch (g->facing) {
    case SCREEN_FACE_SOUTH: p.z--; break;
    case SCREEN_FACE_NORTH: p.z++; break;
    case SCREEN_FACE_EAST: p.x--; break;
    case SCREEN_FACE_WEST: p.x++; break;
    }
    return p;
}

int screen_geom_tile_index(const struct screen_geom *g, int col, int row)
{
    if (!g || col < 0 || row < 0 || col >= g->width || row >= g->height)
        return -1;
    int64_t index = (int64_t)row * (int64_t)g->width + (int64_t)col;
    return index > INT_MAX ? -1 : (int)index;
}

int screen_geom_tile_count(const struct screen_geom *g)
{
    if (!g || screen_geom_validate_dimensions(g->width, g->height) !=
                 SCREEN_GEOM_OK)
        return 0;
    int64_t count = (int64_t)g->width * (int64_t)g->height;
    return count > INT_MAX ? 0 : (int)count;
}

int screen_geom_pixel_width(const struct screen_geom *g)
{
    if (!g || screen_geom_validate_dimensions(g->width, g->height) !=
                 SCREEN_GEOM_OK)
        return 0;
    int64_t width = (int64_t)g->width * SCREEN_TILE_SIZE;
    return width > INT_MAX ? 0 : (int)width;
}

int screen_geom_pixel_height(const struct screen_geom *g)
{
    if (!g || screen_geom_validate_dimensions(g->width, g->height) !=
                 SCREEN_GEOM_OK)
        return 0;
    int64_t height = (int64_t)g->height * SCREEN_TILE_SIZE;
    return height > INT_MAX ? 0 : (int)height;
}

enum screen_facing screen_geom_facing_from_player(
    struct screen_pos c1, struct screen_pos c2,
    struct screen_pos player_pos)
{
    int64_t dx = (int64_t)c2.x - (int64_t)c1.x;
    int64_t dz = (int64_t)c2.z - (int64_t)c1.z;

    if (dz == 0) {
        int wall_z = c1.z;
        if (player_pos.z > wall_z)
            return SCREEN_FACE_SOUTH;
        else
            return SCREEN_FACE_NORTH;
    } else if (dx == 0) {
        int wall_x = c1.x;
        if (player_pos.x > wall_x)
            return SCREEN_FACE_EAST;
        else
            return SCREEN_FACE_WEST;
    }

    return SCREEN_FACE_SOUTH;
}

int screen_geom_facing_candidates(
    struct screen_pos c1, struct screen_pos c2,
    enum screen_facing out[4])
{
    int64_t dx = (int64_t)c2.x - (int64_t)c1.x;
    int64_t dz = (int64_t)c2.z - (int64_t)c1.z;

    if (dx != 0 && dz != 0)
        return 0;
    if (dx != 0) {
        out[0] = SCREEN_FACE_SOUTH;
        out[1] = SCREEN_FACE_NORTH;
        return 2;
    }
    if (dz != 0) {
        out[0] = SCREEN_FACE_EAST;
        out[1] = SCREEN_FACE_WEST;
        return 2;
    }

    out[0] = SCREEN_FACE_SOUTH;
    out[1] = SCREEN_FACE_NORTH;
    out[2] = SCREEN_FACE_EAST;
    out[3] = SCREEN_FACE_WEST;
    return 4;
}

const char *screen_facing_name(enum screen_facing f)
{
    switch (f) {
    case SCREEN_FACE_SOUTH: return "south (+Z)";
    case SCREEN_FACE_NORTH: return "north (-Z)";
    case SCREEN_FACE_EAST:  return "east (+X)";
    case SCREEN_FACE_WEST:  return "west (-X)";
    default: return "unknown";
    }
}

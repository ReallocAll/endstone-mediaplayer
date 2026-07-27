#include "mediaplayer/screen/screen_geometry.h"
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

enum screen_geom_err screen_geom_validate(
    struct screen_pos c1, struct screen_pos c2,
    const char *dim1, const char *dim2,
    enum screen_facing facing,
    struct screen_geom *out)
{
    if (strcmp(dim1, dim2) != 0)
        return SCREEN_GEOM_ERR_DIMENSION_MISMATCH;

    if (facing < 0 || facing > 3)
        return SCREEN_GEOM_ERR_INVALID_FACING;

    int dx = c2.x - c1.x;
    int dy = c2.y - c1.y;
    int dz = c2.z - c1.z;

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

    int width, height;
    if (is_xy_plane) {
        width = (dx < 0 ? -dx : dx) + 1;
        height = (dy < 0 ? -dy : dy) + 1;
    } else {
        width = (dz < 0 ? -dz : dz) + 1;
        height = (dy < 0 ? -dy : dy) + 1;
    }

    if (width > SCREEN_MAX_WIDTH)
        return SCREEN_GEOM_ERR_WIDTH_EXCEED;
    if (height > SCREEN_MAX_HEIGHT)
        return SCREEN_GEOM_ERR_HEIGHT_EXCEED;

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
    out->width = width;
    out->height = height;

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
    int h = h_base + fi->col_dir * col;

    int v = g->corner1.y - row;

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
    return row * g->width + col;
}

int screen_geom_tile_count(const struct screen_geom *g)
{
    return g->width * g->height;
}

int screen_geom_pixel_width(const struct screen_geom *g)
{
    return g->width * SCREEN_TILE_SIZE;
}

int screen_geom_pixel_height(const struct screen_geom *g)
{
    return g->height * SCREEN_TILE_SIZE;
}

enum screen_facing screen_geom_facing_from_player(
    struct screen_pos c1, struct screen_pos c2,
    struct screen_pos player_pos)
{
    int dx = c2.x - c1.x;
    int dz = c2.z - c1.z;

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
    int dx = c2.x - c1.x;
    int dz = c2.z - c1.z;

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

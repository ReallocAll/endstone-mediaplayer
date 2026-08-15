#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_GEOMETRY_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_GEOMETRY_H

#include <stdint.h>

// MCV v1 remains limited to 7x4 tiles.  Screen geometry is a logical
// catalogue concept and deliberately has a larger bound; consumers that
// produce MCV files must continue to enforce the format's own limits.
#define SCREEN_MAX_WIDTH  7
#define SCREEN_MAX_HEIGHT 4
#define SCREEN_LOGICAL_MAX_WIDTH 1024
#define SCREEN_LOGICAL_MAX_HEIGHT 1024
#define SCREEN_TILE_SIZE  128

enum screen_facing {
    SCREEN_FACE_SOUTH = 0, // Front faces +Z.
    SCREEN_FACE_NORTH = 1, // Front faces -Z.
    SCREEN_FACE_EAST  = 2, // Front faces +X.
    SCREEN_FACE_WEST  = 3, // Front faces -X.
};

enum screen_geom_err {
    SCREEN_GEOM_OK = 0,
    SCREEN_GEOM_ERR_DIMENSION_MISMATCH,
    SCREEN_GEOM_ERR_NOT_VERTICAL,
    SCREEN_GEOM_ERR_WIDTH_EXCEED,
    SCREEN_GEOM_ERR_HEIGHT_EXCEED,
    SCREEN_GEOM_ERR_INVALID_FACING,
    SCREEN_GEOM_ERR_WIDTH_INVALID,
    SCREEN_GEOM_ERR_HEIGHT_INVALID,
};

const char *screen_geom_err_name(enum screen_geom_err error);

// Checks logical dimensions before they are used for tile or pixel arithmetic.
enum screen_geom_err screen_geom_validate_dimensions(int width, int height);

struct screen_pos {
    int x, y, z;
};

struct screen_geom {
    struct screen_pos corner1; // Minimum horizontal coordinate and maximum Y.
    struct screen_pos corner2; // Maximum horizontal coordinate and minimum Y.
    enum screen_facing facing;
    int width;
    int height;
    char dimension[64];
};

// Validates two corners and computes normalized screen geometry.
enum screen_geom_err screen_geom_validate(
    struct screen_pos c1, struct screen_pos c2,
    const char *dim1, const char *dim2,
    enum screen_facing facing,
    struct screen_geom *out);

// Returns the world position of a tile in viewer-relative order.
struct screen_pos screen_geom_tile_pos(const struct screen_geom *g, int col, int row);

// Returns the block behind a tile.
struct screen_pos screen_geom_backing_pos(const struct screen_geom *g, int col, int row);

// Returns row * width + col.
int screen_geom_tile_index(const struct screen_geom *g, int col, int row);

// Returns the total tile count.
int screen_geom_tile_count(const struct screen_geom *g);

// Returns the full screen dimensions in pixels.
int screen_geom_pixel_width(const struct screen_geom *g);
int screen_geom_pixel_height(const struct screen_geom *g);

// Determines facing from the player's position relative to the wall.
enum screen_facing screen_geom_facing_from_player(
    struct screen_pos c1, struct screen_pos c2,
    struct screen_pos player_pos);

// Returns the valid facings for the selected plane.
int screen_geom_facing_candidates(
    struct screen_pos c1, struct screen_pos c2,
    enum screen_facing out[4]);

const char *screen_facing_name(enum screen_facing f);

#endif // ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_GEOMETRY_H

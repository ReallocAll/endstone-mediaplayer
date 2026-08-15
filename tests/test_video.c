//
// Unit tests for the video screen system.
// Covers: screen geometry, video format, playback clock,
// screen registry, persistence, and map color.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mediaplayer/screen/screen_geometry.h"
#include "mediaplayer/screen/screen_registry.h"
#include "mediaplayer/screen/screen_persistence.h"
#include "mediaplayer/video/video_args.h"
#include "mediaplayer/video/video_format.h"
#include "mediaplayer/video/video_policy.h"
#include "mediaplayer/video/video_preferences.h"
#include "mediaplayer/video/video_session.h"
#include "mediaplayer/image/mps_catalog.h"
#include "mediaplayer/image/mps_source.h"
#include "mediaplayer/music/music_cache.h"
#include "mediaplayer/music/screen_audio.h"
#include "mediaplayer/map/map_render.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/bedrock/world_bridge.h"
#include "mediaplayer/bedrock/world_read_abi.h"
#include "mediaplayer/bedrock/world_write_abi.h"
#include "mediaplayer/api_provider.h"
#include "endstone_mediaplayer_api.h"
#include "endstone_abi.h"
#include <cppcompat/string.h>
#include "cJSON.h"
#include "miniz.h"
#include <stb_ds.h>

static int g_sound_count;
static void *g_last_sound_player;
static char g_last_sound[64];

FILE *fopen_utf8(const char *path, const char *mode)
{
    return fopen(path, mode);
}

void player_play_sound(void *player, const char *sound,
                       float volume, float pitch)
{
    (void)volume;
    (void)pitch;
    g_sound_count++;
    g_last_sound_player = player;
    snprintf(g_last_sound, sizeof(g_last_sound), "%s", sound);
}

// --- Minimal test framework (same style as test_nbs_parser.c) ---
static int g_pass = 0, g_fail = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 0; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    printf("  [TEST] %s\n", #fn); \
    if (fn()) { g_pass++; printf("  [PASS] %s\n", #fn); } \
    else { g_fail++; } \
} while(0)

// --- Fixture registry: EXPECT returns before a test's own remove(), so
// every on-disk fixture path is registered and swept once in main(). ---
static const char *g_fixture_paths[64];
static int g_fixture_path_count;

static const char *fixture(const char *path)
{
    for (int i = 0; i < g_fixture_path_count; i++) {
        if (strcmp(g_fixture_paths[i], path) == 0) return path;
    }
    if (g_fixture_path_count <
        (int)(sizeof(g_fixture_paths) / sizeof(g_fixture_paths[0]))) {
        g_fixture_paths[g_fixture_path_count++] = path;
    }
    return path;
}

static void remove_manifest_sidecar(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long length = ftell(file);
    if (length <= 0 || length > 10 * 1024 * 1024) {
        fclose(file);
        return;
    }
    rewind(file);
    char *data = calloc((size_t)length + 1, 1);
    if (!data) {
        fclose(file);
        return;
    }
    size_t read_count = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (read_count != (size_t)length) {
        free(data);
        return;
    }
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root)
        return;
    cJSON *sidecar = cJSON_GetObjectItemCaseSensitive(
        root, SCREEN_SIDECAR_FIELD);
    if (cJSON_IsString(sidecar) && sidecar->valuestring[0]) {
        const char *slash = strrchr(path, '/');
        const char *backslash = strrchr(path, '\\');
        const char *separator = slash;
        if (backslash && (!separator || backslash > separator))
            separator = backslash;
        size_t directory_length = separator
                                      ? (size_t)(separator - path + 1)
                                      : 0;
        size_t sidecar_length = strlen(sidecar->valuestring);
        char *sidecar_path = calloc(
            directory_length + sidecar_length + 1, 1);
        if (sidecar_path) {
            memcpy(sidecar_path, path, directory_length);
            memcpy(sidecar_path + directory_length, sidecar->valuestring,
                   sidecar_length);
            remove(sidecar_path);
            free(sidecar_path);
        }
    }
    cJSON_Delete(root);
}

static void remove_persistence_artifacts(const char *path)
{
    if (!path)
        return;
    remove_manifest_sidecar(path);
    remove(path);
    for (int suffix = 0; suffix <= 8; suffix++) {
        char bad_path[512];
        if (suffix == 0)
            snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
        else
            snprintf(bad_path, sizeof(bad_path), "%s.bad.%d", path, suffix);
        remove_manifest_sidecar(bad_path);
        remove(bad_path);
    }
}

static void remove_fixtures(void)
{
    for (int i = 0; i < g_fixture_path_count; i++) {
        remove_persistence_artifacts(g_fixture_paths[i]);
    }
}

static void test_put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void test_put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void test_put_u64(uint8_t *bytes, uint64_t value)
{
    test_put_u32(bytes, (uint32_t)value);
    test_put_u32(bytes + 4, (uint32_t)(value >> 32));
}

static int test_make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static void test_remove_directory(const char *path)
{
#if defined(_WIN32)
    _rmdir(path);
#else
    rmdir(path);
#endif
}

static int write_test_mps(const char *path, uint32_t width,
                          uint32_t height, int malformed)
{
    if (malformed) {
        FILE *bad = fopen(path, "wb");
        if (!bad) return 0;
        uint8_t header[MPS_HEADER_SIZE] = {0};
        int ok = fwrite(header, 1, sizeof(header), bad) == sizeof(header);
        fclose(bad);
        return ok;
    }
    uint64_t tile_count = (uint64_t)width * height;
    uint64_t data_offset = MPS_HEADER_SIZE +
                           tile_count * MPS_INDEX_ENTRY_SIZE;
    uint64_t file_size = data_offset + tile_count * MPS_TILE_BYTES;
    uint8_t header[MPS_HEADER_SIZE] = {0};
    memcpy(header, MPS_MAGIC, MPS_MAGIC_LEN);
    test_put_u16(header + 4, MPS_FORMAT_VERSION);
    test_put_u16(header + 6, MPS_HEADER_SIZE);
    test_put_u32(header + 16, width);
    test_put_u32(header + 20, height);
    test_put_u16(header + 24, MPS_TILE_PIXEL_SIZE);
    test_put_u16(header + 26, MPS_PIXFMT_ABGR8888);
    test_put_u16(header + 28, MPS_INDEX_ENTRY_SIZE);
    test_put_u64(header + 32, tile_count);
    test_put_u64(header + 40, MPS_HEADER_SIZE);
    test_put_u64(header + 48, data_offset);
    test_put_u64(header + 56, file_size);
    test_put_u32(header + 124,
                 (uint32_t)mz_crc32(MZ_CRC32_INIT, header,
                                    MPS_HEADER_SIZE - 4));

    uint8_t *tile = calloc(1, MPS_TILE_BYTES);
    if (!tile) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) {
        free(tile);
        return 0;
    }
    int ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
    for (uint64_t i = 0; ok && i < tile_count; i++) {
        memset(tile, (int)(i + 1), MPS_TILE_BYTES);
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, tile,
                                          MPS_TILE_BYTES);
        uint8_t entry[MPS_INDEX_ENTRY_SIZE] = {0};
        test_put_u64(entry, data_offset + i * MPS_TILE_BYTES);
        test_put_u32(entry + 8, MPS_TILE_BYTES);
        test_put_u32(entry + 12, MPS_TILE_BYTES);
        test_put_u32(entry + 16, crc);
        test_put_u32(entry + 20, crc);
        test_put_u16(entry + 24, MPS_CODEC_RAW);
        ok = fwrite(entry, 1, sizeof(entry), file) == sizeof(entry);
    }
    for (uint64_t i = 0; ok && i < tile_count; i++) {
        memset(tile, (int)(i + 1), MPS_TILE_BYTES);
        ok = fwrite(tile, 1, MPS_TILE_BYTES, file) == MPS_TILE_BYTES;
    }
    fclose(file);
    free(tile);
    return ok;
}

// ================================================================
// SCREEN GEOMETRY TESTS
// ================================================================

static int test_geom_xy_plane_south(void)
{
    struct screen_pos c1 = {0, 66, 5};
    struct screen_pos c2 = {3, 64, 5};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_OK, "validate should succeed");
    EXPECT(g.width == 4, "width should be 4");
    EXPECT(g.height == 3, "height should be 3");
    EXPECT(g.facing == SCREEN_FACE_SOUTH, "facing should be SOUTH");

    // Tile (0,0) = top-left from viewer = (minX, maxY) for SOUTH
    struct screen_pos t00 = screen_geom_tile_pos(&g, 0, 0);
    EXPECT(t00.x == 0 && t00.y == 66 && t00.z == 5, "tile(0,0) should be (0,66,5)");

    // Tile (3,0) = top-right = (maxX, maxY)
    struct screen_pos t30 = screen_geom_tile_pos(&g, 3, 0);
    EXPECT(t30.x == 3 && t30.y == 66 && t30.z == 5, "tile(3,0) should be (3,66,5)");

    // Tile (0,2) = bottom-left = (minX, minY)
    struct screen_pos t02 = screen_geom_tile_pos(&g, 0, 2);
    EXPECT(t02.x == 0 && t02.y == 64 && t02.z == 5, "tile(0,2) should be (0,64,5)");

    return 1;
}

static int test_geom_xy_plane_north(void)
{
    struct screen_pos c1 = {0, 66, 5};
    struct screen_pos c2 = {3, 64, 5};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_NORTH, &g);
    EXPECT(err == SCREEN_GEOM_OK, "validate should succeed");

    // NORTH: col0 = maxX, dir = -1
    struct screen_pos t00 = screen_geom_tile_pos(&g, 0, 0);
    EXPECT(t00.x == 3 && t00.y == 66 && t00.z == 5, "tile(0,0) NORTH should be (3,66,5)");

    struct screen_pos t30 = screen_geom_tile_pos(&g, 3, 0);
    EXPECT(t30.x == 0 && t30.y == 66 && t30.z == 5, "tile(3,0) NORTH should be (0,66,5)");

    return 1;
}

static int test_geom_zy_plane_east(void)
{
    struct screen_pos c1 = {10, 70, 0};
    struct screen_pos c2 = {10, 67, 6};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_EAST, &g);
    EXPECT(err == SCREEN_GEOM_OK, "validate should succeed");
    EXPECT(g.width == 7, "width should be 7");
    EXPECT(g.height == 4, "height should be 4");

    // EAST: col0 = maxZ, dir = -1
    struct screen_pos t00 = screen_geom_tile_pos(&g, 0, 0);
    EXPECT(t00.x == 10 && t00.y == 70 && t00.z == 6, "tile(0,0) EAST should be (10,70,6)");

    struct screen_pos t60 = screen_geom_tile_pos(&g, 6, 0);
    EXPECT(t60.x == 10 && t60.y == 70 && t60.z == 0, "tile(6,0) EAST should be (10,70,0)");

    struct screen_pos t03 = screen_geom_tile_pos(&g, 0, 3);
    EXPECT(t03.x == 10 && t03.y == 67 && t03.z == 6, "tile(0,3) EAST should be (10,67,6)");

    return 1;
}

static int test_geom_zy_plane_west(void)
{
    struct screen_pos c1 = {10, 70, 0};
    struct screen_pos c2 = {10, 67, 6};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_WEST, &g);
    EXPECT(err == SCREEN_GEOM_OK, "validate should succeed");

    // WEST: col0 = minZ, dir = +1
    struct screen_pos t00 = screen_geom_tile_pos(&g, 0, 0);
    EXPECT(t00.x == 10 && t00.y == 70 && t00.z == 0, "tile(0,0) WEST should be (10,70,0)");

    struct screen_pos t60 = screen_geom_tile_pos(&g, 6, 0);
    EXPECT(t60.x == 10 && t60.y == 70 && t60.z == 6, "tile(6,0) WEST should be (10,70,6)");

    return 1;
}

static int test_geom_reversed_corners(void)
{
    // Same wall, corners in opposite order - should produce same geometry
    struct screen_pos c1 = {3, 64, 5};
    struct screen_pos c2 = {0, 66, 5};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_OK, "reversed corners should succeed");
    EXPECT(g.width == 4, "width should be 4");
    EXPECT(g.height == 3, "height should be 3");

    // Tile ordering should be identical regardless of corner input order
    struct screen_pos t00 = screen_geom_tile_pos(&g, 0, 0);
    EXPECT(t00.x == 0 && t00.y == 66 && t00.z == 5, "tile(0,0) should be (0,66,5) regardless of corner order");

    return 1;
}

static int test_geom_1x1(void)
{
    struct screen_pos c1 = {5, 65, 5};
    struct screen_pos c2 = {5, 65, 5};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_OK, "1x1 should succeed");
    EXPECT(g.width == 1, "width should be 1");
    EXPECT(g.height == 1, "height should be 1");
    EXPECT(screen_geom_tile_count(&g) == 1, "tile count should be 1");
    EXPECT(screen_geom_pixel_width(&g) == 128, "pixel width should be 128");
    EXPECT(screen_geom_pixel_height(&g) == 128, "pixel height should be 128");
    return 1;
}

static int test_geom_7x4(void)
{
    struct screen_pos c1 = {0, 67, 0};
    struct screen_pos c2 = {6, 64, 0};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_OK, "7x4 should succeed");
    EXPECT(g.width == 7, "width should be 7");
    EXPECT(g.height == 4, "height should be 4");
    EXPECT(screen_geom_tile_count(&g) == 28, "tile count should be 28");
    EXPECT(screen_geom_pixel_width(&g) == 896, "pixel width should be 896");
    EXPECT(screen_geom_pixel_height(&g) == 512, "pixel height should be 512");
    return 1;
}

static int test_geom_checked_logical_limits(void)
{
    struct screen_geom g;
    EXPECT(screen_geom_validate_dimensions(0, 1) ==
               SCREEN_GEOM_ERR_WIDTH_INVALID,
           "zero width should be rejected by the explicit helper");
    EXPECT(screen_geom_validate_dimensions(1, 0) ==
               SCREEN_GEOM_ERR_HEIGHT_INVALID,
           "zero height should be rejected by the explicit helper");
    EXPECT(screen_geom_validate_dimensions(1025, 1) ==
               SCREEN_GEOM_ERR_WIDTH_EXCEED,
           "width 1025 should be rejected by the explicit helper");
    EXPECT(screen_geom_validate_dimensions(1, 1025) ==
               SCREEN_GEOM_ERR_HEIGHT_EXCEED,
           "height 1025 should be rejected by the explicit helper");
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 1023, 0},
               (struct screen_pos){1023, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &g) == SCREEN_GEOM_OK &&
               g.width == 1024 && g.height == 1024,
           "1024x1024 logical geometry should be accepted");
    EXPECT(screen_geom_validate(
               (struct screen_pos){INT_MIN, 0, 0},
               (struct screen_pos){INT_MAX, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &g) == SCREEN_GEOM_ERR_WIDTH_EXCEED,
           "extreme horizontal coordinates should be rejected safely");
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, INT_MAX, 0},
               (struct screen_pos){0, INT_MIN, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &g) == SCREEN_GEOM_ERR_HEIGHT_EXCEED,
           "extreme vertical coordinates should be rejected safely");
    return 1;
}

static int test_geom_width_exceed(void)
{
    struct screen_pos c1 = {0, 65, 0};
    struct screen_pos c2 = {1024, 65, 0}; // width = 1025
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_ERR_WIDTH_EXCEED, "width 1025 should fail");
    return 1;
}

static int test_geom_height_exceed(void)
{
    struct screen_pos c1 = {0, 69, 0};
    struct screen_pos c2 = {0, -955, 0}; // height = 1025
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_ERR_HEIGHT_EXCEED, "height 1025 should fail");
    return 1;
}

static int test_geom_not_vertical(void)
{
    struct screen_pos c1 = {0, 65, 0};
    struct screen_pos c2 = {3, 65, 3}; // diagonal - neither X nor Z constant
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_ERR_NOT_VERTICAL, "diagonal should fail");
    return 1;
}

static int test_geom_dimension_mismatch(void)
{
    struct screen_pos c1 = {0, 65, 0};
    struct screen_pos c2 = {3, 65, 0};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:nether",
                                                    SCREEN_FACE_SOUTH, &g);
    EXPECT(err == SCREEN_GEOM_ERR_DIMENSION_MISMATCH, "dimension mismatch should fail");
    return 1;
}

static int test_geom_facing_mismatch(void)
{
    // X-Y plane (constant Z) with EAST facing should fail
    struct screen_pos c1 = {0, 65, 5};
    struct screen_pos c2 = {3, 65, 5};
    struct screen_geom g;
    enum screen_geom_err err = screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld",
                                                    SCREEN_FACE_EAST, &g);
    EXPECT(err == SCREEN_GEOM_ERR_INVALID_FACING, "EAST facing on X-Y plane should fail");
    return 1;
}

static int test_geom_tile_index(void)
{
    struct screen_pos c1 = {0, 66, 0};
    struct screen_pos c2 = {2, 64, 0};
    struct screen_geom g;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &g);

    EXPECT(screen_geom_tile_index(&g, 0, 0) == 0, "index(0,0) = 0");
    EXPECT(screen_geom_tile_index(&g, 2, 0) == 2, "index(2,0) = 2");
    EXPECT(screen_geom_tile_index(&g, 0, 1) == 3, "index(0,1) = 3");
    EXPECT(screen_geom_tile_index(&g, 2, 2) == 8, "index(2,2) = 8");
    return 1;
}

static int test_geom_facing_from_player(void)
{
    struct screen_pos c1 = {0, 65, 5};
    struct screen_pos c2 = {3, 65, 5};

    struct screen_pos player_south = {1, 65, 8};
    EXPECT(screen_geom_facing_from_player(c1, c2, player_south) == SCREEN_FACE_SOUTH,
           "player at Z>5 should give SOUTH");

    struct screen_pos player_north = {1, 65, 2};
    EXPECT(screen_geom_facing_from_player(c1, c2, player_north) == SCREEN_FACE_NORTH,
           "player at Z<5 should give NORTH");

    // Z-Y plane
    struct screen_pos c3 = {10, 65, 0};
    struct screen_pos c4 = {10, 65, 5};

    struct screen_pos player_east = {12, 65, 2};
    EXPECT(screen_geom_facing_from_player(c3, c4, player_east) == SCREEN_FACE_EAST,
           "player at X>10 should give EAST");

    struct screen_pos player_west = {8, 65, 2};
    EXPECT(screen_geom_facing_from_player(c3, c4, player_west) == SCREEN_FACE_WEST,
           "player at X<10 should give WEST");

    return 1;
}

static int test_geom_backing_opposes_facing(void)
{
    struct screen_pos p = {10, 64, 20};
    struct screen_geom geometry;
    const enum screen_facing facings[] = {
        SCREEN_FACE_SOUTH, SCREEN_FACE_NORTH,
        SCREEN_FACE_EAST, SCREEN_FACE_WEST
    };
    const struct screen_pos expected[] = {
        {10, 64, 19}, {10, 64, 21},
        {9, 64, 20}, {11, 64, 20}
    };
    for (int i = 0; i < 4; i++) {
        EXPECT(screen_geom_validate(p, p, "minecraft:overworld",
                                    "minecraft:overworld", facings[i],
                                    &geometry) == SCREEN_GEOM_OK,
               "validate 1x1 backing geometry");
        struct screen_pos backing = screen_geom_backing_pos(&geometry, 0, 0);
        EXPECT(backing.x == expected[i].x && backing.y == expected[i].y &&
                   backing.z == expected[i].z,
               "backing position is opposite outward facing");
    }
    return 1;
}

static int test_geom_facing_candidates_from_plane(void)
{
    enum screen_facing candidates[4] = {0};

    struct screen_pos xy1 = {0, 66, 5};
    struct screen_pos xy2 = {3, 64, 5};
    int count = screen_geom_facing_candidates(xy1, xy2, candidates);
    EXPECT(count == 2, "X-Y plane should have two facing candidates");
    EXPECT(candidates[0] == SCREEN_FACE_SOUTH &&
               candidates[1] == SCREEN_FACE_NORTH,
           "X-Y candidates should be south/north");

    struct screen_pos zy1 = {10, 66, 0};
    struct screen_pos zy2 = {10, 64, 3};
    count = screen_geom_facing_candidates(zy1, zy2, candidates);
    EXPECT(count == 2, "Z-Y plane should have two facing candidates");
    EXPECT(candidates[0] == SCREEN_FACE_EAST &&
               candidates[1] == SCREEN_FACE_WEST,
           "Z-Y candidates should be east/west");

    struct screen_pos one = {10, 64, 20};
    count = screen_geom_facing_candidates(one, one, candidates);
    EXPECT(count == 4, "1x1 should probe all four horizontal sides");
    EXPECT(candidates[0] == SCREEN_FACE_SOUTH &&
               candidates[1] == SCREEN_FACE_NORTH &&
               candidates[2] == SCREEN_FACE_EAST &&
               candidates[3] == SCREEN_FACE_WEST,
           "1x1 candidate order should be stable");

    struct screen_pos diagonal = {11, 64, 21};
    EXPECT(screen_geom_facing_candidates(one, diagonal, candidates) == 0,
           "diagonal selection should have no facing candidate");
    return 1;
}

static int test_status_error_names(void)
{
    const enum screen_geom_err geometry_errors[] = {
        SCREEN_GEOM_OK,
        SCREEN_GEOM_ERR_DIMENSION_MISMATCH,
        SCREEN_GEOM_ERR_NOT_VERTICAL,
        SCREEN_GEOM_ERR_WIDTH_EXCEED,
        SCREEN_GEOM_ERR_HEIGHT_EXCEED,
        SCREEN_GEOM_ERR_INVALID_FACING,
    };
    const char *const geometry_names[] = {
        "ok",
        "dimension mismatch",
        "screen is not vertical",
        "screen width exceeds limit",
        "screen height exceeds limit",
        "invalid screen facing",
    };
    for (size_t i = 0;
         i < sizeof(geometry_errors) / sizeof(geometry_errors[0]); i++) {
        EXPECT(strcmp(screen_geom_err_name(geometry_errors[i]),
                      geometry_names[i]) == 0,
               "geometry error name should match");
    }
    EXPECT(strcmp(screen_geom_err_name((enum screen_geom_err)-1),
                  "unknown screen geometry error") == 0,
           "unknown geometry error should have a fallback name");

    const enum screen_error registry_errors[] = {
        SCREEN_OK,
        SCREEN_ERR_NOT_FOUND,
        SCREEN_ERR_NAME_EXISTS,
        SCREEN_ERR_NAME_INVALID,
        SCREEN_ERR_FULL,
    };
    const char *const registry_names[] = {
        "ok",
        "screen not found",
        "screen name already exists",
        "invalid screen name",
        "screen registry is full",
    };
    for (size_t i = 0;
         i < sizeof(registry_errors) / sizeof(registry_errors[0]); i++) {
        EXPECT(strcmp(screen_error_name(registry_errors[i]),
                      registry_names[i]) == 0,
               "registry error name should match");
    }
    EXPECT(strcmp(screen_error_name((enum screen_error)-1),
                  "unknown screen error") == 0,
           "unknown registry error should have a fallback name");
    return 1;
}

// ================================================================
// VIDEO FORMAT TESTS (MCV v1)
// ================================================================

static void write_u16_le(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void write_u32_le(FILE *f, uint32_t v) {
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}
static void write_u64_le(FILE *f, uint64_t v) {
    write_u32_le(f, (uint32_t)v);
    write_u32_le(f, (uint32_t)(v >> 32));
}

static void store_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}
static void store_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void store_u64_le(uint8_t *p, uint64_t v) {
    store_u32_le(p, (uint32_t)v);
    store_u32_le(p + 4, (uint32_t)(v >> 32));
}

// Build a fully valid 128-byte MCV header, trailing CRC included.
static void build_mcv_header(uint8_t out[MCV_HEADER_SIZE], uint16_t version,
                             uint16_t tile_w, uint16_t tile_h,
                             uint16_t pixel_w, uint16_t pixel_h,
                             uint16_t fps_num, uint16_t fps_den,
                             uint16_t codec, uint64_t frame_count,
                             uint64_t data_size, uint64_t data_offset,
                             uint64_t index_offset, uint32_t entry_size)
{
    memset(out, 0, MCV_HEADER_SIZE);
    memcpy(out, MCV_MAGIC, MCV_MAGIC_LEN);
    store_u16_le(out + 4, MCV_HEADER_SIZE);
    store_u16_le(out + 6, version);
    // required_flags, optional_flags, pixel_format stay zero
    store_u16_le(out + 16, tile_w);
    store_u16_le(out + 18, tile_h);
    store_u16_le(out + 20, pixel_w);
    store_u16_le(out + 22, pixel_h);
    store_u16_le(out + 26, codec);
    store_u16_le(out + 28, fps_num);
    store_u16_le(out + 30, fps_den);
    store_u64_le(out + 32, frame_count);
    store_u64_le(out + 40, data_offset);
    store_u64_le(out + 48, data_size);
    store_u64_le(out + 56, index_offset);
    store_u32_le(out + 64, entry_size);
    store_u32_le(out + 124,
                 (uint32_t)mz_crc32(MZ_CRC32_INIT, out, MCV_HEADER_SIZE - 4));
}

static void write_mcv_header(FILE *f, uint16_t version,
                             uint16_t tile_w, uint16_t tile_h,
                             uint16_t pixel_w, uint16_t pixel_h,
                             uint16_t fps_num, uint16_t fps_den,
                             uint16_t codec, uint64_t frame_count,
                             uint64_t data_size, uint64_t data_offset,
                             uint64_t index_offset, uint32_t entry_size)
{
    uint8_t header[MCV_HEADER_SIZE];
    build_mcv_header(header, version, tile_w, tile_h, pixel_w, pixel_h,
                     fps_num, fps_den, codec, frame_count, data_size,
                     data_offset, index_offset, entry_size);
    fwrite(header, 1, sizeof(header), f);
}

static void write_zero_bytes(FILE *f, uint64_t count)
{
    const uint8_t zeros[4096] = {0};
    while (count > 0) {
        size_t chunk = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count;
        fwrite(zeros, 1, chunk, f);
        count -= chunk;
    }
}

static void patch_u8(const char *path, long offset, uint8_t value)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, offset, SEEK_SET);
    fputc(value, f);
    fclose(f);
}

static void patch_u16(const char *path, long offset, uint16_t value)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, offset, SEEK_SET);
    write_u16_le(f, value);
    fclose(f);
}

static void patch_u32(const char *path, long offset, uint32_t value)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, offset, SEEK_SET);
    write_u32_le(f, value);
    fclose(f);
}

static void patch_u64(const char *path, long offset, uint64_t value)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, offset, SEEK_SET);
    write_u64_le(f, value);
    fclose(f);
}

// Recompute the header CRC after a test deliberately patched a header
// field, so the specific validation error surfaces instead of CRC.
static void fix_header_crc(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    uint8_t header[MCV_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, header,
                                          MCV_HEADER_SIZE - 4);
        fseek(f, MCV_HEADER_SIZE - 4, SEEK_SET);
        write_u32_le(f, crc);
    }
    fclose(f);
}

static void write_index_entry(FILE *f, uint64_t offset, uint64_t size,
                              uint32_t crc, uint16_t flags, uint16_t reserved)
{
    write_u64_le(f, offset);
    write_u64_le(f, size);
    write_u32_le(f, crc);
    write_u16_le(f, flags);
    write_u16_le(f, reserved);
}

// Pattern frame: R=x%256, G=y%256, B=frame%256, A=255 (ABGR bytes).
static void fill_test_frame(uint8_t *buf, int pw, int ph, uint32_t frame_idx)
{
    for (size_t p = 0; p < (size_t)pw * ph; p++) {
        int x = (int)(p % pw), y = (int)(p / pw);
        buf[p * 4 + 0] = (uint8_t)(x % 256);
        buf[p * 4 + 1] = (uint8_t)(y % 256);
        buf[p * 4 + 2] = (uint8_t)(frame_idx % 256);
        buf[p * 4 + 3] = 255;
    }
}

// Create a minimal valid uncompressed .mcv file.
static int create_test_mcv(int tile_w, int tile_h, int fps_num, int fps_den,
                           uint32_t frame_count, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    int pw = tile_w * 128, ph = tile_h * 128;
    size_t frame_size = (size_t)pw * ph * 4;
    uint64_t data_size = (uint64_t)frame_size * frame_count;

    uint8_t *frame = malloc(frame_size);
    uint32_t *crcs = malloc((size_t)frame_count * sizeof(uint32_t));
    if (!frame || !crcs) {
        free(frame); free(crcs); fclose(f);
        return 0;
    }

    write_mcv_header(f, MCV_FORMAT_VERSION, (uint16_t)tile_w,
                     (uint16_t)tile_h, (uint16_t)pw, (uint16_t)ph,
                     (uint16_t)fps_num, (uint16_t)fps_den, MCV_CODEC_NONE,
                     frame_count, data_size, MCV_HEADER_SIZE,
                     MCV_HEADER_SIZE + data_size,
                     MCV_FRAME_INDEX_ENTRY_SIZE);

    for (uint32_t i = 0; i < frame_count; i++) {
        fill_test_frame(frame, pw, ph, i);
        crcs[i] = (uint32_t)mz_crc32(MZ_CRC32_INIT, frame, frame_size);
        fwrite(frame, 1, frame_size, f);
    }
    for (uint32_t i = 0; i < frame_count; i++) {
        write_index_entry(f, (uint64_t)i * frame_size, frame_size, crcs[i],
                          MCV_FRAME_FLAG_INDEPENDENT, 0);
    }

    free(frame);
    free(crcs);
    fclose(f);
    return 1;
}

// Create a zlib-codec .mcv with pattern frames; optionally reports the
// stored extent of frame 0 for corruption tests.
static int create_test_mcv_zlib(int tile_w, int tile_h, uint32_t frame_count,
                                const char *path, uint64_t *out_first_offset,
                                uint64_t *out_first_size)
{
    int pw = tile_w * 128, ph = tile_h * 128;
    size_t frame_size = (size_t)pw * ph * 4;

    uint8_t *frame = malloc(frame_size);
    uint8_t **stored = calloc(frame_count, sizeof(*stored));
    mz_ulong *stored_size = calloc(frame_count, sizeof(*stored_size));
    uint32_t *crcs = calloc(frame_count, sizeof(*crcs));
    int ok = 0;
    FILE *f = nullptr;

    if (!frame || !stored || !stored_size || !crcs)
        goto cleanup;

    uint64_t data_size = 0;
    for (uint32_t i = 0; i < frame_count; i++) {
        fill_test_frame(frame, pw, ph, i);
        mz_ulong bound = mz_compressBound((mz_ulong)frame_size);
        stored[i] = malloc(bound);
        if (!stored[i])
            goto cleanup;
        stored_size[i] = bound;
        if (mz_compress2(stored[i], &stored_size[i], frame,
                         (mz_ulong)frame_size, 6) != MZ_OK)
            goto cleanup;
        crcs[i] = (uint32_t)mz_crc32(MZ_CRC32_INIT, stored[i],
                                     stored_size[i]);
        data_size += stored_size[i];
    }

    f = fopen(path, "wb");
    if (!f)
        goto cleanup;
    write_mcv_header(f, MCV_FORMAT_VERSION, (uint16_t)tile_w,
                     (uint16_t)tile_h, (uint16_t)pw, (uint16_t)ph,
                     20, 1, MCV_CODEC_ZLIB, frame_count, data_size,
                     MCV_HEADER_SIZE, MCV_HEADER_SIZE + data_size,
                     MCV_FRAME_INDEX_ENTRY_SIZE);
    for (uint32_t i = 0; i < frame_count; i++)
        fwrite(stored[i], 1, stored_size[i], f);
    uint64_t offset = 0;
    for (uint32_t i = 0; i < frame_count; i++) {
        write_index_entry(f, offset, stored_size[i], crcs[i],
                          MCV_FRAME_FLAG_INDEPENDENT, 0);
        offset += stored_size[i];
    }
    if (out_first_offset) *out_first_offset = 0;
    if (out_first_size) *out_first_size = stored_size[0];
    ok = 1;

cleanup:
    if (f) fclose(f);
    if (stored) {
        for (uint32_t i = 0; i < frame_count; i++)
            free(stored[i]);
    }
    free(stored);
    free(stored_size);
    free(crcs);
    free(frame);
    return ok;
}

// zlib-codec fixture whose index entries are supplied by the test
// (possibly hostile); the data region is zero filler.
static int create_ref_fixture(const char *path, uint64_t frame_count,
                              uint64_t data_size,
                              const struct mcv_frame_ref *refs,
                              uint32_t refs_to_write)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    write_mcv_header(f, MCV_FORMAT_VERSION, 1, 1, 128, 128, 20, 1,
                     MCV_CODEC_ZLIB, frame_count, data_size,
                     MCV_HEADER_SIZE, MCV_HEADER_SIZE + data_size,
                     MCV_FRAME_INDEX_ENTRY_SIZE);
    write_zero_bytes(f, data_size);
    for (uint32_t i = 0; i < refs_to_write; i++) {
        write_index_entry(f, refs[i].offset, refs[i].size, refs[i].crc32,
                          refs[i].flags, refs[i].reserved);
    }
    fclose(f);
    return 1;
}

static int test_mcv_valid_1x1(void)
{
    const char *path = fixture("test_1x1.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 3, path), "create fixture");

    struct mcv_file mf;
    enum mcv_error err = mcv_open(path, &mf);
    EXPECT(err == MCV_OK, "open should succeed");
    EXPECT(mf.header.tile_width == 1, "tile_width = 1");
    EXPECT(mf.header.tile_height == 1, "tile_height = 1");
    EXPECT(mf.header.pixel_width == 128, "pixel_width = 128");
    EXPECT(mf.header.pixel_height == 128, "pixel_height = 128");
    EXPECT(mf.header.frame_count == 3, "frame_count = 3");
    EXPECT(mf.header.fps_num == 20, "fps_num = 20");
    EXPECT(mf.header.codec == MCV_CODEC_NONE, "codec = none");

    // Read frame 0
    uint8_t *buf = malloc(128 * 128 * 4);
    EXPECT(buf != nullptr, "alloc");
    err = mcv_read_frame(&mf, 0, buf, 128 * 128 * 4);
    EXPECT(err == MCV_OK, "read frame 0");

    // Check pixel (0,0): R=0, G=0, B=0, A=255
    EXPECT(buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 255, "pixel(0,0) frame0");

    // Check pixel (1,0): R=1, G=0, B=0, A=255
    EXPECT(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 255, "pixel(1,0) frame0");

    // Read frame 2
    err = mcv_read_frame(&mf, 2, buf, 128 * 128 * 4);
    EXPECT(err == MCV_OK, "read frame 2");
    // pixel(0,0) frame2: R=0, G=0, B=2, A=255
    EXPECT(buf[0] == 0 && buf[1] == 0 && buf[2] == 2 && buf[3] == 255, "pixel(0,0) frame2");

    free(buf);
    mcv_close(&mf);
    remove(path);
    return 1;
}

static int test_mcv_valid_7x4(void)
{
    const char *path = fixture("test_7x4.mcv");
    EXPECT(create_test_mcv(7, 4, 10, 1, 2, path), "create fixture");

    struct mcv_file mf;
    enum mcv_error err = mcv_open(path, &mf);
    EXPECT(err == MCV_OK, "open 7x4 should succeed");
    EXPECT(mf.header.pixel_width == 896, "pixel_width = 896");
    EXPECT(mf.header.pixel_height == 512, "pixel_height = 512");
    EXPECT(mcv_dimensions_match(&mf, 7, 4), "dimensions match 7x4");
    EXPECT(!mcv_dimensions_match(&mf, 4, 2), "dimensions should not match 4x2");

    mcv_close(&mf);
    remove(path);
    return 1;
}

static int test_mcv_bad_magic(void)
{
    const char *path = fixture("test_badmagic.mcv");
    FILE *f = fopen(path, "wb");
    fwrite("XXXX", 1, 4, f);
    for (int i = 0; i < 124; i++) fputc(0, f);
    fclose(f);

    struct mcv_file mf;
    enum mcv_error err = mcv_open(path, &mf);
    EXPECT(err == MCV_ERR_MAGIC, "bad magic should fail");
    remove(path);

    // Other magic values are rejected.
    f = fopen(path, "wb");
    fwrite("MPV\x01", 1, 4, f);
    for (int i = 0; i < 124; i++) fputc(0, f);
    fclose(f);
    err = mcv_open(path, &mf);
    EXPECT(err == MCV_ERR_MAGIC, "other magic is rejected");
    EXPECT(strcmp(mcv_error_name(err), "invalid MCV magic") == 0,
           "magic error is concise");
    remove(path);
    return 1;
}

static int test_mcv_bad_version(void)
{
    const char *path = fixture("test_badver.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u16(path, 6, 99);
    fix_header_crc(path);

    struct mcv_file mf;
    enum mcv_error err = mcv_open(path, &mf);
    EXPECT(err == MCV_ERR_VERSION, "bad version should fail");
    remove(path);
    return 1;
}

static int test_mcv_truncated_header(void)
{
    const char *path = fixture("test_trunc.mcv");
    FILE *f = fopen(path, "wb");
    fwrite(MCV_MAGIC, 1, 4, f);
    write_u16_le(f, MCV_HEADER_SIZE);
    fclose(f); // only 6 bytes

    struct mcv_file mf;
    enum mcv_error err = mcv_open(path, &mf);
    EXPECT(err == MCV_ERR_HEADER_TRUNCATED, "truncated header should fail");
    remove(path);
    return 1;
}

static int test_mcv_header_crc(void)
{
    const char *path = fixture("test_header_crc.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    // Patch a field WITHOUT fixing the CRC: the CRC gate must fire
    // before any field-specific validation.
    patch_u16(path, 16, 2);

    struct mcv_file mf;
    EXPECT(mcv_open(path, &mf) == MCV_ERR_HEADER_CRC,
           "corrupted header is caught by the header CRC");
    remove(path);
    return 1;
}

static int test_mcv_flags_policy(void)
{
    const char *path = fixture("test_flags_policy.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u32(path, 8, 1); // unknown required feature bit
    fix_header_crc(path);
    struct mcv_file mf;
    EXPECT(mcv_open(path, &mf) == MCV_ERR_UNSUPPORTED_FEATURE,
           "unknown required flag is rejected");
    remove(path);

    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u32(path, 12, 0xFFFFFFFFu); // unknown optional bits
    fix_header_crc(path);
    EXPECT(mcv_open(path, &mf) == MCV_OK,
           "unknown optional flags are ignored");
    EXPECT(mf.header.optional_flags == 0xFFFFFFFFu,
           "optional flags are surfaced to the caller");
    mcv_close(&mf);
    remove(path);

    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u32(path, 68, 1); // reserved area must stay zero
    fix_header_crc(path);
    EXPECT(mcv_open(path, &mf) == MCV_ERR_INVALID_HEADER,
           "nonzero reserved area is rejected");
    remove(path);
    return 1;
}

static int test_mcv_frame_duration(void)
{
    const char *path = fixture("test_dur.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");

    struct mcv_file mf;
    mcv_open(path, &mf);
    double dur = mcv_frame_duration_ms(&mf);
    EXPECT(dur > 49.9 && dur < 50.1, "20fps should give ~50ms per frame");

    double total = mcv_total_duration_ms(&mf);
    EXPECT(total > 49.9 && total < 50.1, "1 frame at 20fps = ~50ms total");

    mcv_close(&mf);
    remove(path);
    return 1;
}

static int test_mcv_multiple_uncompressed_frames(void)
{
    const char *path = fixture("test_multi_frames.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 4, path), "create MCV v1 fixture");
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK, "open multi-frame MCV v1");
    EXPECT(file.header.frame_data_offset == 128 &&
               file.header.frame_index_offset ==
                   128 + (uint64_t)4 * 128 * 128 * 4 &&
               file.header.frame_index_entry_size == 24,
           "data-first offsets match MCV v1");
    uint8_t *frame = malloc(128 * 128 * 4);
    EXPECT(frame != nullptr, "allocate frame buffer");
    EXPECT(mcv_read_frame(&file, 0, frame, 128 * 128 * 4) == MCV_OK &&
               frame[2] == 0,
           "read first frame");
    EXPECT(mcv_read_frame(&file, 3, frame, 128 * 128 * 4) == MCV_OK &&
               frame[2] == 3,
           "read last frame");
    EXPECT(mcv_read_frame(&file, 0, frame, 16) == MCV_ERR_FRAME_SIZE_MISMATCH,
           "reject undersized destination buffer");
    free(frame);
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_frame_reference_decode(void)
{
    uint8_t encoded[MCV_FRAME_INDEX_ENTRY_SIZE] = {0};
    uint64_t offset = UINT64_C(0x100000123);
    uint64_t size = UINT64_C(0x200000456);
    for (int i = 0; i < 8; i++) {
        encoded[i] = (uint8_t)(offset >> (i * 8));
        encoded[8 + i] = (uint8_t)(size >> (i * 8));
    }
    encoded[16] = 0xDD; encoded[17] = 0xCC;
    encoded[18] = 0xBB; encoded[19] = 0xAA;
    encoded[20] = 0x01; // independent
    struct mcv_frame_ref ref = {0};
    mcv_decode_frame_ref(encoded, &ref);
    EXPECT(sizeof(ref.offset) == 8 && sizeof(ref.size) == 8,
           "in-memory frame references are uint64");
    EXPECT(ref.offset == offset && ref.size == size,
           "uint64 index values above UINT32_MAX decode without truncation");
    EXPECT(ref.crc32 == 0xAABBCCDDu, "frame CRC decodes little-endian");
    EXPECT(ref.flags == MCV_FRAME_FLAG_INDEPENDENT && ref.reserved == 0,
           "frame flags and reserved decode");

    const char *path = fixture("test_uint64_region.mcv");
    FILE *f = fopen(path, "wb");
    uint64_t large_data = UINT64_C(0x100000123);
    write_mcv_header(f, 1, 1, 1, 128, 128, 20, 1, MCV_CODEC_ZLIB, 1,
                     large_data, 128, 128 + large_data, 24);
    fclose(f);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_ERR_FRAME_SIZE_MISMATCH,
           "declared region above UINT32_MAX is decoded in 64 bits then "
           "rejected by the per-frame size accounting");
    remove(path);
    return 1;
}

static int test_mcv_invalid_data_offset(void)
{
    const char *path = fixture("test_bad_data_offset.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u64(path, 40, 129);
    fix_header_crc(path);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_ERR_INVALID_HEADER,
           "frame_data_offset must be exactly 128");
    remove(path);
    return 1;
}

static int test_mcv_invalid_index_offset(void)
{
    const char *path = fixture("test_bad_index_offset.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u64(path, 56, 129);
    fix_header_crc(path);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_ERR_INVALID_HEADER,
           "frame_index_offset must equal data offset plus data size");
    remove(path);
    return 1;
}

static int test_mcv_invalid_index_entry_size(void)
{
    const char *path = fixture("test_bad_entry_size.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u32(path, 64, 16);
    fix_header_crc(path);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_ERR_INVALID_HEADER,
           "MCV v1 index entries must be 24 bytes");
    remove(path);
    return 1;
}

static int test_mcv_data_index_overlap(void)
{
    const char *path = fixture("test_overlap_regions.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u64(path, 56, MCV_HEADER_SIZE + 32);
    fix_header_crc(path);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_ERR_INVALID_HEADER,
           "overlapping data and index regions are rejected");
    remove(path);
    return 1;
}

static int test_mcv_truncated_data_and_index(void)
{
    const uint64_t frame_size = 128 * 128 * 4;
    const char *data_path = fixture("test_truncated_data.mcv");
    FILE *f = fopen(data_path, "wb");
    write_mcv_header(f, 1, 1, 1, 128, 128, 20, 1, MCV_CODEC_NONE, 1,
                     frame_size, 128, 128 + frame_size, 24);
    fclose(f);
    struct mcv_file file;
    EXPECT(mcv_open(data_path, &file) == MCV_ERR_DATA_TRUNCATED,
           "missing declared frame data is rejected");
    remove(data_path);

    const char *index_path = fixture("test_truncated_index.mcv");
    f = fopen(index_path, "wb");
    write_mcv_header(f, 1, 1, 1, 128, 128, 20, 1, MCV_CODEC_NONE, 1,
                     frame_size, 128, 128 + frame_size, 24);
    write_zero_bytes(f, frame_size);
    write_u64_le(f, 0); // only a third of an index entry
    fclose(f);
    EXPECT(mcv_open(index_path, &file) == MCV_ERR_INDEX_TRUNCATED,
           "partial MCV v1 index is rejected");
    remove(index_path);
    return 1;
}

static int test_mcv_checked_arithmetic_overflow(void)
{
    uint64_t value = 123;
    EXPECT(!mcv_u64_mul_checked(UINT64_MAX, 24, &value),
           "index-size multiplication overflow is detected");
    EXPECT(!mcv_u64_add_checked(UINT64_MAX, 1, &value),
           "frame offset plus size overflow is detected");
    EXPECT(mcv_u64_mul_checked(MCV_MAX_FRAME_COUNT, 24, &value) &&
               value == UINT64_C(144000000),
           "valid index-size multiplication succeeds");
    return 1;
}

static int test_mcv_read_range_failures(void)
{
    struct mcv_file file;
    uint8_t buf[128 * 128 * 4];

    const char *overflow_path = fixture("test_ref_overflow.mcv");
    struct mcv_frame_ref overflow_ref = {UINT64_MAX, 2, 0,
                                         MCV_FRAME_FLAG_INDEPENDENT, 0};
    EXPECT(create_ref_fixture(overflow_path, 1, 1, &overflow_ref, 1), "create fixture");
    EXPECT(mcv_open(overflow_path, &file) == MCV_OK,
           "open is O(1) and accepts a not-yet-read hostile index");
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) == MCV_ERR_OVERFLOW,
           "frame offset plus size overflow is rejected at read time");
    mcv_close(&file);
    remove(overflow_path);

    const char *outside_path = fixture("test_ref_outside.mcv");
    struct mcv_frame_ref outside_ref = {0, 5, 0,
                                        MCV_FRAME_FLAG_INDEPENDENT, 0};
    EXPECT(create_ref_fixture(outside_path, 1, 4, &outside_ref, 1), "create fixture");
    EXPECT(mcv_open(outside_path, &file) == MCV_OK,
           "open accepts the header while the entry is still unread");
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
               MCV_ERR_DATA_TRUNCATED,
           "frame outside frame_data_size is rejected at read time");
    mcv_close(&file);
    remove(outside_path);
    return 1;
}

static int test_mcv_unknown_codec(void)
{
    const char *path = fixture("test_unknown_codec.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u16(path, 26, 99);
    fix_header_crc(path);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_ERR_INVALID_CODEC,
           "unknown codec is rejected during open");
    remove(path);
    return 1;
}

static int test_mcv_wrong_uncompressed_stored_size(void)
{
    const char *path = fixture("test_wrong_stored_size.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    // Entry 0 size field lives at index_offset + 8.
    patch_u64(path, (long)(MCV_HEADER_SIZE + 128 * 128 * 4 + 8),
              128 * 128 * 4 - 1);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK, "open stays O(1)");
    uint8_t buf[128 * 128 * 4];
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
               MCV_ERR_FRAME_SIZE_MISMATCH,
           "wrong uncompressed stored size is rejected at read time");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_uncompressed_layout_enforced(void)
{
    const char *path = fixture("test_uncompressed_layout.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 2, path), "create fixture");
    // Point frame 1 back at frame 0: uncompressed layout is fully
    // determined, so a non-canonical offset must be rejected.
    const uint64_t frame_size = 128 * 128 * 4;
    patch_u64(path, (long)(MCV_HEADER_SIZE + 2 * frame_size + 24), 0);
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK, "open stays O(1)");
    uint8_t buf[128 * 128 * 4];
    EXPECT(mcv_read_frame(&file, 1, buf, sizeof(buf)) ==
               MCV_ERR_FRAME_SIZE_MISMATCH,
           "overlapping uncompressed frame offset is rejected at read time");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_tile_pixel_and_fps_validation(void)
{
    const char *tile_path = fixture("test_tile_pixel_mismatch.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, tile_path), "create fixture");
    patch_u16(tile_path, 20, 129);
    fix_header_crc(tile_path);
    struct mcv_file file;
    EXPECT(mcv_open(tile_path, &file) == MCV_ERR_INVALID_DIMENSIONS,
           "tile-to-pixel mismatch is rejected");
    remove(tile_path);

    const char *fps_path = fixture("test_bad_fps_den.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, fps_path), "create fixture");
    patch_u16(fps_path, 30, 0);
    fix_header_crc(fps_path);
    EXPECT(mcv_open(fps_path, &file) == MCV_ERR_INVALID_DIMENSIONS,
           "zero FPS denominator is rejected");
    remove(fps_path);

    const char *fast_path = fixture("test_fps_too_fast.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, fast_path), "create fixture");
    patch_u16(fast_path, 28, 21);
    fix_header_crc(fast_path);
    EXPECT(mcv_open(fast_path, &file) == MCV_ERR_INVALID_DIMENSIONS,
           "FPS above the 20fps ceiling is rejected");
    remove(fast_path);
    return 1;
}

static int test_mcv_close_clears_state(void)
{
    const char *path = fixture("test_close_clear.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK && file.fp,
           "open state before close");
    mcv_close(&file);
    struct mcv_file zero = {0};
    EXPECT(memcmp(&file, &zero, sizeof(file)) == 0,
           "mcv_close frees and clears all state");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_frame_crc_mismatch(void)
{
    const char *path = fixture("test_frame_crc.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u8(path, MCV_HEADER_SIZE + 100, 0xFF); // corrupt pixel data
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK,
           "open does not touch frame data");
    uint8_t buf[128 * 128 * 4];
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) == MCV_ERR_FRAME_CRC,
           "corrupted frame data is caught by the frame CRC");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_zlib_frame_crc_mismatch(void)
{
    const char *path = fixture("test_zlib_frame_crc.mcv");
    EXPECT(create_test_mcv_zlib(1, 1, 1, path, nullptr, nullptr),
           "create zlib CRC fixture");
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK, "open zlib CRC fixture");
    uint64_t index_offset = file.header.frame_index_offset;
    uint8_t buf[128 * 128 * 4];
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) == MCV_OK,
           "valid compressed frame decodes before CRC corruption");
    uint32_t original_crc = file.cache[0].crc32;
    mcv_close(&file);

    // Leave the compressed stream intact and corrupt only its stored CRC.
    patch_u32(path, (long)(index_offset + 16), original_crc ^ UINT32_C(1));
    EXPECT(mcv_open(path, &file) == MCV_OK &&
               mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
                   MCV_ERR_FRAME_CRC,
           "compressed stored CRC mismatch is reported before decompression");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_utf8_filename(void)
{
    const char *path = fixture("test_\xC3\xA9.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path),
           "create UTF-8 filename fixture");
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK,
           "UTF-8 MCV filename opens through fopen_utf8");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_frame_flags_validation(void)
{
    const char *path = fixture("test_frame_flags.mcv");
    const long flags_offset = (long)(MCV_HEADER_SIZE + 128 * 128 * 4 + 20);
    struct mcv_file file;
    uint8_t buf[128 * 128 * 4];

    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create fixture");
    patch_u16(path, flags_offset, 0); // independent bit cleared
    EXPECT(mcv_open(path, &file) == MCV_OK, "open stays O(1)");
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) == MCV_ERR_FRAME_FLAGS,
           "non-independent frame is rejected in v1");
    mcv_close(&file);

    patch_u16(path, flags_offset, 0x0003); // unknown flag bit
    EXPECT(mcv_open(path, &file) == MCV_OK &&
               mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
                   MCV_ERR_FRAME_FLAGS,
           "unknown frame flag bits are rejected");
    mcv_close(&file);

    patch_u16(path, flags_offset, MCV_FRAME_FLAG_INDEPENDENT);
    patch_u16(path, flags_offset + 2, 1); // reserved must stay zero
    EXPECT(mcv_open(path, &file) == MCV_OK &&
               mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
                   MCV_ERR_FRAME_FLAGS,
           "nonzero entry reserved field is rejected");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_zlib_roundtrip(void)
{
    const char *path = fixture("test_zlib_roundtrip.mcv");
    uint64_t first_offset = 0, first_size = 0;
    EXPECT(create_test_mcv_zlib(1, 1, 4, path, &first_offset, &first_size),
           "create zlib fixture");
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK, "open zlib MCV");
    EXPECT(file.header.codec == MCV_CODEC_ZLIB, "codec = zlib");
    uint8_t buf[128 * 128 * 4];
    EXPECT(mcv_read_frame(&file, 0, buf, sizeof(buf)) == MCV_OK &&
               buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 255 &&
               buf[4] == 1,
           "zlib frame 0 decompresses to the pattern");
    EXPECT(mcv_read_frame(&file, 3, buf, sizeof(buf)) == MCV_OK &&
               buf[2] == 3,
           "zlib frame 3 decompresses to the pattern");
    mcv_close(&file);

    // Corrupting a stored byte must fail the stored-byte CRC gate first.
    patch_u8(path, (long)(MCV_HEADER_SIZE + first_offset + first_size / 2),
             0xFF);
    EXPECT(mcv_open(path, &file) == MCV_OK &&
               mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
                   MCV_ERR_FRAME_CRC,
           "corrupted compressed bytes are caught by the stored CRC");
    mcv_close(&file);
    remove(path);

    // Recomputing the stored CRC lets invalid zlib reach decompression.
    EXPECT(create_test_mcv_zlib(1, 1, 1, path, &first_offset, &first_size),
           "create second zlib fixture");
    EXPECT(mcv_open(path, &file) == MCV_OK, "reopen zlib fixture");
    uint64_t data_size = file.header.frame_data_size;
    mcv_close(&file);

    FILE *raw = fopen(path, "r+b");
    EXPECT(raw != nullptr, "reopen fixture for corruption");
    uint8_t *stored = malloc((size_t)first_size);
    EXPECT(stored != nullptr, "allocate stored buffer");
    fseek(raw, (long)(MCV_HEADER_SIZE + first_offset), SEEK_SET);
    EXPECT(fread(stored, 1, (size_t)first_size, raw) == (size_t)first_size,
           "read stored frame");
    stored[first_size / 2] ^= 0xFF;
    fseek(raw, (long)(MCV_HEADER_SIZE + first_offset), SEEK_SET);
    fwrite(stored, 1, (size_t)first_size, raw);
    fclose(raw);
    patch_u32(path, (long)(MCV_HEADER_SIZE + data_size + 16),
              (uint32_t)mz_crc32(MZ_CRC32_INIT, stored, (size_t)first_size));
    free(stored);

    EXPECT(mcv_open(path, &file) == MCV_OK &&
               mcv_read_frame(&file, 0, buf, sizeof(buf)) ==
                   MCV_ERR_DECOMPRESS,
           "valid-CRC garbage stream fails decompression");
    mcv_close(&file);
    remove(path);
    return 1;
}

static int test_mcv_index_window(void)
{
    // More frames than MCV_INDEX_CACHE_ENTRIES to force window refills
    // in both directions.
    const uint32_t frame_count = 300;
    const char *path = fixture("test_index_window.mcv");
    EXPECT(create_test_mcv_zlib(1, 1, frame_count, path, nullptr, nullptr),
           "create window fixture");
    struct mcv_file file;
    EXPECT(mcv_open(path, &file) == MCV_OK, "open window fixture");
    uint8_t *buf = malloc(128 * 128 * 4);
    EXPECT(buf != nullptr, "allocate frame buffer");

    const struct {
        uint32_t idx;
        uint64_t want_first;
        uint32_t want_count;
    } probes[] = {
        {0, 0, MCV_INDEX_CACHE_ENTRIES},          // initial fill
        {255, 0, MCV_INDEX_CACHE_ENTRIES},        // hit: no refill
        {256, 256, frame_count - MCV_INDEX_CACHE_ENTRIES}, // tail refill
        {299, 256, frame_count - MCV_INDEX_CACHE_ENTRIES}, // hit near EOF
        {0, 0, MCV_INDEX_CACHE_ENTRIES},          // backward refill
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint32_t idx = probes[i].idx;
        EXPECT(mcv_read_frame(&file, idx, buf, 128 * 128 * 4) == MCV_OK &&
                   buf[2] == (uint8_t)(idx % 256),
               "window probe decodes the right frame");
        EXPECT(file.cache_first == probes[i].want_first &&
                   file.cache_count == probes[i].want_count,
               "index window covers the expected entry span");
    }

    free(buf);
    mcv_close(&file);
    remove(path);
    return 1;
}

// ================================================================
// PLAYBACK CLOCK TESTS
// ================================================================

static int test_clock_normal_20fps(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_clock.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 10, path), "create fixture");

    int err = video_session_start(&s, path, 1, 0, 1000);
    EXPECT(err == 0, "start should succeed");
    EXPECT(s.state == PLAY_PLAYING, "state should be PLAYING");
    EXPECT(s.frame_duration_ms > 49.9 && s.frame_duration_ms < 50.1, "frame dur ~50ms");

    // At t=1000 (start), frame should be 0
    uint32_t frame = 0;
    int changed = video_session_tick(&s, 1000, &frame);
    // First tick at start time - frame 0 already loaded
    EXPECT(s.current_frame == 0, "frame should be 0 at start");

    // At t=1050, should advance to frame 1
    changed = video_session_tick(&s, 1050, &frame);
    EXPECT(changed == 1, "should change at 50ms");
    EXPECT(frame == 1, "frame should be 1 at 50ms");

    // At t=1100, frame 2
    changed = video_session_tick(&s, 1100, &frame);
    EXPECT(changed == 1 && frame == 2, "frame 2 at 100ms");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_clock_skip_frames(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_skip.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 100, path), "create fixture");

    video_session_start(&s, path, 1, 0, 0);

    // Jump ahead 500ms = 10 frames at 20fps
    uint32_t frame = 0;
    int changed = video_session_tick(&s, 500, &frame);
    EXPECT(changed == 1, "should change after lag");
    EXPECT(frame == 10, "should be at frame 10");
    EXPECT(s.skipped_frames == 9, "should have skipped 9 frames");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_clock_pause_resume(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_pause.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 100, path), "create fixture");

    video_session_start(&s, path, 1, 0, 0);

    // Advance to frame 4 (200ms)
    uint32_t frame = 0;
    video_session_tick(&s, 200, &frame);
    EXPECT(frame == 4, "frame 4 at 200ms");

    // Pause at 250ms
    video_session_pause(&s, 250);
    EXPECT(s.state == PLAY_PAUSED, "should be paused");

    // Tick during pause - no change
    int changed = video_session_tick(&s, 500, &frame);
    EXPECT(changed == 0, "no change during pause");

    // Resume at 1000ms (paused for 750ms)
    video_session_resume(&s, 1000);
    EXPECT(s.state == PLAY_PLAYING, "should be playing");
    EXPECT(s.accumulated_pause == 750, "accumulated pause = 750ms");

    // Effective time = 1050 - 750 = 300ms → frame 6
    changed = video_session_tick(&s, 1050, &frame);
    EXPECT(changed == 1, "should change after resume");
    EXPECT(frame == 6, "frame 6 at effective 300ms");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_clock_single_play(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_single.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture"); // 5 frames = 250ms total

    video_session_start(&s, path, 1, 0, 0); // loop=1 (play once)

    uint32_t frame = 0;
    // At 300ms, video should be finished (5 frames * 50ms = 250ms)
    int changed = video_session_tick(&s, 300, &frame);
    EXPECT(changed == 1, "should signal end");
    EXPECT(s.state == PLAY_FINISHED, "state should be FINISHED");
    EXPECT(frame == 4, "last frame = 4");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_clock_infinite_loop(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_loop.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture"); // 5 frames

    video_session_start(&s, path, -1, 0, 0); // infinite loop

    uint32_t frame = 0;
    // At 250ms = exactly 5 frames, should wrap to frame 0
    video_session_tick(&s, 260, &frame);
    EXPECT(frame == 0 || frame == 1, "should wrap around"); // 260/50=5.2 → 5%5=0

    // At 300ms = 6 frames elapsed, frame 6%5=1
    video_session_tick(&s, 300, &frame);
    EXPECT(frame == 1, "frame 1 after wrap");

    EXPECT(s.state == PLAY_PLAYING, "should still be playing");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_clock_multi_loop(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_multi.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture"); // 5 frames, 250ms per loop

    video_session_start(&s, path, 3, 0, 0); // 3 loops = 750ms total

    uint32_t frame = 0;
    // At 800ms, should be finished (3*250=750ms)
    video_session_tick(&s, 800, &frame);
    EXPECT(s.state == PLAY_FINISHED, "should finish after 3 loops");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_clock_two_screens_independent(void)
{
    struct video_session s1, s2;
    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));

    const char *path1 = fixture("test_ind1.mcv");
    const char *path2 = fixture("test_ind2.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 10, path1), "create fixture");
    EXPECT(create_test_mcv(1, 1, 10, 1, 10, path2), "create fixture"); // 10fps = 100ms per frame

    video_session_start(&s1, path1, 1, 0, 0);
    video_session_start(&s2, path2, 1, 1, 0);

    uint32_t f1 = 0, f2 = 0;
    // At 200ms: s1 at frame 4 (200/50), s2 at frame 2 (200/100)
    video_session_tick(&s1, 200, &f1);
    video_session_tick(&s2, 200, &f2);
    EXPECT(f1 == 4, "screen1 frame 4 at 200ms");
    EXPECT(f2 == 2, "screen2 frame 2 at 200ms");

    // Stop screen1, screen2 continues
    video_session_stop(&s1);
    EXPECT(s1.state == PLAY_STOPPED, "screen1 stopped");

    video_session_tick(&s2, 300, &f2);
    EXPECT(f2 == 3, "screen2 frame 3 at 300ms");
    EXPECT(s2.state == PLAY_PLAYING, "screen2 still playing");

    video_session_stop(&s2);
    remove(path1);
    remove(path2);
    return 1;
}

// ================================================================
// COMMAND ARGUMENT TESTS
// ================================================================

static int test_args_parse_int(void)
{
    int value = 123;
    EXPECT(mpv_parse_int("0", &value) && value == 0, "zero parses");
    EXPECT(mpv_parse_int("-1", &value) && value == -1, "negative parses");
    EXPECT(mpv_parse_int("2147483647", &value) && value == INT_MAX,
           "INT_MAX parses");

    value = 999;
    EXPECT(!mpv_parse_int("abc", &value) && value == 999,
           "garbage is rejected without touching the output");
    EXPECT(!mpv_parse_int("", &value), "empty string is rejected");
    EXPECT(!mpv_parse_int(nullptr, &value), "null is rejected");
    EXPECT(!mpv_parse_int("12abc", &value), "trailing garbage is rejected");
    EXPECT(!mpv_parse_int(" 7", &value), "leading space is rejected");
    EXPECT(!mpv_parse_int("+7", &value), "leading plus is rejected");
    EXPECT(!mpv_parse_int("9999999999999999999999", &value),
           "out-of-range value is rejected");
    return 1;
}

static int test_args_parse_loop(void)
{
    int loop = 0;
    EXPECT(mpv_parse_loop("-1", &loop) && loop == -1, "-1 means forever");
    EXPECT(mpv_parse_loop("1", &loop) && loop == 1, "1 plays once");
    EXPECT(mpv_parse_loop("250", &loop) && loop == 250, "N repeats N times");

    // Invalid specifications must fail loudly but still leave a safe
    // play-once value, never an accidental infinite loop.
    EXPECT(!mpv_parse_loop("0", &loop) && loop == 1, "0 is rejected");
    EXPECT(!mpv_parse_loop("-2", &loop) && loop == 1,
           "negatives other than -1 are rejected");
    EXPECT(!mpv_parse_loop("forever", &loop) && loop == 1,
           "garbage is rejected");
    EXPECT(!mpv_parse_loop(nullptr, &loop) && loop == 1, "null is rejected");
    return 1;
}

static int test_args_parse_index(void)
{
    int index = -1;
    EXPECT(mpv_parse_index("0", 3, &index) && index == 0, "first index");
    EXPECT(mpv_parse_index("2", 3, &index) && index == 2, "last index");

    EXPECT(!mpv_parse_index("3", 3, &index), "index at count is rejected");
    EXPECT(!mpv_parse_index("-1", 3, &index), "negative index is rejected");
    EXPECT(!mpv_parse_index("0", 0, &index), "empty catalog rejects any index");
    // atoi() used to turn this into video 0 and play the wrong file.
    EXPECT(!mpv_parse_index("abc", 3, &index),
           "non-numeric index is rejected instead of playing video 0");
    return 1;
}

static int test_args_video_fits_screen(void)
{
    EXPECT(mpv_video_fits_screen(4, 2, 4, 2), "identical dimensions match");
    EXPECT(!mpv_video_fits_screen(4, 2, 2, 4), "transposed does not match");
    EXPECT(!mpv_video_fits_screen(1, 1, 7, 4), "smaller does not match");
    EXPECT(!mpv_video_fits_screen(7, 4, 1, 1), "larger does not match");
    return 1;
}

// ================================================================
// SESSION LIFECYCLE TESTS
// ================================================================

static int test_session_multi_loop_counter(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_loop_counter.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture"); // 5 frames = 250ms per loop
    video_session_start(&s, path, 3, 1, 0);

    uint32_t frame = 0;
    video_session_tick(&s, 100, &frame);
    EXPECT(s.loop_current == 1, "first pass reports loop 1");

    video_session_tick(&s, 300, &frame);
    EXPECT(s.state == PLAY_PLAYING, "still playing during loop 2");
    EXPECT(frame == 1, "wrapped to frame 1 in loop 2");
    EXPECT(s.loop_current == 2, "second pass reports loop 2");

    video_session_tick(&s, 550, &frame);
    EXPECT(s.loop_current == 3, "third pass reports loop 3");

    video_session_stop(&s);
    remove(path);
    return 1;
}

static int test_session_loop_bounds(void)
{
    struct video_session s;
    memset(&s, 0, sizeof(s));

    const char *path = fixture("test_loop_bounds.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture");

    // A huge loop count must not overflow the total-frame computation
    // into a premature finish.
    video_session_start(&s, path, 1000000, 1, 0);
    uint32_t frame = 0;
    video_session_tick(&s, 100000, &frame);
    EXPECT(s.state == PLAY_PLAYING,
           "large finite loop count keeps playing rather than overflowing");
    video_session_stop(&s);

    // Negative counts other than -1 are meaningless: play once.
    video_session_start(&s, path, -7, 1, 0);
    EXPECT(s.loop_total == 1, "invalid negative loop is normalized to once");
    video_session_tick(&s, 400, &frame);
    EXPECT(s.state == PLAY_FINISHED, "normalized single play finishes");
    video_session_stop(&s);

    remove(path);
    return 1;
}

static int test_engine_slot_lookup_by_runtime_id(void)
{
    struct video_engine eng;
    video_engine_init(&eng);

    const char *path = fixture("test_engine_lookup.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture");

    EXPECT(video_engine_find(&eng, 7) == nullptr, "no session before play");

    struct video_session *a = video_engine_acquire(&eng, 7);
    EXPECT(a != nullptr, "acquire returns a free slot");
    EXPECT(video_session_start(a, path, 1, 7, 0) == 0, "start session 7");
    EXPECT(video_engine_find(&eng, 7) == a, "session is found by runtime id");
    EXPECT(video_engine_find(&eng, 8) == nullptr, "other ids do not match");

    // Re-acquiring the same screen reuses and restarts its slot.
    struct video_session *again = video_engine_acquire(&eng, 7);
    EXPECT(again == a, "acquire reuses the screen's existing slot");
    EXPECT(!a->active, "previous session was stopped before reuse");

    EXPECT(video_session_start(a, path, 1, 7, 0) == 0, "restart session 7");
    video_engine_release(&eng, 7);
    EXPECT(video_engine_find(&eng, 7) == nullptr, "release clears the session");
    EXPECT(!a->active && a->frame_buf == nullptr,
           "release frees the frame buffer");

    video_engine_shutdown(&eng);
    remove(path);
    return 1;
}

static int test_engine_survives_other_screen_delete(void)
{
    // Regression: sessions used to be keyed by registry index, and
    // screen_registry_delete compacts the array.  Deleting an earlier
    // screen therefore orphaned a later screen's live session.
    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_geom geom;
    EXPECT(screen_geom_validate((struct screen_pos){0, 66, 5},
                                (struct screen_pos){0, 64, 5},
                                "minecraft:overworld", "minecraft:overworld",
                                SCREEN_FACE_EAST, &geom) == SCREEN_GEOM_OK,
           "geometry for fixtures");

    int idx_a = -1, idx_b = -1;
    EXPECT(screen_registry_create(&reg, "alpha", "uuid-a", &geom, &idx_a) ==
               SCREEN_OK, "create screen alpha");
    EXPECT(screen_registry_create(&reg, "beta", "uuid-b", &geom, &idx_b) ==
               SCREEN_OK, "create screen beta");
    uint64_t beta_id = reg.screens[idx_b]->runtime_id;
    EXPECT(beta_id != reg.screens[idx_a]->runtime_id,
           "runtime ids are distinct");

    struct video_engine eng;
    video_engine_init(&eng);
    const char *path = fixture("test_engine_delete.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 100, path), "create fixture");

    struct video_session *beta = video_engine_acquire(&eng, beta_id);
    EXPECT(beta != nullptr && video_session_start(beta, path, -1, beta_id, 0) == 0,
           "beta starts playing");

    // Delete the EARLIER screen: beta slides from index 1 to index 0.
    EXPECT(screen_registry_delete(&reg, "alpha") == SCREEN_OK, "delete alpha");
    EXPECT(reg.count == 1 && reg.screens[0]->runtime_id == beta_id,
           "beta was compacted down to index 0");

    struct video_session *still = video_engine_find(&eng, beta_id);
    EXPECT(still == beta, "beta's session survives the unrelated delete");
    EXPECT(still->active && still->state == PLAY_PLAYING,
           "beta keeps playing after the unrelated delete");

    uint32_t frame = 0;
    EXPECT(video_session_tick(still, 500, &frame) == 1 && frame == 10,
           "beta's clock still advances");

    // Deleting beta itself releases the session.
    video_engine_release(&eng, beta_id);
    EXPECT(video_engine_find(&eng, beta_id) == nullptr,
           "beta's session is released with beta");

    video_engine_shutdown(&eng);
    screen_registry_cleanup(&reg);
    remove(path);
    return 1;
}

static int test_session_finish_releases_resources(void)
{
    struct video_engine eng;
    video_engine_init(&eng);

    const char *path = fixture("test_finish_release.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 5, path), "create fixture"); // 250ms of frames

    struct video_session *s = video_engine_acquire(&eng, 42);
    EXPECT(s != nullptr && video_session_start(s, path, 1, 42, 0) == 0,
           "session starts");
    EXPECT(s->frame_buf != nullptr, "frame buffer allocated while playing");

    uint32_t frame = 0;
    EXPECT(video_session_tick(s, 400, &frame) == 1 &&
               s->state == PLAY_FINISHED,
           "single play finishes past the end");

    // video_tick stops the session on PLAY_FINISHED so the file handle
    // and the multi-megabyte frame buffer are not held until an
    // explicit /mpv stop.
    video_session_stop(s);
    EXPECT(s->frame_buf == nullptr && s->frame_buf_size == 0,
           "stopping a finished session frees the frame buffer");
    EXPECT(video_engine_find(&eng, 42) == nullptr,
           "finished session no longer occupies the screen");

    video_engine_shutdown(&eng);
    remove(path);
    return 1;
}

static int test_session_start_failure_releases_resources(void)
{
    struct video_session session = {0};
    const char *path = fixture("test_start_failure_release.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 1, path), "create start failure fixture");
    patch_u8(path, MCV_HEADER_SIZE + 100, 0xFF);

    int error = video_session_start(&session, path, 1, 73, 0);
    EXPECT(error == MCV_ERR_FRAME_CRC,
           "first-frame failure preserves its exact MCV error");
    EXPECT(!session.active && session.state == PLAY_STOPPED &&
               session.mcv.fp == nullptr && session.frame_buf == nullptr &&
               session.frame_buf_size == 0 &&
               session.mcv.stored_scratch == nullptr,
           "failed start stops and releases all session resources");
    remove(path);
    return 1;
}

// ================================================================
// SCREEN REGISTRY TESTS
// ================================================================

static int test_registry_create_find(void)
{
    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_pos c1 = {0, 66, 5}, c2 = {3, 64, 5};
    struct screen_geom geom;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &geom);

    int idx = -1;
    enum screen_error err = screen_registry_create(&reg, "lobby", "uuid-1234", &geom, &idx);
    EXPECT(err == SCREEN_OK, "create should succeed");
    EXPECT(idx == 0, "first index should be 0");
    EXPECT(reg.count == 1, "count should be 1");

    int found = screen_registry_find(&reg, "lobby");
    EXPECT(found == 0, "find should return 0");

    found = screen_registry_find(&reg, "nonexistent");
    EXPECT(found == -1, "find nonexistent should return -1");

    screen_registry_cleanup(&reg);
    return 1;
}

static int test_registry_duplicate_name(void)
{
    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_pos c1 = {0, 65, 0}, c2 = {1, 65, 0};
    struct screen_geom geom;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &geom);

    screen_registry_create(&reg, "test", "uuid-1", &geom, nullptr);
    enum screen_error err = screen_registry_create(&reg, "test", "uuid-2", &geom, nullptr);
    EXPECT(err == SCREEN_ERR_NAME_EXISTS, "duplicate name should fail");
    screen_registry_cleanup(&reg);
    return 1;
}

static int test_registry_invalid_name(void)
{
    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_pos c1 = {0, 65, 0}, c2 = {1, 65, 0};
    struct screen_geom geom;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &geom);

    EXPECT(screen_registry_create(&reg, "../etc/passwd", "u", &geom, nullptr) == SCREEN_ERR_NAME_INVALID,
           "path traversal should fail");
    EXPECT(screen_registry_create(&reg, "a/b", "u", &geom, nullptr) == SCREEN_ERR_NAME_INVALID,
           "slash should fail");
    EXPECT(screen_registry_create(&reg, ".hidden", "u", &geom, nullptr) == SCREEN_ERR_NAME_INVALID,
           "leading dot should fail");
    EXPECT(screen_registry_create(&reg, "valid-name_123", "u", &geom, nullptr) == SCREEN_OK,
           "valid name should succeed");
    screen_registry_cleanup(&reg);
    return 1;
}

static int test_registry_runtime_identity_survives_shifts(void)
{
    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_pos c1 = {0, 65, 0}, c2 = {1, 65, 0};
    struct screen_geom geom;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &geom);

    screen_registry_create(&reg, "a", "owner-a", &geom, nullptr);
    screen_registry_create(&reg, "b", "owner-b", &geom, nullptr);
    screen_registry_create(&reg, "c", "owner-c", &geom, nullptr);
    struct screen_entry *stable = reg.screens[2];
    uint64_t removed_id = reg.screens[1]->runtime_id;
    uint64_t shifted_id = reg.screens[2]->runtime_id;
    struct mpv_public_membership membership = {0};
    mpv_membership_replace(&membership, &removed_id, 1);
    EXPECT(removed_id != 0 && shifted_id != 0 && removed_id != shifted_id,
           "runtime screen identities are unique");
    EXPECT(screen_registry_delete(&reg, "b") == SCREEN_OK,
           "delete middle screen");
    EXPECT(strcmp(reg.screens[1]->name, "c") == 0 &&
               reg.screens[1]->runtime_id == shifted_id &&
               reg.screens[1] == stable,
           "compaction moves pointers while entry address stays stable");
    EXPECT(mpv_membership_transition(&membership,
                                     reg.screens[1]->runtime_id, true) ==
               MPV_MEMBERSHIP_ENTERED,
           "deleted screen membership cannot attach to shifted screen");
    screen_registry_create(&reg, "d", "owner-d", &geom, nullptr);
    EXPECT(reg.screens[2]->runtime_id != removed_id,
           "deleted runtime identity is not reused");
    screen_registry_cleanup(&reg);
    return 1;
}

static int test_registry_lazy_tiles_and_cleanup(void)
{
    struct screen_registry reg;
    screen_registry_init(&reg);
    struct screen_geom large = {0};
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 1023, 0},
               (struct screen_pos){1023, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &large) == SCREEN_GEOM_OK,
           "construct 1024x1024 registry geometry");
    int index = -1;
    EXPECT(screen_registry_create(&reg, "large", "owner", &large, &index) ==
               SCREEN_OK,
           "large logical screen should create");
    EXPECT(reg.screens[index]->tiles == nullptr &&
               reg.screens[index]->tiles_capacity == 0,
           "large logical screen keeps compatibility tiles lazy");
    EXPECT(screen_entry_materialize_tiles(reg.screens[index]) ==
               SCREEN_ERR_MATERIALIZATION_LIMIT &&
               reg.screens[index]->tiles == nullptr &&
               reg.screens[index]->tiles_capacity == 0,
           "large logical screen cannot materialize a million map records");

    struct screen_geom small = {0};
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 1, 0},
               (struct screen_pos){1, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &small) == SCREEN_GEOM_OK,
           "construct materialization geometry");
    int small_index = -1;
    EXPECT(screen_registry_create(&reg, "small", "owner", &small,
                                  &small_index) == SCREEN_OK,
           "small logical screen should create");
    struct screen_entry *entry = reg.screens[small_index];
    EXPECT(screen_entry_materialize_tiles(entry) == SCREEN_OK &&
               entry->tiles != nullptr && entry->tiles_capacity == 4,
           "materialization allocates exactly the checked tile count");
    EXPECT(entry->tiles[0].map_id == -1 && entry->tiles[3].map_id == -1,
           "materialized map ids start invalid");
    screen_registry_cleanup(&reg);
    EXPECT(reg.count == 0, "registry cleanup releases all entries");
    return 1;
}

static int test_registry_delete(void)
{
    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_pos c1 = {0, 65, 0}, c2 = {1, 65, 0};
    struct screen_geom geom;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &geom);

    screen_registry_create(&reg, "a", "u1", &geom, nullptr);
    screen_registry_create(&reg, "b", "u2", &geom, nullptr);
    screen_registry_create(&reg, "c", "u3", &geom, nullptr);
    EXPECT(reg.count == 3, "count = 3");

    screen_registry_delete(&reg, "b");
    EXPECT(reg.count == 2, "count = 2 after delete");
    EXPECT(screen_registry_find(&reg, "b") == -1, "b not found");
    EXPECT(screen_registry_find(&reg, "a") == 0, "a at index 0");
    EXPECT(screen_registry_find(&reg, "c") == 1, "c shifted to index 1");
    screen_registry_cleanup(&reg);
    return 1;
}

// ================================================================
// PERSISTENCE TESTS
// ================================================================

static unsigned char *persistence_read_bytes(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return nullptr;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return nullptr;
    }
    long length = ftell(file);
    if (length < 0) {
        fclose(file);
        return nullptr;
    }
    rewind(file);
    unsigned char *data = calloc((size_t)length + 1, 1);
    if (!data) {
        fclose(file);
        return nullptr;
    }
    size_t read_count = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (read_count != (size_t)length) {
        free(data);
        return nullptr;
    }
    if (size_out)
        *size_out = (size_t)length;
    return data;
}

static int persistence_write_bytes(const char *path, const void *data,
                                   size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    int ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

static uint32_t persistence_read_u32_le(const unsigned char *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static void persistence_write_u32_le(unsigned char *data, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        data[i] = (unsigned char)(value >> (8 * i));
}

static char *persistence_sidecar_path(const char *manifest_path)
{
    size_t manifest_size = 0;
    unsigned char *manifest = persistence_read_bytes(manifest_path,
                                                       &manifest_size);
    if (!manifest)
        return nullptr;
    cJSON *root = cJSON_Parse((const char *)manifest);
    free(manifest);
    if (!root)
        return nullptr;
    cJSON *field = cJSON_GetObjectItemCaseSensitive(
        root, SCREEN_SIDECAR_FIELD);
    if (!cJSON_IsString(field) || !field->valuestring[0]) {
        cJSON_Delete(root);
        return nullptr;
    }

    const char *separator = strrchr(manifest_path, '/');
    const char *backslash = strrchr(manifest_path, '\\');
    if (backslash && (!separator || backslash > separator))
        separator = backslash;
    size_t directory_length = separator
                                  ? (size_t)(separator - manifest_path + 1)
                                  : 0;
    size_t sidecar_length = strlen(field->valuestring);
    char *result = calloc(directory_length + sidecar_length + 1, 1);
    if (result) {
        memcpy(result, manifest_path, directory_length);
        memcpy(result + directory_length, field->valuestring,
               sidecar_length);
    }
    cJSON_Delete(root);
    return result;
}

static int persistence_update_ref(const char *manifest_path, const char *key,
                                  const char *value)
{
    size_t manifest_size = 0;
    unsigned char *manifest = persistence_read_bytes(manifest_path,
                                                       &manifest_size);
    if (!manifest)
        return -1;
    cJSON *root = cJSON_Parse((const char *)manifest);
    free(manifest);
    if (!root)
        return -1;
    cJSON *screens = cJSON_GetObjectItemCaseSensitive(root, "screens");
    cJSON *screen = cJSON_GetArrayItem(screens, 0);
    cJSON *ref = cJSON_GetObjectItemCaseSensitive(screen, "map_ids");
    cJSON *replacement = cJSON_CreateString(value);
    if (!cJSON_IsArray(screens) || !cJSON_IsObject(screen) ||
        !cJSON_IsObject(ref) || !replacement ||
        !cJSON_ReplaceItemInObjectCaseSensitive(ref, key, replacement)) {
        cJSON_Delete(replacement);
        cJSON_Delete(root);
        return -1;
    }
    replacement = nullptr;
    char *rewritten = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rewritten)
        return -1;
    int result = persistence_write_bytes(manifest_path, rewritten,
                                         strlen(rewritten));
    free(rewritten);
    return result;
}

static int persistence_seed_destination(struct screen_registry *reg,
                                        struct screen_entry **entry_out)
{
    screen_registry_init(reg);
    struct screen_geom geom;
    if (screen_geom_validate(
            (struct screen_pos){0, 64, 0},
            (struct screen_pos){0, 64, 0},
            "minecraft:overworld", "minecraft:overworld",
            SCREEN_FACE_SOUTH, &geom) != SCREEN_GEOM_OK)
        return -1;
    int index = -1;
    if (screen_registry_create(reg, "sentinel", "owner", &geom, &index) !=
        SCREEN_OK)
        return -1;
    if (entry_out)
        *entry_out = reg->screens[index];
    return 0;
}

static int persistence_load_failure_preserves(const char *path)
{
    struct screen_registry destination;
    struct screen_entry *sentinel = nullptr;
    if (persistence_seed_destination(&destination, &sentinel) != 0)
        return 0;
    int warnings = 0;
    int result = screen_persistence_load(&destination, path, &warnings);
    int preserved = result == -1 && warnings == 1 &&
                    destination.count == 1 &&
                    destination.screens[0] == sentinel &&
                    strcmp(destination.screens[0]->name, "sentinel") == 0;
    screen_registry_cleanup(&destination);
    return preserved;
}

static int test_persistence_v1_migration_and_extremes(void)
{
    const char *path = fixture("test_persistence_v1.json");
    remove_persistence_artifacts(path);
    FILE *file = fopen(path, "wb");
    EXPECT(file != nullptr, "open v1 migration fixture");
    fprintf(file,
            "{\"format_version\":1,\"screens\":[{"
            "\"name\":\"legacy-managed\",\"owner_uuid\":\"owner\","
            "\"dimension\":\"minecraft:overworld\",\"facing\":0,"
            "\"width\":2,\"height\":1,"
            "\"corner1\":{\"x\":0,\"y\":64,\"z\":0},"
            "\"corner2\":{\"x\":1,\"y\":64,\"z\":0},"
            "\"created_at\":1700000000,\"plugin_managed\":true,"
            "\"map_ids\":[\"-9223372036854775808\","
            "\"9223372036854775807\"]}]}");
    fclose(file);

    struct screen_registry loaded;
    screen_registry_init(&loaded);
    int warnings = 0;
    EXPECT(screen_persistence_load(&loaded, path, &warnings) == 0 &&
               warnings == 0 && loaded.count == 1,
           "v1 managed fixture loads");
    EXPECT(loaded.screens[0]->plugin_managed &&
               loaded.screens[0]->tiles[0].map_id_valid &&
               loaded.screens[0]->tiles[0].map_id == INT64_MIN &&
               loaded.screens[0]->tiles[1].map_id == INT64_MAX,
           "v1 map IDs preserve both int64 extremes");
    EXPECT(screen_persistence_save(&loaded, path) == 0,
           "v1 fixture saves as v2");

    size_t manifest_size = 0;
    unsigned char *manifest = persistence_read_bytes(path, &manifest_size);
    EXPECT(manifest != nullptr && manifest_size < 4096,
           "migrated manifest remains compact");
    EXPECT(strstr((const char *)manifest, "\"format_version\":2") != nullptr,
           "migrated manifest uses v2");
    EXPECT(strstr((const char *)manifest, "\"map_ids\":[") == nullptr &&
               strstr((const char *)manifest, "\"map_ids\":{") != nullptr,
           "v2 never emits a map_ids JSON array");
    free(manifest);
    char *sidecar = persistence_sidecar_path(path);
    EXPECT(sidecar != nullptr, "v2 manifest names a sidecar basename");
    FILE *sidecar_file = sidecar ? fopen(sidecar, "rb") : nullptr;
    EXPECT(sidecar_file != nullptr, "migrated sidecar exists beside manifest");
    if (sidecar_file)
        fclose(sidecar_file);
    free(sidecar);

    screen_registry_cleanup(&loaded);
    remove_persistence_artifacts(path);
    return 1;
}

static int test_persistence_v2_roundtrip_and_logical_size(void)
{
    const char *path = fixture("test_persistence_v2.json");
    const char *logical_path = fixture("test_persistence_logical.json");
    remove_persistence_artifacts(path);
    remove_persistence_artifacts(logical_path);

    struct screen_registry registry;
    screen_registry_init(&registry);
    struct screen_geom geom;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 64, 0},
               (struct screen_pos){1, 64, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &geom) == SCREEN_GEOM_OK,
           "construct exact v2 geometry");
    int index = -1;
    EXPECT(screen_registry_create(&registry, "exact", "owner", &geom, &index) ==
               SCREEN_OK,
           "create exact v2 screen");
    struct screen_entry *entry = registry.screens[index];
    entry->plugin_managed = 1;
    EXPECT(screen_entry_materialize_tiles(entry) == SCREEN_OK,
           "materialize exact v2 tiles");
    entry->tiles[0].map_id = INT64_MIN;
    entry->tiles[0].map_id_valid = 1;
    entry->tiles[1].map_id = INT64_MAX;
    entry->tiles[1].map_id_valid = 1;
    EXPECT(screen_persistence_save(&registry, path) == 0,
           "save exact v2 IDs");
    size_t committed_manifest_size = 0;
    unsigned char *committed_manifest = persistence_read_bytes(
        path, &committed_manifest_size);
    EXPECT(committed_manifest != nullptr,
           "read committed manifest before deterministic save failure");
    entry->tiles[1].map_id_valid = 0;
    EXPECT(screen_persistence_save(&registry, path) == -1,
           "incomplete physical map IDs reject the save");
    size_t preserved_manifest_size = 0;
    unsigned char *preserved_manifest = persistence_read_bytes(
        path, &preserved_manifest_size);
    EXPECT(preserved_manifest != nullptr &&
               preserved_manifest_size == committed_manifest_size &&
               memcmp(preserved_manifest, committed_manifest,
                      committed_manifest_size) == 0,
           "failed save leaves the committed manifest byte-exact");
    free(preserved_manifest);
    free(committed_manifest);
    entry->tiles[1].map_id_valid = 1;
    screen_registry_cleanup(&registry);

    struct screen_registry loaded;
    screen_registry_init(&loaded);
    int warnings = 0;
    EXPECT(screen_persistence_load(&loaded, path, &warnings) == 0 &&
               warnings == 0 && loaded.count == 1,
           "load exact v2 IDs");
    EXPECT(loaded.screens[0]->tiles[0].map_id == INT64_MIN &&
               loaded.screens[0]->tiles[1].map_id == INT64_MAX &&
               loaded.screens[0]->tiles[0].map_id_valid &&
               loaded.screens[0]->tiles[1].map_id_valid,
           "v2 sidecar roundtrip preserves exact IDs");
    screen_registry_cleanup(&loaded);

    screen_registry_init(&registry);
    struct screen_geom large_geom;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 63, 0},
               (struct screen_pos){63, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &large_geom) == SCREEN_GEOM_OK,
           "construct moderately large materialized geometry");
    EXPECT(screen_registry_create(&registry, "large", "owner", &large_geom,
                                  &index) == SCREEN_OK,
           "create moderately large screen");
    entry = registry.screens[index];
    entry->plugin_managed = 1;
    EXPECT(screen_entry_materialize_tiles(entry) == SCREEN_OK,
           "materialize moderately large screen");
    for (int tile = 0; tile < screen_geom_tile_count(&large_geom); tile++) {
        entry->tiles[tile].map_id = INT64_C(1000000) + tile;
        entry->tiles[tile].map_id_valid = 1;
    }
    // The next save publishes a new content-derived generation; explicitly
    // remove the prior generation so this test leaves no orphan sidecar.
    remove_manifest_sidecar(path);
    EXPECT(screen_persistence_save(&registry, path) == 0,
           "save moderately large screen");
    size_t large_manifest_size = 0;
    unsigned char *large_manifest = persistence_read_bytes(
        path, &large_manifest_size);
    EXPECT(large_manifest != nullptr && large_manifest_size < 4096,
           "manifest remains bounded by screen count, not tile count");
    free(large_manifest);
    screen_registry_cleanup(&registry);

    screen_registry_init(&registry);
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 1023, 0},
               (struct screen_pos){1023, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &large_geom) == SCREEN_GEOM_OK,
           "construct logical 1024x1024 geometry");
    EXPECT(screen_registry_create(&registry, "logical", "owner", &large_geom,
                                  nullptr) == SCREEN_OK,
           "create logical 1024x1024 screen");
    EXPECT(screen_persistence_save(&registry, logical_path) == 0,
           "save logical screen");
    size_t logical_manifest_size = 0;
    unsigned char *logical_manifest = persistence_read_bytes(
        logical_path, &logical_manifest_size);
    EXPECT(logical_manifest != nullptr &&
               strstr((const char *)logical_manifest,
                      "\"" SCREEN_SIDECAR_FIELD "\"") == nullptr &&
               strstr((const char *)logical_manifest, "\"map_ids\"") == nullptr,
           "logical screen emits no sidecar or map ID reference");
    free(logical_manifest);
    screen_registry_cleanup(&registry);
    remove_persistence_artifacts(path);
    remove_persistence_artifacts(logical_path);
    return 1;
}

static int test_persistence_sidecar_corruption_matrix(void)
{
    const char *path = fixture("test_persistence_corruption.json");
    remove_persistence_artifacts(path);

    struct screen_registry registry;
    screen_registry_init(&registry);
    struct screen_geom geom;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 64, 0},
               (struct screen_pos){1, 64, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &geom) == SCREEN_GEOM_OK,
           "construct corruption matrix geometry");
    int index = -1;
    EXPECT(screen_registry_create(&registry, "managed", "owner", &geom,
                                  &index) == SCREEN_OK,
           "create corruption matrix screen");
    struct screen_entry *entry = registry.screens[index];
    entry->plugin_managed = 1;
    EXPECT(screen_entry_materialize_tiles(entry) == SCREEN_OK,
           "materialize corruption matrix tiles");
    entry->tiles[0].map_id = INT64_C(-77);
    entry->tiles[1].map_id = INT64_C(88);
    entry->tiles[0].map_id_valid = 1;
    entry->tiles[1].map_id_valid = 1;
    EXPECT(screen_persistence_save(&registry, path) == 0,
           "save corruption matrix fixture");
    screen_registry_cleanup(&registry);

    size_t manifest_size = 0;
    unsigned char *manifest_original = persistence_read_bytes(
        path, &manifest_size);
    char *sidecar_path = persistence_sidecar_path(path);
    size_t sidecar_size = 0;
    unsigned char *sidecar_original = sidecar_path
                                          ? persistence_read_bytes(
                                                sidecar_path, &sidecar_size)
                                          : nullptr;
    EXPECT(manifest_original != nullptr && sidecar_path != nullptr &&
               sidecar_original != nullptr && sidecar_size >= 40,
           "read corruption matrix files");
    EXPECT(strstr((const char *)manifest_original,
                  "\"" SCREEN_SIDECAR_FIELD "\"") != nullptr,
           "corruption matrix manifest references sidecar");

    // Ensure the first two diagnostic copies exercise .bad and .bad.1.
    remove("test_persistence_corruption.json.bad");
    remove("test_persistence_corruption.json.bad.1");
    unsigned char *first_bad = nullptr;
    size_t first_bad_size = 0;
    for (int case_index = 0; case_index < 5; case_index++) {
        unsigned char *mutated = calloc(sidecar_size, 1);
        EXPECT(mutated != nullptr, "allocate sidecar corruption copy");
        memcpy(mutated, sidecar_original, sidecar_size);
        size_t written_size = sidecar_size;
        if (case_index == 0) {
            mutated[0] ^= 0x01;
        } else if (case_index == 1) {
            mutated[8] ^= 0x01;
        } else if (case_index == 2) {
            written_size--;
        } else if (case_index == 3) {
            mutated[sidecar_size - 1] ^= 0x01;
        } else {
            mutated[sidecar_size - 1] ^= 0x01;
            uint32_t crc = (uint32_t)mz_crc32(
                MZ_CRC32_INIT, mutated + SCREEN_SIDECAR_HEADER_SIZE,
                sidecar_size - SCREEN_SIDECAR_HEADER_SIZE);
            persistence_write_u32_le(mutated + 20, crc);
        }
        EXPECT(persistence_write_bytes(sidecar_path, mutated, written_size) ==
                   0,
               "write sidecar corruption case");
        size_t before_manifest_size = 0;
        unsigned char *before_manifest = persistence_read_bytes(
            path, &before_manifest_size);
        EXPECT(before_manifest != nullptr &&
                   before_manifest_size == manifest_size &&
                   memcmp(before_manifest, manifest_original,
                          manifest_size) == 0,
               "manifest is intact before sidecar failure");
        free(before_manifest);
        EXPECT(persistence_load_failure_preserves(path),
               "sidecar failure preserves populated destination");
        size_t after_manifest_size = 0;
        unsigned char *after_manifest = persistence_read_bytes(
            path, &after_manifest_size);
        size_t after_sidecar_size = 0;
        unsigned char *after_sidecar = persistence_read_bytes(
            sidecar_path, &after_sidecar_size);
        EXPECT(after_manifest != nullptr && after_manifest_size == manifest_size &&
                   memcmp(after_manifest, manifest_original, manifest_size) == 0 &&
                   after_sidecar != nullptr && after_sidecar_size == written_size &&
                   memcmp(after_sidecar, mutated, written_size) == 0,
               "sidecar and manifest remain unchanged after failure");
        free(after_manifest);
        free(after_sidecar);
        free(mutated);

        if (case_index == 0) {
            first_bad = persistence_read_bytes(
                "test_persistence_corruption.json.bad", &first_bad_size);
            EXPECT(first_bad != nullptr && first_bad_size == manifest_size &&
                       memcmp(first_bad, manifest_original, manifest_size) == 0,
                   "first diagnostic copy is exact");
        }
        if (case_index == 1) {
            size_t second_bad_size = 0;
            unsigned char *second_bad = persistence_read_bytes(
                "test_persistence_corruption.json.bad.1", &second_bad_size);
            EXPECT(second_bad != nullptr && second_bad_size == manifest_size &&
                       memcmp(second_bad, manifest_original, manifest_size) == 0 &&
                       first_bad != nullptr && first_bad_size == manifest_size &&
                       memcmp(first_bad, manifest_original, manifest_size) == 0,
                   "successive diagnostic copy never overwrites .bad");
            free(second_bad);
        }
    }
    free(first_bad);

    // A changed global checksum must not hide a per-screen checksum error.
    EXPECT(persistence_write_bytes(sidecar_path, sidecar_original,
                                   sidecar_size) == 0,
           "restore sidecar before reference cases");
    EXPECT(persistence_write_bytes(path, manifest_original, manifest_size) == 0,
           "restore manifest before reference cases");
    EXPECT(persistence_update_ref(path, "offset", "999999999") == 0,
           "write out-of-range reference");
    EXPECT(persistence_load_failure_preserves(path),
           "out-of-range reference preserves destination");
    size_t range_manifest_size = 0;
    unsigned char *range_manifest = persistence_read_bytes(
        path, &range_manifest_size);
    EXPECT(range_manifest != nullptr && range_manifest_size > 0,
           "range-corrupt manifest remains present");
    free(range_manifest);
    EXPECT(persistence_write_bytes(path, manifest_original, manifest_size) == 0,
           "restore manifest before count case");
    EXPECT(persistence_update_ref(path, "count", "3") == 0,
           "write mismatched reference count");
    EXPECT(persistence_load_failure_preserves(path),
           "mismatched reference count preserves destination");
    size_t count_manifest_size = 0;
    unsigned char *count_manifest = persistence_read_bytes(
        path, &count_manifest_size);
    EXPECT(count_manifest != nullptr && count_manifest_size > 0,
           "count-corrupt manifest remains present");
    free(count_manifest);

    free(manifest_original);
    free(sidecar_original);
    free(sidecar_path);
    remove_persistence_artifacts(path);
    return 1;
}

static int test_persistence_roundtrip(void)
{
    const char *path = fixture("test_screens.json");

    struct screen_registry reg;
    screen_registry_init(&reg);

    struct screen_pos c1 = {10, 70, 5}, c2 = {13, 67, 5};
    struct screen_geom geom;
    screen_geom_validate(c1, c2, "minecraft:overworld", "minecraft:overworld", SCREEN_FACE_SOUTH, &geom);

    int idx;
    screen_registry_create(&reg, "lobby", "owner-uuid-1234", &geom, &idx);
    reg.screens[idx]->created_at = 1700000000;

    // Second screen
    struct screen_pos c3 = {0, 65, 0}, c4 = {0, 63, 3};
    struct screen_geom geom2;
    screen_geom_validate(c3, c4, "minecraft:nether", "minecraft:nether", SCREEN_FACE_EAST, &geom2);
    screen_registry_create(&reg, "nether-screen", "owner-2", &geom2, nullptr);

    EXPECT(screen_persistence_save(&reg, path) == 0, "save should succeed");

    // Load into fresh registry
    struct screen_registry reg2;
    screen_registry_init(&reg2);
    int warnings = 0;
    EXPECT(screen_persistence_load(&reg2, path, &warnings) == 0, "load should succeed");
    EXPECT(warnings == 0, "no warnings");
    EXPECT(reg2.count == 2, "loaded 2 screens");

    int li = screen_registry_find(&reg2, "lobby");
    EXPECT(li >= 0, "lobby found");
    EXPECT(strcmp(reg2.screens[li]->owner_uuid, "owner-uuid-1234") == 0, "owner preserved");
    EXPECT(reg2.screens[li]->geom.width == 4, "width preserved");
    EXPECT(reg2.screens[li]->geom.height == 4, "height preserved");
    EXPECT(reg2.screens[li]->geom.facing == SCREEN_FACE_SOUTH, "facing preserved");
    EXPECT(reg2.screens[li]->created_at == 1700000000, "created_at preserved");

    int ni = screen_registry_find(&reg2, "nether-screen");
    EXPECT(ni >= 0, "nether-screen found");
    EXPECT(strcmp(reg2.screens[ni]->geom.dimension, "minecraft:nether") == 0, "dimension preserved");
    EXPECT(reg2.screens[ni]->geom.facing == SCREEN_FACE_EAST, "east facing preserved");

    screen_registry_cleanup(&reg);
    screen_registry_cleanup(&reg2);
    remove(path);
    return 1;
}

static int test_persistence_omits_and_ignores_viewers(void)
{
    const char *path = fixture("test_public_screens.json");
    const char *rewritten_path = fixture("test_public_screens_rewritten.json");
    struct screen_registry registry;
    screen_registry_init(&registry);
    struct screen_pos position = {10, 64, 10};
    struct screen_geom geometry;
    EXPECT(screen_geom_validate(position, position, "minecraft:overworld",
                                "minecraft:overworld", SCREEN_FACE_SOUTH,
                                &geometry) == SCREEN_GEOM_OK,
           "construct public persistence geometry");
    EXPECT(screen_registry_create(&registry, "public", "owner-kept",
                                  &geometry, nullptr) == SCREEN_OK,
           "create public persistence screen");
    EXPECT(screen_persistence_save(&registry, path) == 0,
           "save public screen");

    FILE *saved = fopen(path, "rb");
    EXPECT(saved != nullptr, "open newly saved JSON");
    fseek(saved, 0, SEEK_END);
    long saved_size = ftell(saved);
    fseek(saved, 0, SEEK_SET);
    char *saved_json = malloc((size_t)saved_size + 1);
    EXPECT(saved_json != nullptr, "allocate saved JSON buffer");
    size_t saved_read = fread(saved_json, 1, (size_t)saved_size, saved);
    fclose(saved);
    saved_json[saved_read] = '\0';
    EXPECT(strstr(saved_json, "\"viewers\"") == nullptr,
           "new saves omit obsolete viewers field");
    free(saved_json);

    FILE *legacy = fopen(path, "wb");
    EXPECT(legacy != nullptr, "open legacy JSON fixture");
    fprintf(legacy,
            "{\"format_version\":1,\"screens\":[{"
            "\"name\":\"legacy-public\",\"owner_uuid\":\"owner-kept\","
            "\"dimension\":\"minecraft:overworld\",\"facing\":0,"
            "\"width\":1,\"height\":1,"
            "\"corner1\":{\"x\":10,\"y\":64,\"z\":10},"
            "\"corner2\":{\"x\":10,\"y\":64,\"z\":10},"
            "\"viewers\":[\"old-viewer-1\",\"old-viewer-2\"]}]}" );
    fclose(legacy);

    struct screen_registry loaded;
    screen_registry_init(&loaded);
    int warnings = 0;
    EXPECT(screen_persistence_load(&loaded, path, &warnings) == 0 &&
               warnings == 0 && loaded.count == 1,
           "legacy viewers field is tolerated and ignored");
    EXPECT(strcmp(loaded.screens[0]->owner_uuid, "owner-kept") == 0,
           "owner metadata remains preserved");
    EXPECT(screen_persistence_save(&loaded, rewritten_path) == 0,
           "legacy screen rewrites in public format");

    saved = fopen(rewritten_path, "rb");
    EXPECT(saved != nullptr, "open rewritten public JSON");
    fseek(saved, 0, SEEK_END);
    saved_size = ftell(saved);
    fseek(saved, 0, SEEK_SET);
    saved_json = malloc((size_t)saved_size + 1);
    EXPECT(saved_json != nullptr, "allocate rewritten JSON buffer");
    saved_read = fread(saved_json, 1, (size_t)saved_size, saved);
    fclose(saved);
    saved_json[saved_read] = '\0';
    EXPECT(strstr(saved_json, "\"viewers\"") == nullptr,
           "obsolete viewers disappear after successful save");
    free(saved_json);
    screen_registry_cleanup(&registry);
    screen_registry_cleanup(&loaded);
    remove(path);
    remove(rewritten_path);
    return 1;
}

static int test_persistence_facing_range(void)
{
    // screen_geom_tile_pos indexes a static 4-entry table with the
    // facing value, so a tampered file must be rejected at load.
    const char *path = fixture("test_facing_range.json");
    struct screen_registry loaded;
    screen_registry_init(&loaded);

    static const struct {
        const char *facing;
        int accepted;
    } cases[] = {
        {"0", 1}, {"3", 1},         // SOUTH .. WEST are the valid range
        {"4", 0}, {"-1", 0}, {"99", 0},
        {"\"south\"", 0},           // non-numeric facing
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FILE *f = fopen(path, "wb");
        EXPECT(f != nullptr, "open facing fixture");
        fprintf(f,
                "{\"format_version\":1,\"screens\":[{"
                "\"name\":\"probe\",\"owner_uuid\":\"owner\","
                "\"dimension\":\"minecraft:overworld\",\"facing\":%s,"
                "\"width\":1,\"height\":1,"
                "\"corner1\":{\"x\":10,\"y\":64,\"z\":10},"
                "\"corner2\":{\"x\":10,\"y\":64,\"z\":10}}]}",
                cases[i].facing);
        fclose(f);

        int warnings = 0;
        EXPECT(screen_persistence_load(&loaded, path, &warnings) == 0,
               "loader tolerates the fixture file");
        if (cases[i].accepted) {
            EXPECT(loaded.count == 1 && warnings == 0,
                   "in-range facing loads without warnings");
            EXPECT(loaded.screens[0]->geom.facing >= SCREEN_FACE_SOUTH &&
                       loaded.screens[0]->geom.facing <= SCREEN_FACE_WEST,
                   "loaded facing stays inside the table");
        } else {
            EXPECT(loaded.count == 0 && warnings == 1,
                   "out-of-range or non-numeric facing is skipped");
        }
    }

    remove(path);
    return 1;
}

static int test_managed_map_identity_roundtrip(void)
{
    const char *path = fixture("test_managed_screens.json");
    struct screen_registry registry;
    screen_registry_init(&registry);
    struct screen_pos position = {126, 111, 161};
    struct screen_geom geometry;
    EXPECT(screen_geom_validate(position, position, "minecraft:overworld",
                                "minecraft:overworld", SCREEN_FACE_SOUTH,
                                &geometry) == SCREEN_GEOM_OK,
           "construct managed 1x1 geometry");
    int index = -1;
    EXPECT(screen_registry_create(&registry, "managed", "owner", &geometry,
                                  &index) == SCREEN_OK,
           "create managed persistence entry");
    registry.screens[index]->plugin_managed = 1;
    EXPECT(screen_entry_materialize_tiles(registry.screens[index]) == SCREEN_OK,
           "materialize managed persistence tiles");
    registry.screens[index]->tiles[0].map_id = INT64_C(-1507533518726);
    registry.screens[index]->tiles[0].map_id_valid = 1;
    EXPECT(screen_persistence_save(&registry, path) == 0,
           "persist managed map identity");

    struct screen_registry loaded;
    screen_registry_init(&loaded);
    int warnings = 0;
    EXPECT(screen_persistence_load(&loaded, path, &warnings) == 0 &&
               warnings == 0,
           "load managed map identity");
    int loaded_index = screen_registry_find(&loaded, "managed");
    EXPECT(loaded_index >= 0 && loaded.screens[loaded_index]->plugin_managed,
           "managed ownership marker survives restart");
    EXPECT(loaded.screens[loaded_index]->tiles[0].map_id_valid &&
               loaded.screens[loaded_index]->tiles[0].map_id ==
                   INT64_C(-1507533518726),
           "int64 map id survives restart exactly");
    EXPECT(!loaded.screens[loaded_index]->tiles_initialized &&
               loaded.screens[loaded_index]->tiles[0].map_view == nullptr,
           "runtime map pointers are not persisted");
    screen_registry_cleanup(&registry);
    screen_registry_cleanup(&loaded);
    remove(path);
    return 1;
}

static int test_persistence_corrupted(void)
{
    const char *path = fixture("test_corrupt.json");
    remove_persistence_artifacts(path);

    // Write garbage
    FILE *f = fopen(path, "w");
    fprintf(f, "{invalid json!!!}}}");
    fclose(f);

    struct screen_registry reg;
    screen_registry_init(&reg);
    struct screen_geom sentinel_geom;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 64, 0},
               (struct screen_pos){0, 64, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &sentinel_geom) == SCREEN_GEOM_OK,
           "construct corrupt-load sentinel geometry");
    int sentinel_index = -1;
    EXPECT(screen_registry_create(&reg, "sentinel", "owner",
                                  &sentinel_geom, &sentinel_index) == SCREEN_OK,
           "create corrupt-load sentinel");
    struct screen_entry *sentinel = reg.screens[sentinel_index];
    int warnings = 0;
    int ret = screen_persistence_load(&reg, path, &warnings);
    EXPECT(ret == -1, "corrupted JSON should return -1");
    EXPECT(reg.count == 1 && reg.screens[0] == sentinel &&
               strcmp(reg.screens[0]->name, "sentinel") == 0,
           "corrupt load preserves populated destination");
    EXPECT(warnings == 1, "corrupt file is reported as a warning");

    // The loader copies the unreadable file aside without moving or
    // overwriting the original.
    FILE *aside = fopen("test_corrupt.json.bad", "rb");
    EXPECT(aside != nullptr, "corrupt file is preserved as .bad");
    fclose(aside);
    aside = fopen(path, "rb");
    EXPECT(aside != nullptr, "original corrupt path remains present");
    if (aside)
        fclose(aside);

    screen_registry_cleanup(&reg);
    remove_persistence_artifacts(path);
    return 1;
}

static int test_persistence_truncated(void)
{
    const char *path = fixture("test_trunc.json");
    remove_persistence_artifacts(path);

    FILE *f = fopen(path, "w");
    fprintf(f, "{\"format_version\": 1, \"screens\": [{\"name\": \"ok\", \"owner_uuid\": \"u\", ");
    fclose(f); // truncated mid-object
    size_t original_size = 0;
    unsigned char *original = persistence_read_bytes(path, &original_size);
    EXPECT(original != nullptr, "read truncated manifest bytes");

    struct screen_registry reg;
    screen_registry_init(&reg);
    struct screen_geom sentinel_geom;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 64, 0},
               (struct screen_pos){0, 64, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &sentinel_geom) == SCREEN_GEOM_OK,
           "construct truncation sentinel geometry");
    EXPECT(screen_registry_create(&reg, "sentinel", "owner",
                                  &sentinel_geom, nullptr) == SCREEN_OK,
           "create truncation sentinel");
    struct screen_entry *sentinel = reg.screens[0];
    int warnings = 0;
    int ret = screen_persistence_load(&reg, path, &warnings);
    EXPECT(ret == -1 && reg.count == 1 && reg.screens[0] == sentinel,
           "truncated load preserves populated destination");
    size_t after_size = 0;
    unsigned char *after = persistence_read_bytes(path, &after_size);
    EXPECT(after != nullptr && after_size == original_size &&
               memcmp(after, original, original_size) == 0,
           "truncated manifest remains unchanged");
    free(after);
    size_t bad_size = 0;
    unsigned char *bad = persistence_read_bytes("test_trunc.json.bad",
                                                 &bad_size);
    EXPECT(bad != nullptr && bad_size == original_size &&
               memcmp(bad, original, original_size) == 0,
           "truncated manifest diagnostic is an exact copy");
    free(bad);
    free(original);

    screen_registry_cleanup(&reg);
    remove_persistence_artifacts(path);
    return 1;
}

static int test_persistence_missing_file(void)
{
    const char *path = "nonexistent_file_xyz.json";
    remove_persistence_artifacts(path);
    struct screen_registry reg;
    screen_registry_init(&reg);
    struct screen_geom sentinel_geom;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 64, 0},
               (struct screen_pos){0, 64, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &sentinel_geom) == SCREEN_GEOM_OK,
           "construct missing-file sentinel geometry");
    EXPECT(screen_registry_create(&reg, "sentinel", "owner",
                                  &sentinel_geom, nullptr) == SCREEN_OK,
           "create missing-file sentinel");
    struct screen_entry *sentinel = reg.screens[0];
    int warnings = 0;
    int ret = screen_persistence_load(&reg, path, &warnings);
    EXPECT(ret == 0, "missing file should return 0 (not an error)");
    EXPECT(reg.count == 1 && reg.screens[0] == sentinel &&
               strcmp(reg.screens[0]->name, "sentinel") == 0 &&
               warnings == 0,
           "missing file leaves destination untouched");
    screen_registry_cleanup(&reg);
    return 1;
}

static int test_persistence_invalid_dimensions(void)
{
    const char *path = fixture("test_baddim.json");

    FILE *f = fopen(path, "w");
    fprintf(f, "{\"format_version\":1,\"screens\":[{\"name\":\"bad\",\"owner_uuid\":\"u\","
               "\"dimension\":\"minecraft:overworld\",\"facing\":0,\"width\":1025,\"height\":1025,"
               "\"corner1\":{\"x\":0,\"y\":65,\"z\":0},\"corner2\":{\"x\":1,\"y\":65,\"z\":0},"
               "\"viewers\":[]}]}");
    fclose(f);

    struct screen_registry reg;
    screen_registry_init(&reg);
    int warnings = 0;
    screen_persistence_load(&reg, path, &warnings);
    EXPECT(reg.count == 0, "invalid dimensions should be skipped");
    EXPECT(warnings == 1, "one warning for invalid entry");

    remove(path);
    return 1;
}

static int test_preferences_roundtrip(void)
{
    const char *path = fixture("test_preferences.json");
    struct mpv_preferences preferences;
    mpv_preferences_init(&preferences);
    EXPECT(mpv_preferences_enabled(&preferences, "player-a"),
           "players default to public media enabled");
    EXPECT(mpv_preferences_set_enabled(&preferences, "player-a", false),
           "player can disable public media");
    EXPECT(!mpv_preferences_enabled(&preferences, "player-a"),
           "disabled preference is visible immediately");
    EXPECT(mpv_preferences_save(&preferences, path) == 0,
           "preferences save succeeds");

    struct mpv_preferences loaded;
    EXPECT(mpv_preferences_load(&loaded, path) == 0,
           "preferences load succeeds");
    EXPECT(!mpv_preferences_enabled(&loaded, "player-a") &&
               mpv_preferences_enabled(&loaded, "player-b"),
           "only explicit opt-outs persist");
    EXPECT(mpv_preferences_set_enabled(&loaded, "player-a", true),
           "player can re-enable public media");
    EXPECT(mpv_preferences_enabled(&loaded, "player-a"),
           "re-enabled player is removed from opt-outs");
    remove(path);
    return 1;
}

static int test_screen_audio_video_clock(void)
{
    struct music_cache cache;
    music_cache_init(&cache);
    struct music_cache_entry song = {0};
    snprintf(song.song_name, sizeof(song.song_name), "demo");
    arrput(song.notes, ((struct music_note){
        .time_ms = 0, .instrument = 0, .volume = 1.0f, .pitch = 1.0f}));
    arrput(song.notes, ((struct music_note){
        .time_ms = 50, .instrument = 1, .volume = 0.5f, .pitch = 1.2f}));
    arrput(song.notes, ((struct music_note){
        .time_ms = 900, .instrument = 2, .volume = 0.7f, .pitch = 0.8f}));
    arrput(cache.entries, song);

    struct screen_audio_engine engine;
    screen_audio_engine_init(&engine);
    struct screen_audio_session *session = &engine.sessions[0];
    session->active = true;
    session->screen_runtime_id = 42;
    session->song_index = 0;
    session->loop_current = 1;

    int player_a, player_b;
    void *players[] = {&player_a, &player_b};
    g_sound_count = 0;
    g_last_sound_player = nullptr;
    g_last_sound[0] = '\0';

    screen_audio_tick(session, &cache, 1, 0, false, players, 2);
    EXPECT(g_sound_count == 2 && strcmp(g_last_sound, "note.harp") == 0,
           "time-zero note is sent to every screen viewer");
    screen_audio_tick(session, &cache, 1, 60, false, players, 2);
    EXPECT(g_sound_count == 4 &&
               strcmp(g_last_sound, "note.bassattack") == 0,
           "future note follows video loop elapsed time");
    screen_audio_tick(session, &cache, 1, 60, false, players, 2);
    EXPECT(g_sound_count == 4, "audio notes are never repeated within a loop");

    screen_audio_tick(session, &cache, 2, 0, true, players, 2);
    EXPECT(g_sound_count == 6 && session->cursor == 1,
           "video loop transition restarts the NBS cursor");
    screen_audio_tick(session, &cache, 4, 950, true, players, 2);
    EXPECT(g_sound_count == 8 && session->cursor == 3,
           "lagged loop restart skips stale notes instead of bursting");

    screen_audio_stop(&engine, 42);
    EXPECT(screen_audio_find(&engine, 42) == nullptr,
           "stopping video removes its screen audio session");
    music_cache_shutdown(&cache);
    return 1;
}

static int test_detailed_tick_reports_loop_boundary(void)
{
    struct video_session session = {0};
    const char *path = fixture("test_detailed_loop.mcv");
    EXPECT(create_test_mcv(1, 1, 20, 1, 4, path),
           "create detailed loop fixture");
    EXPECT(video_session_start(&session, path, 2, 88, 1000) == 0,
           "start detailed loop session");

    struct video_tick_result tick;
    video_session_tick_detailed(&session, 1210, &tick);
    EXPECT(tick.loop_changed && !tick.finished &&
               tick.loop_current == 2 && tick.frame == 0 &&
               tick.loop_elapsed_ms >= 9 && tick.loop_elapsed_ms <= 11,
           "tick explicitly reports the second loop boundary");
    video_session_tick_detailed(&session, 1400, &tick);
    EXPECT(tick.finished && session.state == PLAY_FINISHED,
           "detailed tick reports the final video boundary");

    video_session_stop(&session);
    remove(path);
    return 1;
}

// ================================================================
// COMMAND AND PUBLIC VIEWER POLICY TESTS
// ================================================================

static int test_command_permission_and_surface(void)
{
    EXPECT(!mpv_command_allowed(true, false, "play"),
           "non-operator player cannot control playback");
    EXPECT(mpv_command_allowed(true, false, "watch") &&
               mpv_command_allowed(true, false, "help"),
           "non-operator player can manage personal viewing");
    EXPECT(mpv_command_allowed(true, true, "play"),
           "operator player passes centralized gate");
    EXPECT(mpv_command_allowed(false, false, "play"),
           "console passes centralized gate");

    int catalog = 7, screens = 8, playback = 9, persistence = 10;
    if (mpv_command_allowed(true, false, "play")) {
        catalog = screens = playback = persistence = 0;
    }
    EXPECT(catalog == 7 && screens == 8 && playback == 9 &&
               persistence == 10,
           "denied command cannot reach state-changing dispatch");

    const char *removed[] = {
        "(pos1)", "(pos2)", "(validate)", "(repair)",
        "(bind)", "(unbind)",
    };
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
    EXPECT(MPV_COMMAND_USAGE_COUNT == 17,
           "debug build registers exactly 17 usages");
#else
    EXPECT(MPV_COMMAND_USAGE_COUNT == 16,
           "default build registers exactly 16 usages");
#endif
    EXPECT(strcmp(mpv_command_usages[5],
                   "/mpv (materialize)<a: MpvMaterialize> <name: string>") == 0,
           "materialize command is adjacent to create");
    EXPECT(strcmp(mpv_command_usages[6],
                   "/mpv (delete)<a: MpvDelete> <name: string>") == 0,
           "delete command has no confirmation parameter");
    for (int i = 0; i < MPV_COMMAND_USAGE_COUNT; i++) {
        for (size_t r = 0; r < sizeof(removed) / sizeof(removed[0]); r++) {
            EXPECT(strstr(mpv_command_usages[i], removed[r]) == nullptr,
                   "removed command is absent from registrations");
        }
    }
    EXPECT(mpv_command_action_registered("") &&
               mpv_command_action_registered("help") &&
               mpv_command_action_registered("watch") &&
               mpv_command_action_registered("materialize") &&
               mpv_command_action_registered("images") &&
               mpv_command_action_registered("image"),
           "root, help and watch remain registered");
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
    EXPECT(mpv_command_action_registered("debug"),
           "debug build registers the debug action");
#else
    EXPECT(!mpv_command_action_registered("debug"),
           "default build rejects the debug action");
#endif
    EXPECT(!mpv_command_action_registered("pos1") &&
               !mpv_command_action_registered("pos2") &&
               !mpv_command_action_registered("validate") &&
               !mpv_command_action_registered("repair") &&
               !mpv_command_action_registered("bind") &&
               !mpv_command_action_registered("unbind"),
           "all six obsolete actions are rejected");
    return 1;
}

static int test_mps_catalog_and_source_lifecycle(void)
{
    const char *directory = "test_mps_catalog_runtime";
    const char *valid_path = "test_mps_catalog_runtime/valid.mps";
    const char *bad_path = "test_mps_catalog_runtime/malformed.mps";
    remove(valid_path);
    remove(bad_path);
    test_remove_directory(directory);
    EXPECT(test_make_directory(directory) &&
               write_test_mps(valid_path, 2, 1, 0) &&
               write_test_mps(bad_path, 2, 1, 1),
           "write valid and malformed MPS catalog fixtures");

    struct mps_catalog catalog;
    mps_catalog_init(&catalog, directory);
    EXPECT(catalog.count == 1 && strcmp(catalog.entries[0].name, "valid") == 0 &&
               catalog.entries[0].tile_width == 2 &&
               catalog.entries[0].tile_height == 1,
           "catalog keeps valid MPS files and ignores malformed headers");

    struct mps_source_engine engine;
    mps_source_engine_init(&engine);
    struct mps_source *source = (struct mps_source *)(uintptr_t)1;
    EXPECT(mps_source_start(&engine, 42, valid_path, "valid", 1, 1,
                            &source) == MPS_SOURCE_ERR_DIMENSION &&
               source == nullptr,
           "source rejects exact-dimension mismatch and clears output");
    EXPECT(mps_source_start(&engine, 42, valid_path, "valid", 2, 1,
                            &source) == MPS_SOURCE_OK && source != nullptr &&
               source->initial_cursor == 0 && source->tile_cache == nullptr,
           "source starts with lazy cache and zero initial cursor");
    uint64_t first_attachment = source->attachment_id;
    struct presenter_stream_tile tile = {0};
    EXPECT(mps_source_read(source, 0, &tile) == MPS_SOURCE_OK &&
               tile.pixels[0] == 1 && source->tile_read_count == 1,
           "source lazily reads the first tile");
    EXPECT(mps_source_read(source, 0, &tile) == MPS_SOURCE_OK &&
               tile.pixels[0] == 1 && source->tile_read_count == 1,
           "repeated tile read uses the one-tile cache");
    EXPECT(mps_source_read(source, 1, &tile) == MPS_SOURCE_OK &&
               tile.pixels[0] == 2 && source->tile_read_count == 2,
           "different tile invalidates cache and reads once");

    struct mps_source *replacement = nullptr;
    EXPECT(mps_source_start(&engine, 42, valid_path, "replacement", 2, 1,
                            &replacement) == MPS_SOURCE_OK &&
               replacement != nullptr &&
               replacement->attachment_id != first_attachment &&
               replacement->tile_cache == nullptr,
           "replacement closes old source and changes attachment identity");
    mps_source_release(&engine, 42);
    EXPECT(mps_source_find(&engine, 42) == nullptr,
           "source release removes runtime state and cache");
    mps_source_engine_shutdown(&engine);
    EXPECT(mps_source_find(&engine, 42) == nullptr,
           "source shutdown leaves no active runtime source");
    remove(valid_path);
    remove(bad_path);
    test_remove_directory(directory);
    return 1;
}

static struct screen_geom public_test_geometry(void)
{
    struct screen_geom geometry;
    struct screen_pos position = {0, 64, 0};
    screen_geom_validate(position, position, "minecraft:overworld",
                         "minecraft:overworld", SCREEN_FACE_SOUTH,
                         &geometry);
    return geometry;
}

static struct mpv_public_snapshot public_snapshot(double x, double y, double z,
                                                  const char *dimension)
{
    struct mpv_public_snapshot snapshot = {0};
    snapshot.valid = true;
    snapshot.x = x;
    snapshot.y = y;
    snapshot.z = z;
    snprintf(snapshot.dimension, sizeof(snapshot.dimension), "%s", dimension);
    return snapshot;
}

static int test_public_viewer_eligibility(void)
{
    struct screen_geom geometry = public_test_geometry();
    struct mpv_screen_center center = mpv_screen_center(&geometry);
    EXPECT(center.x == 0.5 && center.y == 64.5 && center.z == 0.5,
           "1x1 screen center uses block-cell geometric center");

    struct mpv_public_snapshot below = public_snapshot(
        16.5, center.y, center.z, "minecraft:overworld");
    struct mpv_public_snapshot exact = public_snapshot(
        17.0, center.y, center.z, "minecraft:overworld");
    struct mpv_public_snapshot above = public_snapshot(
        17.01, center.y, center.z, "minecraft:overworld");
    struct mpv_public_snapshot other = public_snapshot(
        center.x, center.y, center.z, "minecraft:nether");
    struct mpv_public_snapshot invalid = below;
    invalid.valid = false;

    EXPECT(mpv_public_viewer_eligible(true, &below, &geometry),
            "same-dimension player below 16 blocks is eligible");
    EXPECT(mpv_public_viewer_eligible(true, &exact, &geometry),
            "same-dimension player exactly 16 blocks is eligible");
    EXPECT(!mpv_public_viewer_eligible(true, &above, &geometry),
            "player above 16 blocks is excluded");
    EXPECT(!mpv_public_viewer_eligible(true, &other, &geometry),
           "different-dimension player is excluded");
    EXPECT(!mpv_public_viewer_eligible(true, &invalid, &geometry),
           "invalid snapshot is excluded");
    EXPECT(!mpv_public_viewer_eligible(false, &below, &geometry),
           "offline player is excluded");

    struct screen_geom huge;
    EXPECT(screen_geom_validate(
               (struct screen_pos){0, 1023, 0},
               (struct screen_pos){1023, 0, 0},
               "minecraft:overworld", "minecraft:overworld",
               SCREEN_FACE_SOUTH, &huge) == SCREEN_GEOM_OK,
           "construct huge public screen geometry");
    struct mpv_public_snapshot huge_near = public_snapshot(
        1023.5, 0.5, 16.0, "minecraft:overworld");
    struct mpv_public_snapshot huge_far = public_snapshot(
        -32.0, 0.5, 16.0, "minecraft:overworld");
    EXPECT(mpv_public_viewer_eligible(true, &huge_near, &huge),
           "near edge of a huge screen remains eligible");
    EXPECT(!mpv_public_viewer_eligible(true, &huge_far, &huge),
           "far outside a huge screen is excluded");
    return 1;
}

static int test_public_viewer_collection_and_membership(void)
{
    struct screen_geom geometry = public_test_geometry();
    struct mpv_screen_center center = mpv_screen_center(&geometry);
    int player_a, player_b, player_far, player_other, player_invalid;
    int player_disabled;
    struct mpv_public_candidate candidates[] = {
        {.player = &player_a, .uuid = "a", .online = true,
         .public_media_enabled = true,
         .snapshot = public_snapshot(center.x, center.y, center.z,
                                     "minecraft:overworld")},
        {.player = &player_b, .uuid = "b", .online = true,
         .public_media_enabled = true,
         .snapshot = public_snapshot(center.x + 16.0, center.y,
                                     center.z, "minecraft:overworld")},
        {.player = &player_far, .uuid = "far", .online = true,
         .public_media_enabled = true,
         .snapshot = public_snapshot(center.x + 17.0, center.y, center.z,
                                     "minecraft:overworld")},
        {.player = &player_other, .uuid = "other", .online = true,
         .public_media_enabled = true,
         .snapshot = public_snapshot(center.x, center.y, center.z,
                                     "minecraft:nether")},
        {.player = &player_invalid, .uuid = "invalid", .online = true,
         .public_media_enabled = true},
        {.player = &player_disabled, .uuid = "disabled", .online = true,
         .public_media_enabled = false,
         .snapshot = public_snapshot(center.x, center.y, center.z,
                                     "minecraft:overworld")},
    };
    void *players[5] = {0};
    const char *ids[5] = {0};
    int count = mpv_collect_public_viewers(
        &geometry, candidates, 6, players, ids, 5);
    EXPECT(count == 2 && players[0] == &player_a && players[1] == &player_b,
            "all nearby public players and only those players are collected");
    EXPECT(players[0] != &player_disabled && players[1] != &player_disabled,
           "disabled public media preference excludes a nearby player");
    EXPECT(strcmp(ids[0], "a") == 0 && strcmp(ids[1], "b") == 0,
           "UUIDs are carried only as diagnostics");

    struct mpv_public_membership membership = {0};
    uint64_t screen_id = 42;
    EXPECT(mpv_membership_transition(&membership, screen_id, true) ==
               MPV_MEMBERSHIP_ENTERED,
           "first eligible refresh triggers entry resend");
    mpv_membership_replace(&membership, &screen_id, 1);
    EXPECT(mpv_membership_transition(&membership, screen_id, true) ==
               MPV_MEMBERSHIP_STAYED,
           "remaining eligible does not trigger another entry resend");
    EXPECT(mpv_membership_transition(&membership, screen_id, false) ==
               MPV_MEMBERSHIP_LEFT,
           "leaving range is detected");
    mpv_membership_replace(&membership, nullptr, 0);
    EXPECT(mpv_membership_transition(&membership, screen_id, true) ==
               MPV_MEMBERSHIP_ENTERED,
           "re-entering range triggers another entry resend");
    return 1;
}

// ================================================================
// MAP COLOR TESTS (ABGR format verification)
// ================================================================

static int test_map_test_patterns(void)
{
    EXPECT(map_render_pattern_pixel(MAP_TEST_RED, 0, 0, 256, 256) ==
               UINT32_C(0xff0000ff),
           "red diagnostic pattern is ABGR");
    EXPECT(map_render_pattern_pixel(MAP_TEST_GREEN, 0, 0, 256, 256) ==
               UINT32_C(0xff00ff00),
           "green diagnostic pattern is ABGR");
    EXPECT(map_render_pattern_pixel(MAP_TEST_BLUE, 0, 0, 256, 256) ==
               UINT32_C(0xffff0000),
           "blue diagnostic pattern is ABGR");
    EXPECT(map_render_pattern_pixel(MAP_TEST_CHECKER, 0, 0, 256, 256) !=
               map_render_pattern_pixel(MAP_TEST_CHECKER, 16, 0, 256, 256),
           "checker alternates every 16 pixels");
    EXPECT(map_render_pattern_pixel(MAP_TEST_QUADRANTS, 10, 10, 256, 256) ==
               UINT32_C(0xff0000ff),
           "quadrant top-left is red");
    EXPECT(map_render_pattern_pixel(MAP_TEST_QUADRANTS, 200, 10, 256, 256) ==
               UINT32_C(0xff00ff00),
           "quadrant top-right is green");
    EXPECT(map_render_pattern_pixel(MAP_TEST_QUADRANTS, 10, 200, 256, 256) ==
               UINT32_C(0xffff0000),
           "quadrant bottom-left is blue");
    EXPECT(map_render_pattern_pixel(MAP_TEST_QUADRANTS, 200, 200, 256, 256) ==
               UINT32_C(0xffffffff),
           "quadrant bottom-right is white");
    return 1;
}

#if defined(ES_PLATFORM_WINDOWS)
static void *g_fake_map_argument;
static void *g_fake_shared_argument;
static int g_fake_destroy_count;
static int g_fake_delete_count;
static void *g_fake_uuid_this;
static void *g_fake_renderer;
static void *g_fake_renderer_control;
static void *g_fake_map_view;
static int64_t g_fake_get_map_requested_id;
static int g_fake_create_map_calls;
static int g_fake_get_map_calls;
static uint32_t g_fake_rendered_pixel;
static int g_fake_render_send_count;
static int g_fake_map_locked;
static void *g_fake_probe_block;
static void *g_fake_probe_actor;
static int g_fake_probe_x;
static int g_fake_probe_y;
static int g_fake_probe_z;
static int g_fake_probe_actor_x;
static int g_fake_probe_actor_y;
static int g_fake_probe_actor_z;
static unsigned int g_fake_probe_delete_flags;

static void fake_send_map(void *self, void *map)
{
    (void)self;
    g_fake_map_argument = map;
}

static void fake_add_renderer(void *self, void *shared)
{
    (void)self;
    g_fake_shared_argument = shared;
}

static void fake_destroy(void *self)
{
    (void)self;
    g_fake_destroy_count++;
}

static void fake_delete(void *self)
{
    (void)self;
    g_fake_delete_count++;
}

static void *fake_get_unique_id(void *self, unsigned char *output)
{
    g_fake_uuid_this = self;
    for (int i = 0; i < ES_UUID_SIZE; i++) {
        output[i] = (unsigned char)i;
    }
    return output;
}

static void *fake_dimension_get_block(void *self, void **output,
                                      int x, int y, int z)
{
    (void)self;
    g_fake_probe_x = x;
    g_fake_probe_y = y;
    g_fake_probe_z = z;
    *output = g_fake_probe_block;
    return output;
}

static void *fake_block_source_get_actor(void *self, const int *position)
{
    (void)self;
    g_fake_probe_actor_x = position[0];
    g_fake_probe_actor_y = position[1];
    g_fake_probe_actor_z = position[2];
    return g_fake_probe_actor;
}

static void fake_block_delete(void *self, unsigned int flags)
{
    (void)self;
    g_fake_probe_delete_flags = flags;
}

static void *fake_create_map(void *self, void *dimension)
{
    (void)self;
    (void)dimension;
    g_fake_create_map_calls++;
    return g_fake_map_view;
}

static void *fake_server_get_map(void *self, int64_t map_id)
{
    (void)self;
    g_fake_get_map_calls++;
    g_fake_get_map_requested_id = map_id;
    return g_fake_map_view;
}

static int64_t fake_get_map_id(void *self)
{
    (void)self;
    return 777;
}

static void fake_set_locked(void *self, bool locked)
{
    (void)self;
    g_fake_map_locked = locked;
}

static void fake_map_add_renderer(void *self, struct es_msvc_shared_ptr *shared)
{
    g_fake_renderer = shared->ptr;
    g_fake_renderer_control = shared->control;
    es_msvc_shared_ptr_add_ref(shared); // renderers_ copy
    es_msvc_shared_ptr_add_ref(shared); // canvases_ key copy
    void **renderer_vtable = *(void ***)shared->ptr;
    ((void (*)(void *, void *))renderer_vtable[ES_MAPRENDERER_SLOT_INIT])(
        shared->ptr, self);
    es_msvc_shared_ptr_release(shared); // destroy by-value parameter
}

static bool fake_map_remove_renderer(void *self,
                                     const struct es_msvc_shared_ptr *shared)
{
    (void)self;
    struct es_msvc_shared_ptr renderers_copy = *shared;
    struct es_msvc_shared_ptr canvases_copy = *shared;
    es_msvc_shared_ptr_release(&renderers_copy);
    es_msvc_shared_ptr_release(&canvases_copy);
    g_fake_renderer = nullptr;
    g_fake_renderer_control = nullptr;
    return true;
}

static void fake_player_send_rendered_map(void *self, void *map)
{
    g_fake_render_send_count++;
    uint32_t pixels[SCREEN_TILE_SIZE * SCREEN_TILE_SIZE] = {0};
    struct {
        void **vtable;
        uint32_t *begin;
        uint32_t *end;
        uint32_t *capacity;
    } canvas = { nullptr, pixels, pixels + SCREEN_TILE_SIZE * SCREEN_TILE_SIZE,
                 pixels + SCREEN_TILE_SIZE * SCREEN_TILE_SIZE };
    void **renderer_vtable = *(void ***)g_fake_renderer;
    ((void (*)(void *, void *, void *, void *))
        renderer_vtable[ES_MAPRENDERER_SLOT_RENDER])(
            g_fake_renderer, map, &canvas, self);
    g_fake_rendered_pixel = pixels[0];
}

static int test_map_abi_dispatch(void)
{
    void *player_vtable[ES_PLAYER_SLOT_SEND_MAP + 1] = {0};
    struct { void **vtable; } player = { player_vtable };
    int map_object = 42;
    player_vtable[ES_PLAYER_SLOT_SEND_MAP] = (void *)fake_send_map;
    g_fake_map_argument = nullptr;
    es_player_send_map(&player, &map_object);
    EXPECT(g_fake_map_argument == &map_object,
           "sendMap wrapper dispatches MapView& through measured slot");
    EXPECT(es_player_send_map_target(&player) == (void *)fake_send_map,
           "sendMap target diagnostic reports measured slot target");

    void *map_vtable[ES_MAPVIEW_SLOT_ADD_RENDERER + 1] = {0};
    struct { void **vtable; } map = { map_vtable };
    struct es_msvc_shared_ptr shared = { &map_object, &player };
    map_vtable[ES_MAPVIEW_SLOT_ADD_RENDERER] = (void *)fake_add_renderer;
    g_fake_shared_argument = nullptr;
    es_map_view_add_renderer(&map, &shared);
    EXPECT(g_fake_shared_argument == &shared,
           "addRenderer wrapper passes address of 16-byte by-value parameter");

    struct {
        void **primary_vtable;
        void **offline_vtable;
    } endstone_player = {0};
    void *offline_vtable[ES_OFFLINE_PLAYER_SLOT_GET_UNIQUE_ID + 1] = {0};
    offline_vtable[ES_OFFLINE_PLAYER_SLOT_GET_UNIQUE_ID] =
        (void *)fake_get_unique_id;
    endstone_player.offline_vtable = offline_vtable;
    char uuid[37];
    g_fake_uuid_this = nullptr;
    EXPECT(es_player_uuid_string(&endstone_player, uuid),
           "UUID wrapper succeeds for measured OfflinePlayer subobject");
    EXPECT(g_fake_uuid_this == &endstone_player.offline_vtable,
           "UUID wrapper applies +8 secondary-base this adjustment");
    EXPECT(strcmp(uuid, "00010203-0405-0607-0809-0a0b0c0d0e0f") == 0,
           "UUID wrapper formats canonical persistent identity");

    void *dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ + 1] = {0};
    struct { void **vtable; } dimension = {dimension_vtable};
    dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ] =
        (void *)fake_dimension_get_block;

    void *source_vtable[ES_BLOCK_SOURCE_SLOT_GET_BLOCK_ENTITY + 1] = {0};
    struct { void **vtable; } source = {source_vtable};
    source_vtable[ES_BLOCK_SOURCE_SLOT_GET_BLOCK_ENTITY] =
        (void *)fake_block_source_get_actor;

    void *block_vtable[] = {(void *)fake_block_delete};
    struct {
        void **vtable;
        void *block_source;
    } block = {block_vtable, &source};

    void *actor_vtable[] = {(void *)fake_destroy};
    struct { void **vtable; } actor = {actor_vtable};

    g_fake_probe_block = &block;
    g_fake_probe_actor = &actor;
    g_fake_probe_delete_flags = 0;
    struct es_block_actor_probe probe;
    EXPECT(es_probe_block_actor(&dimension, 126, 111, 161, &probe),
           "read-only BlockActor probe resolves a block entity");
    EXPECT(g_fake_probe_x == 126 && g_fake_probe_y == 111 &&
               g_fake_probe_z == 161,
           "Dimension hidden-return wrapper preserves x/y/z arguments");
    EXPECT(g_fake_probe_actor_x == 126 && g_fake_probe_actor_y == 111 &&
               g_fake_probe_actor_z == 161,
           "BlockSource probe receives the requested BlockPos by reference");
    EXPECT(probe.block_found && probe.block_actor_found &&
               probe.block_actor == &actor,
           "BlockActor probe reports safe object-presence diagnostics");
    EXPECT(probe.block_actor_vptr == actor_vtable,
           "BlockActor probe captures only the primary runtime vptr");
    EXPECT(g_fake_probe_delete_flags == 1,
           "temporary EndstoneBlock uses scalar deleting destructor");
    return 1;
}

static int test_msvc_shared_ptr_release_contract(void)
{
    void *control_vtable[] = { (void *)fake_destroy, (void *)fake_delete };
    struct {
        void **vtable;
        int uses;
        int weaks;
    } control = { control_vtable, 1, 1 };
    int object;
    struct es_msvc_shared_ptr shared = { &object, &control };
    g_fake_destroy_count = 0;
    g_fake_delete_count = 0;
    es_msvc_shared_ptr_release(&shared);
    EXPECT(g_fake_destroy_count == 1, "last strong reference calls _Destroy");
    EXPECT(g_fake_delete_count == 1, "last weak reference calls _Delete_this");
    EXPECT(shared.ptr == nullptr && shared.control == nullptr,
           "released shared_ptr storage is cleared");
    return 1;
}

// ----------------------------------------------------------------
// Stateful fake write-world.  Backs both the renderer lifetime test (which
// needs a resolvable fake Player for the pure-C map creation path) and the
// Pure-C World Write ABI tests below.  Every object mimics the measured
// MSVC layouts: hidden return buffers, scalar deleting destructors with
// flag 1, consumed 16-byte optional<ItemStack> parameters and the fake
// BlockStates list shape.
// ----------------------------------------------------------------

#define FAKE_WW_INV_SIZE 6
#define FAKE_WW_CELLS 4

struct fake_ww_server { void **vtable; };
struct fake_ww_map_view { void **vtable; };
struct fake_ww_registry { void **vtable; };
struct fake_ww_item_type { void **vtable; };

struct fake_ww_cell {
    struct screen_pos pos;
    char type[48];
};

struct fake_ww_world { // serves as the Endstone Dimension object
    void **vtable;
    struct fake_ww_cell cells[FAKE_WW_CELLS];
    int cell_count;
    int set_data_calls;
    void *last_set_data;
    int last_physics;
    int set_data_applies;
};

struct fake_ww_block {
    void **vtable;
    struct fake_ww_world *world; // +8: read as the block_source diagnostic
    struct fake_ww_cell *cell;
};

struct fake_ww_block_data {
    void **vtable;
    char type[32];
    int facing;
};

struct fake_ww_item_impl {
    void **vtable;
    int64_t map_id;
    int has_map_id;
};

struct fake_ww_item_meta {
    void **vtable;
    int type;
    int has_map_id;
    int64_t map_id;
    void *map_view;
};

struct fake_ww_inventory {
    void **vtable;
    int occupied[FAKE_WW_INV_SIZE];
    int64_t map_id[FAKE_WW_INV_SIZE];
    int clear_calls;
    int last_cleared;
    int drop_set_item_at; // slot whose setItem silently fails, or -1
};

struct fake_ww_player {
    void **vtable;
    struct fake_ww_world *world;
    struct fake_ww_inventory *inventory;
};

static void *g_ww_server_vtable[ES_SERVER_SLOT_GET_REGISTRY + 1];
static void *g_ww_registry_vtable[ES_ITEM_REGISTRY_SLOT_GET + 1];
static void *g_ww_item_type_vtable[ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK + 1];
static void *g_ww_impl_vtable[ES_ITEM_STACK_SLOT_SET_ITEM_META + 1];
static void *g_ww_meta_vtable[ES_MAP_META_SLOT_SET_MAP_VIEW + 1];
static void *g_ww_inventory_vtable[ES_INVENTORY_SLOT_CLEAR_SLOT + 1];
static void *g_ww_player_vtable[ES_PLAYER_SLOT_GET_INVENTORY + 1];
static void *g_ww_dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ + 1];
static void *g_ww_block_vtable[ES_BLOCK_SLOT_SET_DATA + 1];
static void *g_ww_block_data_vtable[1];
static void *g_ww_map_view_vtable[ES_MAPVIEW_SLOT_GET_ID + 1];

static struct fake_ww_registry g_ww_registry;
static struct fake_ww_item_type g_ww_item_type;

static int g_ww_block_live;
static int g_ww_block_data_live;
static int g_ww_impl_live;
static int g_ww_meta_live;
static int g_ww_bad_delete_flags;
static int g_ww_set_lore_calls;
static int g_ww_clear_all_calls;
static int g_ww_create_states_calls;
static int g_ww_create_air_calls;
static int g_ww_states_shape_ok;
static int g_ww_states_facing;
static char g_ww_states_type[48];
static int g_ww_registry_identifier_ok;
static int g_ww_registry_missing;
static int g_ww_create_stack_amount;
static int g_ww_set_item_meta_result;
static int g_ww_meta_type;
static int64_t g_ww_map_view_id;
static int g_ww_set_item_bad_param;
static char g_ww_last_display_name[192];

static void *fake_ww_get_location(void *self, struct es_location *out)
{
    struct fake_ww_player *player = self;
    memset(out, 0, sizeof(*out));
    out->dimension = player->world;
    out->x = 0.5f;
    out->y = 64.0f;
    out->z = 0.5f;
    return out;
}

static void *fake_ww_get_dimension(void *self)
{
    return ((struct fake_ww_player *)self)->world;
}

static void *fake_ww_get_inventory(void *self)
{
    return ((struct fake_ww_player *)self)->inventory;
}

static void *fake_ww_dimension_get_name(void *self, void *out)
{
    (void)self;
    cpp_string_construct(out, "minecraft:overworld");
    return out;
}

static struct fake_ww_cell *fake_ww_find_cell(struct fake_ww_world *world,
                                              int x, int y, int z)
{
    for (int i = 0; i < world->cell_count; i++) {
        if (world->cells[i].pos.x == x && world->cells[i].pos.y == y &&
            world->cells[i].pos.z == z) {
            return &world->cells[i];
        }
    }
    return nullptr;
}

static void *fake_ww_get_block(void *self, void **out, int x, int y, int z)
{
    struct fake_ww_world *world = self;
    struct fake_ww_cell *cell = fake_ww_find_cell(world, x, y, z);
    *out = nullptr;
    if (!cell) return out;
    struct fake_ww_block *block = calloc(1, sizeof(*block));
    block->vtable = g_ww_block_vtable;
    block->world = world;
    block->cell = cell;
    g_ww_block_live++;
    *out = block;
    return out;
}

static void fake_ww_block_delete(void *self, unsigned int flags)
{
    if (flags != 1) g_ww_bad_delete_flags++;
    g_ww_block_live--;
    free(self);
}

static void *fake_ww_block_get_type(void *self, void *out)
{
    struct fake_ww_block *block = self;
    cpp_string_construct(out, block->cell->type);
    return out;
}

static void fake_ww_block_set_data(void *self, void *block_data, bool physics)
{
    struct fake_ww_block *block = self;
    struct fake_ww_block_data *data = block_data;
    block->world->set_data_calls++;
    block->world->last_set_data = block_data;
    block->world->last_physics = physics;
    if (block->world->set_data_applies) {
        snprintf(block->cell->type, sizeof(block->cell->type), "%s",
                 data->type);
    }
}

static void fake_ww_block_data_delete(void *self, unsigned int flags)
{
    if (flags != 1) g_ww_bad_delete_flags++;
    g_ww_block_data_live--;
    free(self);
}

static struct fake_ww_block_data *fake_ww_new_block_data(const char *type,
                                                         int facing)
{
    struct fake_ww_block_data *data = calloc(1, sizeof(*data));
    data->vtable = g_ww_block_data_vtable;
    snprintf(data->type, sizeof(data->type), "%s", type);
    data->facing = facing;
    g_ww_block_data_live++;
    return data;
}

static void *fake_ww_create_block_data_states(void *self, void **out,
                                              void *type_string,
                                              void *states_ptr)
{
    (void)self;
    g_ww_create_states_calls++;
    snprintf(g_ww_states_type, sizeof(g_ww_states_type), "%s",
             cpp_string_str(type_string));
    struct es_block_states *states = states_ptr;
    struct es_block_state_node *sentinel = states->head;
    struct es_block_state_node *node =
        sentinel && sentinel->next != sentinel ? sentinel->next : nullptr;
    int shape_ok = states->max_load_factor == 1.0f && states->size == 1 &&
                   !states->vec_first && !states->vec_last &&
                   !states->vec_end && states->mask == 7 &&
                   states->maxidx == 8 && node && node != sentinel;
    if (node) {
        // The 16-char key must be a real heap string: _Mysize 16, _Myres 31.
        shape_ok = shape_ok && node->next == sentinel &&
                   node->prev == sentinel && sentinel->prev == node &&
                   node->variant_index == ES_BLOCK_STATE_WHICH_INT &&
                   strcmp(cpp_string_str(node->key), "facing_direction") == 0 &&
                   *(size_t *)(node->key + 16) == 16 &&
                   *(size_t *)(node->key + 24) == 31;
        g_ww_states_facing = *(int32_t *)node->variant_storage;
        // Emulate the callee-destroys contract with the shared heap.
        cpp_string_destroy(node->key);
        free(node);
    }
    free(sentinel);
    cpp_string_destroy(type_string); // SSO type id: no-op
    g_ww_states_shape_ok = shape_ok;
    *out = fake_ww_new_block_data(g_ww_states_type, g_ww_states_facing);
    return out;
}

static void *fake_ww_create_block_data_air(void *self, void **out,
                                           void *type_string)
{
    (void)self;
    g_ww_create_air_calls++;
    int is_air = strcmp(cpp_string_str(type_string), "minecraft:air") == 0;
    cpp_string_destroy(type_string);
    *out = fake_ww_new_block_data(is_air ? "minecraft:air" : "minecraft:bad",
                                  -1);
    return out;
}

static void *fake_ww_get_registry(void *self, void *name_string)
{
    (void)self;
    if (strcmp(cpp_string_str(name_string), "ItemType") != 0) return nullptr;
    return g_ww_registry_missing ? nullptr : &g_ww_registry;
}

static void *fake_ww_registry_get(void *self, struct es_identifier *identifier)
{
    (void)self;
    g_ww_registry_identifier_ok =
        identifier && identifier->ns_len == 9 && identifier->key_len == 10 &&
        memcmp(identifier->ns, "minecraft", 9) == 0 &&
        memcmp(identifier->key, "filled_map", 10) == 0;
    return g_ww_registry_identifier_ok ? (void *)&g_ww_item_type : nullptr;
}

static void fake_ww_impl_delete(void *self, unsigned int flags)
{
    if (flags != 1) g_ww_bad_delete_flags++;
    g_ww_impl_live--;
    free(self);
}

static struct fake_ww_item_impl *fake_ww_new_impl(int64_t map_id,
                                                  int has_map_id)
{
    struct fake_ww_item_impl *impl = calloc(1, sizeof(*impl));
    impl->vtable = g_ww_impl_vtable;
    impl->map_id = map_id;
    impl->has_map_id = has_map_id;
    g_ww_impl_live++;
    return impl;
}

static void *fake_ww_create_item_stack(void *self, void **out, int amount)
{
    (void)self;
    g_ww_create_stack_amount = amount;
    *out = fake_ww_new_impl(-1, 0);
    return out;
}

static void *fake_ww_impl_get_meta(void *self, void **out)
{
    struct fake_ww_item_impl *impl = self;
    struct fake_ww_item_meta *meta = calloc(1, sizeof(*meta));
    meta->vtable = g_ww_meta_vtable;
    meta->type = g_ww_meta_type;
    meta->has_map_id = impl->has_map_id;
    meta->map_id = impl->map_id;
    g_ww_meta_live++;
    *out = meta;
    return out;
}

static bool fake_ww_impl_set_meta(void *self, const void *meta_ptr)
{
    struct fake_ww_item_impl *impl = self;
    const struct fake_ww_item_meta *meta = meta_ptr;
    if (!g_ww_set_item_meta_result) return false;
    impl->has_map_id = meta->has_map_id;
    impl->map_id = meta->map_id;
    return true;
}

static void fake_ww_meta_delete(void *self, unsigned int flags)
{
    if (flags != 1) g_ww_bad_delete_flags++;
    g_ww_meta_live--;
    free(self);
}

static int fake_ww_meta_get_type(void *self)
{
    return ((struct fake_ww_item_meta *)self)->type;
}

static void fake_ww_meta_set_display_name(void *self, void *optional_ptr)
{
    (void)self;
    struct es_optional_string *parameter = optional_ptr;
    if (!parameter->has_value) return;
    snprintf(g_ww_last_display_name, sizeof(g_ww_last_display_name), "%s",
             cpp_string_str(parameter->value));
    // Emulate callee destruction of the by-value optional; for a >15 char
    // name this frees the caller's heap buffer through the shared heap.
    cpp_string_destroy(parameter->value);
    parameter->has_value = 0;
}

static void fake_ww_meta_set_lore(void *self, void *optional_ptr)
{
    (void)self;
    (void)optional_ptr;
    g_ww_set_lore_calls++;
}

static bool fake_ww_meta_has_map_id(void *self)
{
    return ((struct fake_ww_item_meta *)self)->has_map_id != 0;
}

static int64_t fake_ww_meta_get_map_id(void *self)
{
    return ((struct fake_ww_item_meta *)self)->map_id;
}

static int64_t fake_ww_map_view_get_id(void *self)
{
    (void)self;
    return g_ww_map_view_id;
}

static void fake_ww_meta_set_map_view(void *self, const void *map_view)
{
    struct fake_ww_item_meta *meta = self;
    meta->map_view = (void *)map_view;
    if (map_view) {
        // The measured setMapView reads MapView slot 1 getId internally.
        meta->map_id = ((int64_t (*)(void *))(
                            (*(void ***)map_view)[ES_MAPVIEW_SLOT_GET_ID]))(
                            (void *)map_view);
        meta->has_map_id = 1;
    }
}

static int fake_ww_inv_get_size(void *self)
{
    (void)self;
    return FAKE_WW_INV_SIZE;
}

static void *fake_ww_inv_get_item(void *self, void *out_ptr, int slot)
{
    struct fake_ww_inventory *inventory = self;
    struct es_optional_item_stack *out = out_ptr;
    if (slot < 0 || slot >= FAKE_WW_INV_SIZE || !inventory->occupied[slot]) {
        // The impl field is GARBAGE when the optional is empty; production
        // code crashes this test if it ever dereferences it.
        out->impl = (void *)(uintptr_t)0xDEADDEAD;
        out->has_value = 0;
        return out;
    }
    out->impl = fake_ww_new_impl(inventory->map_id[slot],
                                 inventory->map_id[slot] >= 0);
    out->has_value = 1;
    return out;
}

static void fake_ww_inv_set_item(void *self, int slot, void *param_ptr)
{
    struct fake_ww_inventory *inventory = self;
    struct es_optional_item_stack *parameter = param_ptr;
    if (!parameter->has_value || !parameter->impl) {
        g_ww_set_item_bad_param = 1;
        return;
    }
    struct fake_ww_item_impl *impl = parameter->impl;
    if (slot >= 0 && slot < FAKE_WW_INV_SIZE &&
        slot != inventory->drop_set_item_at) {
        inventory->occupied[slot] = 1;
        inventory->map_id[slot] = impl->has_map_id ? impl->map_id : -1;
    }
    // The callee consumes the by-value optional: the impl is moved into the
    // inventory and the caller's field is nulled.
    fake_ww_impl_delete(impl, 1);
    parameter->impl = nullptr;
    parameter->has_value = 0;
}

static void fake_ww_inv_clear_all(void *self)
{
    (void)self;
    g_ww_clear_all_calls++;
}

static void fake_ww_inv_clear_slot(void *self, int slot)
{
    struct fake_ww_inventory *inventory = self;
    inventory->clear_calls++;
    inventory->last_cleared = slot;
    if (slot >= 0 && slot < FAKE_WW_INV_SIZE) {
        inventory->occupied[slot] = 0;
        inventory->map_id[slot] = -1;
    }
}

static void setup_fake_write_world(struct fake_ww_server *server,
                                   struct fake_ww_player *player,
                                   struct fake_ww_world *world,
                                   struct fake_ww_inventory *inventory,
                                   struct fake_ww_map_view *map_view)
{
    memset(g_ww_server_vtable, 0, sizeof(g_ww_server_vtable));
    memset(g_ww_registry_vtable, 0, sizeof(g_ww_registry_vtable));
    memset(g_ww_item_type_vtable, 0, sizeof(g_ww_item_type_vtable));
    memset(g_ww_impl_vtable, 0, sizeof(g_ww_impl_vtable));
    memset(g_ww_meta_vtable, 0, sizeof(g_ww_meta_vtable));
    memset(g_ww_inventory_vtable, 0, sizeof(g_ww_inventory_vtable));
    memset(g_ww_player_vtable, 0, sizeof(g_ww_player_vtable));
    memset(g_ww_dimension_vtable, 0, sizeof(g_ww_dimension_vtable));
    memset(g_ww_block_vtable, 0, sizeof(g_ww_block_vtable));
    memset(g_ww_map_view_vtable, 0, sizeof(g_ww_map_view_vtable));

    g_ww_server_vtable[ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES] =
        (void *)fake_ww_create_block_data_states;
    g_ww_server_vtable[ES_SERVER_SLOT_CREATE_BLOCK_DATA] =
        (void *)fake_ww_create_block_data_air;
    g_ww_server_vtable[ES_SERVER_SLOT_GET_REGISTRY] =
        (void *)fake_ww_get_registry;
    g_ww_registry_vtable[ES_ITEM_REGISTRY_SLOT_GET] =
        (void *)fake_ww_registry_get;
    g_ww_item_type_vtable[ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK] =
        (void *)fake_ww_create_item_stack;
    g_ww_impl_vtable[ES_ITEM_STACK_SLOT_DELETE] = (void *)fake_ww_impl_delete;
    g_ww_impl_vtable[ES_ITEM_STACK_SLOT_GET_ITEM_META] =
        (void *)fake_ww_impl_get_meta;
    g_ww_impl_vtable[ES_ITEM_STACK_SLOT_SET_ITEM_META] =
        (void *)fake_ww_impl_set_meta;
    g_ww_meta_vtable[ES_ITEM_META_SLOT_DELETE] = (void *)fake_ww_meta_delete;
    g_ww_meta_vtable[ES_ITEM_META_SLOT_GET_TYPE] =
        (void *)fake_ww_meta_get_type;
    g_ww_meta_vtable[ES_ITEM_META_SLOT_SET_DISPLAY_NAME] =
        (void *)fake_ww_meta_set_display_name;
    g_ww_meta_vtable[ES_ITEM_META_SLOT_SET_LORE] =
        (void *)fake_ww_meta_set_lore;
    g_ww_meta_vtable[ES_MAP_META_SLOT_HAS_MAP_ID] =
        (void *)fake_ww_meta_has_map_id;
    g_ww_meta_vtable[ES_MAP_META_SLOT_GET_MAP_ID] =
        (void *)fake_ww_meta_get_map_id;
    g_ww_meta_vtable[ES_MAP_META_SLOT_SET_MAP_VIEW] =
        (void *)fake_ww_meta_set_map_view;
    g_ww_inventory_vtable[ES_INVENTORY_SLOT_GET_SIZE] =
        (void *)fake_ww_inv_get_size;
    g_ww_inventory_vtable[ES_INVENTORY_SLOT_GET_ITEM] =
        (void *)fake_ww_inv_get_item;
    g_ww_inventory_vtable[ES_INVENTORY_SLOT_SET_ITEM] =
        (void *)fake_ww_inv_set_item;
    g_ww_inventory_vtable[ES_INVENTORY_SLOT_CLEAR_ALL] =
        (void *)fake_ww_inv_clear_all;
    g_ww_inventory_vtable[ES_INVENTORY_SLOT_CLEAR_SLOT] =
        (void *)fake_ww_inv_clear_slot;
    g_ww_player_vtable[ES_PLAYER_SLOT_GET_LOCATION] =
        (void *)fake_ww_get_location;
    g_ww_player_vtable[ES_PLAYER_SLOT_GET_DIMENSION] =
        (void *)fake_ww_get_dimension;
    g_ww_player_vtable[ES_PLAYER_SLOT_GET_INVENTORY] =
        (void *)fake_ww_get_inventory;
    g_ww_dimension_vtable[ES_DIMENSION_SLOT_GET_NAME] =
        (void *)fake_ww_dimension_get_name;
    g_ww_dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ] =
        (void *)fake_ww_get_block;
    g_ww_block_vtable[0] = (void *)fake_ww_block_delete;
    g_ww_block_vtable[ES_BLOCK_SLOT_GET_TYPE] =
        (void *)fake_ww_block_get_type;
    g_ww_block_vtable[ES_BLOCK_SLOT_SET_DATA] =
        (void *)fake_ww_block_set_data;
    g_ww_block_data_vtable[ES_BLOCK_DATA_SLOT_DELETE] =
        (void *)fake_ww_block_data_delete;
    g_ww_map_view_vtable[ES_MAPVIEW_SLOT_GET_ID] =
        (void *)fake_ww_map_view_get_id;

    g_ww_registry.vtable = g_ww_registry_vtable;
    g_ww_item_type.vtable = g_ww_item_type_vtable;

    g_ww_block_live = 0;
    g_ww_block_data_live = 0;
    g_ww_impl_live = 0;
    g_ww_meta_live = 0;
    g_ww_bad_delete_flags = 0;
    g_ww_set_lore_calls = 0;
    g_ww_clear_all_calls = 0;
    g_ww_create_states_calls = 0;
    g_ww_create_air_calls = 0;
    g_ww_states_shape_ok = 0;
    g_ww_states_facing = -1;
    g_ww_states_type[0] = '\0';
    g_ww_registry_identifier_ok = 0;
    g_ww_registry_missing = 0;
    g_ww_create_stack_amount = 0;
    g_ww_set_item_meta_result = 1;
    g_ww_meta_type = ES_ITEM_META_TYPE_MAP;
    g_ww_map_view_id = 0;
    g_ww_set_item_bad_param = 0;
    g_ww_last_display_name[0] = '\0';

    memset(world, 0, sizeof(*world));
    world->vtable = g_ww_dimension_vtable;
    world->cell_count = 4;
    world->cells[0] = (struct fake_ww_cell){{0, 64, 0}, "minecraft:air"};
    world->cells[1] =
        (struct fake_ww_cell){{0, 64, 1}, "minecraft:quartz_block"};
    world->cells[2] = (struct fake_ww_cell){{1, 64, 0}, "minecraft:air"};
    world->cells[3] =
        (struct fake_ww_cell){{1, 64, 1}, "minecraft:quartz_block"};
    world->set_data_applies = 1;

    memset(inventory, 0, sizeof(*inventory));
    inventory->vtable = g_ww_inventory_vtable;
    inventory->drop_set_item_at = -1;
    for (int i = 0; i < FAKE_WW_INV_SIZE; i++) inventory->map_id[i] = -1;

    player->vtable = g_ww_player_vtable;
    player->world = world;
    player->inventory = inventory;

    server->vtable = g_ww_server_vtable;
    map_view->vtable = g_ww_map_view_vtable;
}

static struct presenter_viewer test_presenter_viewer(
    void *player, uint64_t stable_id, double x, double y, double z,
    const char *dimension)
{
    struct presenter_viewer viewer = {
        .player = player,
        .context = nullptr,
        .stable_id = stable_id,
        .x = x,
        .y = y,
        .z = z,
    };
    snprintf(viewer.dimension, sizeof(viewer.dimension), "%s", dimension);
    return viewer;
}

static int test_renderer_registration_callback_and_lifetime(void)
{
    void *server_vtable[ES_SERVER_SLOT_CREATE_MAP + 1] = {0};
    void *map_vtable[ES_MAPVIEW_SLOT_SET_LOCKED + 1] = {0};
    void *player_vtable[ES_PLAYER_SLOT_SEND_MAP + 1] = {0};
    struct { void **vtable; } server = { server_vtable };
    struct { void **vtable; } map = { map_vtable };
    struct { void **vtable; } player = { player_vtable };
    struct { void **vtable; } second_player = { player_vtable };
    server_vtable[ES_SERVER_SLOT_GET_MAP] = (void *)fake_server_get_map;
    server_vtable[ES_SERVER_SLOT_CREATE_MAP] = (void *)fake_create_map;
    map_vtable[ES_MAPVIEW_SLOT_GET_ID] = (void *)fake_get_map_id;
    map_vtable[ES_MAPVIEW_SLOT_ADD_RENDERER] =
        (void *)fake_map_add_renderer;
    map_vtable[ES_MAPVIEW_SLOT_REMOVE_RENDERER] =
        (void *)fake_map_remove_renderer;
    map_vtable[ES_MAPVIEW_SLOT_SET_LOCKED] = (void *)fake_set_locked;
    player_vtable[ES_PLAYER_SLOT_SEND_MAP] =
        (void *)fake_player_send_rendered_map;
    g_fake_map_view = &map;
    g_fake_map_locked = 0;
    g_fake_rendered_pixel = 0;
    g_fake_render_send_count = 0;
    g_fake_create_map_calls = 0;
    g_fake_get_map_calls = 0;
    g_fake_get_map_requested_id = 0;

    // The pure-C map creation path resolves the creator's world through the
    // measured Player/Dimension slots, so a resolvable fake player is
    // required (the old C++ bridge test used an opaque dummy pointer).
    struct fake_ww_server ww_server;
    struct fake_ww_player creator;
    struct fake_ww_world ww_world;
    struct fake_ww_inventory ww_inventory;
    struct fake_ww_map_view ww_view;
    setup_fake_write_world(&ww_server, &creator, &ww_world, &ww_inventory,
                           &ww_view);

    struct map_render_ctx context;
    map_render_init(&context, &server, nullptr);
    struct screen_registry registry;
    screen_registry_init(&registry);
    struct screen_geom geometry;
    struct screen_pos position = {0, 64, 0};
    EXPECT(screen_geom_validate(position, position, "minecraft:overworld",
                                "minecraft:overworld", SCREEN_FACE_SOUTH,
                                &geometry) == SCREEN_GEOM_OK,
           "construct 1x1 renderer test screen");
    int screen_index = -1;
    EXPECT(screen_registry_create(&registry, "lifecycle", "owner",
                                  &geometry, &screen_index) == SCREEN_OK,
           "create renderer test screen with a Surface");
    struct screen_entry *screen = registry.screens[screen_index];
    EXPECT(map_render_init_screen(&context, screen, &creator) == MAP_RENDER_OK,
           "register renderer through measured shared_ptr ABI");
    EXPECT(g_fake_create_map_calls == 1 && g_fake_get_map_calls == 0,
           "new screen allocates its map once");
    EXPECT(screen->tiles_initialized && screen->tiles[0].map_id == 777,
           "MapView registration records map id");
    EXPECT(g_fake_map_locked, "MapView is locked");

    struct map_renderer_stats stats;
    EXPECT(map_render_get_stats(screen, 0, &stats),
           "renderer statistics are available");
    EXPECT(stats.initialize_count == 1,
           "addRenderer executes initialize callback exactly once");
    EXPECT(stats.strong_references == 3,
           "screen owner plus Endstone renderer/canvas owners remain");

    void *players[] = { &player, &second_player };
    const char *player_ids[] = { "diagnostic-a", "diagnostic-b" };
    EXPECT(map_render_set_test_pattern(&context, screen, MAP_TEST_RED,
                                       players, player_ids, 2) == 0,
           "send fixed diagnostic pattern to supplied public viewers");
    EXPECT(g_fake_render_send_count == 2,
           "map layer sends to all supplied viewers without UUID filtering");
    EXPECT(g_fake_rendered_pixel == UINT32_C(0xff0000ff),
           "renderer callback writes red into measured canvas buffer");
    EXPECT(map_render_get_stats(screen, 0, &stats) &&
                stats.render_count == 2 && stats.last_player == &second_player,
           "renderer callback diagnostics record both supplied players");
    EXPECT(stats.last_canvas_valid && stats.last_canvas_pixels ==
               SCREEN_TILE_SIZE * SCREEN_TILE_SIZE,
           "renderer diagnostics validate the full 128x128 canvas buffer");
    EXPECT(stats.last_write_verified &&
               stats.last_first_pixel == UINT32_C(0xff0000ff) &&
               stats.last_last_pixel == UINT32_C(0xff0000ff),
           "renderer diagnostics verify the fixed-pattern canvas write");
    struct presenter_viewer viewers[] = {
        test_presenter_viewer(&player, 1, 0.5, 64.5, 2.0,
                              "minecraft:overworld"),
        test_presenter_viewer(&second_player, 2, 0.5, 64.5, 3.0,
                              "minecraft:overworld"),
    };
    struct presenter_stats presenter_stats;
    size_t frame_bytes = (size_t)SCREEN_TILE_SIZE * SCREEN_TILE_SIZE * 4;
    uint8_t *frame = malloc(frame_bytes);
    EXPECT(frame != nullptr, "allocate change-detection frame");
    memset(frame, 0x33, frame_bytes);
    EXPECT(map_render_submit_frame(screen, frame, frame_bytes,
                                   SURFACE_FORMAT_ABGR8888) == MAP_RENDER_OK,
           "submit producer frame into the Surface");
    memset(frame, 0x44, frame_bytes);
    int before = g_fake_render_send_count;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 1 && presenter_stats.sent == 1 &&
               g_fake_render_send_count == before + 2 &&
               g_fake_rendered_pixel == UINT32_C(0x33333333),
           "present copies exactly one Surface tile to both viewers");

    memset(frame, 0x33, frame_bytes);
    EXPECT(map_render_submit_frame(screen, frame, frame_bytes,
                                   SURFACE_FORMAT_ABGR8888) == MAP_RENDER_OK,
           "submit unchanged full frame");
    before = g_fake_render_send_count;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 0 && presenter_stats.sent == 0 &&
               g_fake_render_send_count == before,
           "unchanged full frame creates no dirty work or transmissions");

    frame[frame_bytes - 4] = 0x55; // Last pixel only.
    EXPECT(map_render_submit_frame(screen, frame, frame_bytes,
                                   SURFACE_FORMAT_ABGR8888) == MAP_RENDER_OK,
           "submit one-pixel change");
    before = g_fake_render_send_count;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 1 && presenter_stats.sent == 1 &&
               g_fake_render_send_count == before + 2,
           "one-pixel change sends only the affected tile to both viewers");

    struct presenter_resident_cursor cursor = {0};
    bool done = false;
    before = g_fake_render_send_count;
    EXPECT(map_render_resend(&context, screen, &viewers[0], &cursor, 1,
                             &done, &presenter_stats) == 0 &&
               presenter_stats.examined == 1 && presenter_stats.sent == 1 &&
               done && cursor.index == 1 &&
               g_fake_render_send_count == before + 1,
           "forced resend uses one viewer and resident cursor");

    map_render_clear(&context, screen);
    struct surface_stats surface_stats;
    EXPECT(surface_get_stats(screen->surface, &surface_stats) == SURFACE_OK &&
               surface_stats.generation == 0 &&
               surface_stats.resident_tiles == 0 &&
               surface_stats.pending_tiles == 0 &&
               surface_stats.allocated_pixel_bytes == 0,
           "clear resets Surface residents without retained tile history");
    memset(frame, 0x33, frame_bytes);
    EXPECT(map_render_submit_frame(screen, frame, frame_bytes,
                                   SURFACE_FORMAT_ABGR8888) == MAP_RENDER_OK,
           "submit frame after clear");
    before = g_fake_render_send_count;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.sent == 1 &&
               g_fake_render_send_count == before + 2,
           "clear invalidates content so the next frame is sent");
    free(frame);

    map_render_destroy_screen(&context, screen);
    EXPECT(!screen->tiles_initialized && screen->tiles[0].renderer == nullptr,
           "screen destruction removes renderer and releases final owner");
    EXPECT(screen->tiles[0].map_id_valid && screen->tiles[0].map_id == 777,
           "runtime destruction preserves persistent screen map identity");
    EXPECT(map_render_init_screen(&context, screen, &creator) ==
               MAP_RENDER_OK,
           "restart resolves the persistent MapView");
    EXPECT(g_fake_create_map_calls == 1 && g_fake_get_map_calls == 1 &&
               g_fake_get_map_requested_id == 777,
           "restart reuses stored map id instead of allocating a new map");
    map_render_destroy_screen(&context, screen);
    screen_registry_cleanup(&registry);
    return 1;
}

// ----------------------------------------------------------------
// Multi-map fakes: one fake MapView per tile, so per-tile send counts
// and rendered windows are observable on a multi-tile screen.
// ----------------------------------------------------------------

#define FAKE_MULTI_MAPS (SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT)

struct fake_multi_map {
    void **vtable;
    struct es_msvc_shared_ptr renderer;
    int64_t id;
    int send_count;
    uint32_t first_pixel;
};

static struct fake_multi_map g_multi_maps[FAKE_MULTI_MAPS];
static int g_multi_map_count;
static int g_multi_send_total;

static void *fake_multi_create_map(void *self, void *dimension)
{
    (void)self;
    (void)dimension;
    if (g_multi_map_count >= FAKE_MULTI_MAPS) return nullptr;
    struct fake_multi_map *map = &g_multi_maps[g_multi_map_count];
    map->id = 900 + g_multi_map_count++;
    return map;
}

static int64_t fake_multi_get_id(void *self)
{
    return ((struct fake_multi_map *)self)->id;
}

static void fake_multi_add_renderer(void *self,
                                    struct es_msvc_shared_ptr *shared)
{
    struct fake_multi_map *map = self;
    map->renderer = *shared;
    es_msvc_shared_ptr_add_ref(shared); // renderers_ copy
    es_msvc_shared_ptr_add_ref(shared); // canvases_ key copy
    void **renderer_vtable = *(void ***)shared->ptr;
    ((void (*)(void *, void *))renderer_vtable[ES_MAPRENDERER_SLOT_INIT])(
        shared->ptr, self);
    es_msvc_shared_ptr_release(shared); // destroy by-value parameter
}

static bool fake_multi_remove_renderer(void *self,
                                       const struct es_msvc_shared_ptr *shared)
{
    struct fake_multi_map *map = self;
    struct es_msvc_shared_ptr renderers_copy = *shared;
    struct es_msvc_shared_ptr canvases_copy = *shared;
    es_msvc_shared_ptr_release(&renderers_copy);
    es_msvc_shared_ptr_release(&canvases_copy);
    map->renderer.ptr = nullptr;
    map->renderer.control = nullptr;
    return true;
}

static void fake_multi_set_locked(void *self, bool locked)
{
    (void)self;
    (void)locked;
}

static void fake_multi_send_map(void *self, void *map_object)
{
    struct fake_multi_map *map = map_object;
    map->send_count++;
    g_multi_send_total++;
    uint32_t pixels[SCREEN_TILE_SIZE * SCREEN_TILE_SIZE] = {0};
    struct {
        void **vtable;
        uint32_t *begin;
        uint32_t *end;
        uint32_t *capacity;
    } canvas = { nullptr, pixels, pixels + SCREEN_TILE_SIZE * SCREEN_TILE_SIZE,
                 pixels + SCREEN_TILE_SIZE * SCREEN_TILE_SIZE };
    void **renderer_vtable = *(void ***)map->renderer.ptr;
    ((void (*)(void *, void *, void *, void *))
        renderer_vtable[ES_MAPRENDERER_SLOT_RENDER])(
            map->renderer.ptr, map_object, &canvas, self);
    map->first_pixel = pixels[0];
}

static void fill_tile_window(uint32_t *frame, int pixel_width, int col,
                             int row, uint32_t value)
{
    for (int y = 0; y < SCREEN_TILE_SIZE; y++) {
        for (int x = 0; x < SCREEN_TILE_SIZE; x++) {
            frame[(size_t)(row * SCREEN_TILE_SIZE + y) * pixel_width +
                  (size_t)col * SCREEN_TILE_SIZE + x] = value;
        }
    }
}

static int test_render_multi_tile_dirty_tracking(void)
{
    void *server_vtable[ES_SERVER_SLOT_CREATE_MAP + 1] = {0};
    void *map_vtable[ES_MAPVIEW_SLOT_SET_LOCKED + 1] = {0};
    void *player_vtable[ES_PLAYER_SLOT_SEND_MAP + 1] = {0};
    struct { void **vtable; } server = { server_vtable };
    struct { void **vtable; } viewer_a = { player_vtable };
    struct { void **vtable; } viewer_b = { player_vtable };
    server_vtable[ES_SERVER_SLOT_CREATE_MAP] = (void *)fake_multi_create_map;
    map_vtable[ES_MAPVIEW_SLOT_GET_ID] = (void *)fake_multi_get_id;
    map_vtable[ES_MAPVIEW_SLOT_ADD_RENDERER] =
        (void *)fake_multi_add_renderer;
    map_vtable[ES_MAPVIEW_SLOT_REMOVE_RENDERER] =
        (void *)fake_multi_remove_renderer;
    map_vtable[ES_MAPVIEW_SLOT_SET_LOCKED] = (void *)fake_multi_set_locked;
    player_vtable[ES_PLAYER_SLOT_SEND_MAP] = (void *)fake_multi_send_map;

    memset(g_multi_maps, 0, sizeof(g_multi_maps));
    g_multi_map_count = 0;
    g_multi_send_total = 0;
    for (int i = 0; i < FAKE_MULTI_MAPS; i++) {
        g_multi_maps[i].vtable = map_vtable;
    }

    struct fake_ww_server ww_server;
    struct fake_ww_player creator;
    struct fake_ww_world ww_world;
    struct fake_ww_inventory ww_inventory;
    struct fake_ww_map_view ww_view;
    setup_fake_write_world(&ww_server, &creator, &ww_world, &ww_inventory,
                           &ww_view);

    struct map_render_ctx context;
    map_render_init(&context, &server, nullptr);
    struct screen_registry registry;
    screen_registry_init(&registry);
    struct screen_geom geometry;
    EXPECT(screen_geom_validate((struct screen_pos){0, 65, 0},
                                (struct screen_pos){2, 64, 0},
                                "minecraft:overworld", "minecraft:overworld",
                                SCREEN_FACE_SOUTH,
                                &geometry) == SCREEN_GEOM_OK,
           "construct 3x2 renderer test screen");
    int screen_index = -1;
    EXPECT(screen_registry_create(&registry, "grid", "owner", &geometry,
                                  &screen_index) == SCREEN_OK,
           "create 3x2 renderer test screen with a Surface");
    struct screen_entry *screen = registry.screens[screen_index];
    EXPECT(map_render_init_screen(&context, screen, &creator) ==
               MAP_RENDER_OK,
           "register a renderer per tile");
    const int tile_count = screen_geom_tile_count(&screen->geom);
    EXPECT(tile_count == 6 && g_multi_map_count == 6,
           "3x2 screen allocates six distinct maps");
    for (int i = 0; i < tile_count; i++) {
        EXPECT(screen->tiles[i].map_view == &g_multi_maps[i] &&
                   screen->tiles[i].map_id == g_multi_maps[i].id,
               "tiles keep their own map identity");
    }

    int pixel_width = screen_geom_pixel_width(&screen->geom);
    int pixel_height = screen_geom_pixel_height(&screen->geom);
    size_t frame_bytes = (size_t)pixel_width * pixel_height * 4;
    uint32_t *frame = malloc(frame_bytes);
    EXPECT(frame != nullptr, "allocate 3x2 frame");
    for (int i = 0; i < tile_count; i++) {
        fill_tile_window(frame, pixel_width, i % 3, i / 3,
                         UINT32_C(0xff000000) | (uint32_t)(i + 1));
    }

    struct presenter_viewer viewers[] = {
        test_presenter_viewer(&viewer_a, 1, 1.5, 65.5, 2.0,
                              "minecraft:overworld"),
        test_presenter_viewer(&viewer_b, 2, 1.5, 65.5, 3.0,
                              "minecraft:overworld"),
    };
    struct presenter_stats presenter_stats;
    EXPECT(map_render_submit_frame(screen, (const uint8_t *)frame,
                                   frame_bytes, SURFACE_FORMAT_ABGR8888) ==
               MAP_RENDER_OK,
           "submit initial multi-tile frame");
    EXPECT(map_render_present(&context, screen, viewers, 2, 2,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 2 && presenter_stats.sent == 2,
           "presenter budget carries dirty tiles to the next call");
    struct surface_stats surface_stats;
    EXPECT(surface_get_stats(screen->surface, &surface_stats) == SURFACE_OK &&
               surface_stats.pending_tiles == 4,
           "four dirty tiles remain after the first budget");
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 4 && presenter_stats.sent == 4 &&
               g_multi_send_total == 2 * tile_count,
           "carryover drains and both eligible viewers receive each tile");
    for (int i = 0; i < tile_count; i++) {
        EXPECT(g_multi_maps[i].send_count == 2,
               "each map is sent exactly once per viewer");
        EXPECT(g_multi_maps[i].first_pixel ==
                   (UINT32_C(0xff000000) | (uint32_t)(i + 1)),
               "each renderer extracts its own tile window");
        struct surface_tile_view tile = {0};
        EXPECT(surface_read_tile(screen->surface, (uint32_t)(i % 3),
                                 (uint32_t)(i / 3), &tile) == SURFACE_OK &&
                   ((const uint32_t *)tile.pixels)[0] ==
                       (UINT32_C(0xff000000) | (uint32_t)(i + 1)),
               "Surface stores each extracted tile window");
    }

    // Change exactly one tile: (col 2, row 0) = index 2.
    fill_tile_window(frame, pixel_width, 2, 0, UINT32_C(0xff123456));
    EXPECT(map_render_submit_frame(screen, (const uint8_t *)frame,
                                   frame_bytes, SURFACE_FORMAT_ABGR8888) ==
               MAP_RENDER_OK,
           "submit one-tile change");
    int before = g_multi_send_total;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 1 && presenter_stats.sent == 1 &&
               g_multi_send_total == before + 2,
           "a one-tile change sends viewers x 1 maps, not viewers x tiles");
    EXPECT(g_multi_maps[2].send_count == 4 &&
               g_multi_maps[2].first_pixel == UINT32_C(0xff123456),
           "only the changed tile is retransmitted, with its new window");
    struct surface_tile_view changed_tile = {0};
    EXPECT(surface_read_tile(screen->surface, 2, 0, &changed_tile) ==
               SURFACE_OK &&
               ((const uint32_t *)changed_tile.pixels)[0] ==
                   UINT32_C(0xff123456),
           "the changed Surface tile is refreshed");
    for (int i = 0; i < tile_count; i++) {
        if (i == 2) continue;
        EXPECT(g_multi_maps[i].send_count == 2,
               "untouched tiles are not resent");
        struct surface_tile_view tile = {0};
        EXPECT(surface_read_tile(screen->surface, (uint32_t)(i % 3),
                                 (uint32_t)(i / 3), &tile) == SURFACE_OK &&
                   ((const uint32_t *)tile.pixels)[0] ==
                       (UINT32_C(0xff000000) | (uint32_t)(i + 1)),
               "untouched Surface tiles remain unchanged");
    }

    EXPECT(map_render_submit_frame(screen, (const uint8_t *)frame,
                                   frame_bytes, SURFACE_FORMAT_ABGR8888) ==
               MAP_RENDER_OK,
           "submit identical multi-tile frame");
    before = g_multi_send_total;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.examined == 0 && presenter_stats.sent == 0 &&
               g_multi_send_total == before,
           "an identical full frame sends nothing");

    // The mirrored tile (col 0, row 1) = index 3 must be the one dirtied:
    // a tile_col/tile_row transposition would dirty (col 1, row 0) instead.
    fill_tile_window(frame, pixel_width, 0, 1, UINT32_C(0xff654321));
    EXPECT(map_render_submit_frame(screen, (const uint8_t *)frame,
                                   frame_bytes, SURFACE_FORMAT_ABGR8888) ==
               MAP_RENDER_OK,
           "submit mirrored tile change");
    before = g_multi_send_total;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               g_multi_send_total == before + 2 &&
               g_multi_maps[3].send_count == 4 &&
               g_multi_maps[3].first_pixel == UINT32_C(0xff654321),
           "row-major tile addressing dirties the correct tile");
    EXPECT(g_multi_maps[1].send_count == 2,
           "the transposed tile stays clean");

    map_render_clear(&context, screen);
    EXPECT(surface_get_stats(screen->surface, &surface_stats) == SURFACE_OK &&
               surface_stats.generation == 0 && surface_stats.resident_tiles == 0 &&
               surface_stats.pending_tiles == 0 &&
               surface_stats.allocated_pixel_bytes == 0,
           "clear resets multi-tile Surface history");
    memset(frame, 0, frame_bytes);
    fill_tile_window(frame, pixel_width, 2, 1, UINT32_C(0xffabcdef));
    EXPECT(map_render_submit_frame(screen, (const uint8_t *)frame,
                                   frame_bytes, SURFACE_FORMAT_ABGR8888) ==
               MAP_RENDER_OK,
           "submit sparse resident frame");
    before = g_multi_send_total;
    EXPECT(map_render_present(&context, screen, viewers, 2, 8,
                              &presenter_stats) == MAP_RENDER_OK &&
               presenter_stats.sent == 1 && g_multi_send_total == before + 2,
           "sparse frame presents its one resident tile");
    struct presenter_resident_cursor cursor = {0};
    bool done = false;
    before = g_multi_send_total;
    EXPECT(map_render_resend(&context, screen, &viewers[0], &cursor, 1,
                             &done, &presenter_stats) == 0 && done &&
               cursor.index == 1 && presenter_stats.examined == 1 &&
               presenter_stats.sent == 1 && g_multi_send_total == before + 1 &&
               g_multi_maps[5].first_pixel == UINT32_C(0xffabcdef),
           "forced resend walks sparse residents by cursor");

    free(frame);
    map_render_destroy_screen(&context, screen);
    screen_registry_cleanup(&registry);
    return 1;
}

struct fake_world_block {
    void **vtable;
    void *block_source;
    const char *type;
};

struct fake_world_dimension {
    void **vtable;
    struct fake_world_block *air;
    struct fake_world_block *support;
    struct fake_world_block *frame;
};

struct fake_world_player {
    void **vtable;
    struct fake_world_dimension *dimension;
};

static void *g_world_location_this;
static void *g_world_location_out;
static void *g_world_dimension_this;
static void *g_world_name_this;
static void *g_world_name_out;
static void *g_world_block_this;
static void *g_world_block_out;
static void *g_world_type_this;
static void *g_world_type_out;
static int g_world_block_delete_count;
static unsigned int g_world_block_delete_flags;

static void *fake_world_get_location(void *self, struct es_location *out)
{
    struct fake_world_player *player = self;
    g_world_location_this = self;
    g_world_location_out = out;
    memset(out, 0, sizeof(*out));
    out->dimension = player->dimension;
    out->x = 10.75f;
    out->y = 64.25f;
    out->z = -2.25f;
    out->pitch = 12.5f;
    out->yaw = -90.0f;
    return out;
}

static void *fake_world_get_dimension(void *self)
{
    g_world_dimension_this = self;
    return ((struct fake_world_player *)self)->dimension;
}

static void *fake_world_get_name(void *self, void *out)
{
    g_world_name_this = self;
    g_world_name_out = out;
    cpp_string_construct(out, "minecraft:overworld");
    return out;
}

static void *fake_world_get_block(void *self, void **out,
                                  int x, int y, int z)
{
    struct fake_world_dimension *dimension = self;
    (void)y;
    (void)z;
    g_world_block_this = self;
    g_world_block_out = out;
    if (x == 20) *out = dimension->support;
    else if (x == 30) *out = dimension->frame;
    else *out = dimension->air;
    return out;
}

static void *fake_world_get_type(void *self, void *out)
{
    struct fake_world_block *block = self;
    g_world_type_this = self;
    g_world_type_out = out;
    cpp_string_construct(out, block->type);
    return out;
}

static void fake_world_block_delete(void *self, unsigned int flags)
{
    (void)self;
    g_world_block_delete_count++;
    g_world_block_delete_flags = flags;
}

static void setup_fake_world(struct fake_world_player *player,
                             struct fake_world_dimension *dimension,
                             struct fake_world_block *air,
                             struct fake_world_block *support,
                             struct fake_world_block *frame,
                             void **player_vtable, void **dimension_vtable,
                             void **block_vtable, void *block_source)
{
    memset(player_vtable, 0,
           sizeof(void *) * (ES_PLAYER_SLOT_GET_DIMENSION + 1));
    memset(dimension_vtable, 0,
           sizeof(void *) * (ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ + 1));
    memset(block_vtable, 0,
           sizeof(void *) * (ES_BLOCK_SLOT_GET_TYPE + 1));
    player_vtable[ES_PLAYER_SLOT_GET_LOCATION] =
        (void *)fake_world_get_location;
    player_vtable[ES_PLAYER_SLOT_GET_DIMENSION] =
        (void *)fake_world_get_dimension;
    dimension_vtable[ES_DIMENSION_SLOT_GET_NAME] =
        (void *)fake_world_get_name;
    dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ] =
        (void *)fake_world_get_block;
    block_vtable[0] = (void *)fake_world_block_delete;
    block_vtable[ES_BLOCK_SLOT_GET_TYPE] = (void *)fake_world_get_type;
    *air = (struct fake_world_block){block_vtable, block_source,
                                    "minecraft:air"};
    *support = (struct fake_world_block){block_vtable, block_source,
                                        "minecraft:quartz_block"};
    *frame = (struct fake_world_block){block_vtable, block_source,
                                      "minecraft:frame"};
    *dimension = (struct fake_world_dimension){
        dimension_vtable, air, support, frame};
    *player = (struct fake_world_player){player_vtable, dimension};
}

static int test_world_read_abi_snapshot_and_failure_safety(void)
{
    void *player_vtable[ES_PLAYER_SLOT_GET_DIMENSION + 1];
    void *dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ + 1];
    void *block_vtable[ES_BLOCK_SLOT_GET_TYPE + 1];
    void *source_vtable[] = {(void *)0x1};
    struct { void **vtable; } source = {source_vtable};
    struct fake_world_player player;
    struct fake_world_dimension dimension;
    struct fake_world_block air, support, frame;
    setup_fake_world(&player, &dimension, &air, &support, &frame,
                     player_vtable, dimension_vtable, block_vtable, &source);

    struct mp_player_snapshot snapshot;
    struct mp_world_c_trace trace;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    EXPECT(mp_world_c_debug_player_get_snapshot(&player, &snapshot, &trace,
                                                nullptr, 0),
           "pure-C snapshot dispatches measured world ABI");
    EXPECT(g_world_location_this == &player &&
               g_world_dimension_this == &player,
           "snapshot virtuals receive the exact borrowed Player this pointer");
    EXPECT(g_world_location_out != nullptr && g_world_name_out != nullptr &&
               g_world_name_this == &dimension,
           "hidden return buffers and Dimension this are supplied");
    EXPECT(snapshot.x == 10.75f && snapshot.y == 64.25f &&
               snapshot.z == -2.25f && snapshot.pitch == 12.5f &&
               snapshot.yaw == -90.0f,
           "Location POD fields use measured offsets");
    EXPECT(snapshot.block_x == 10 && snapshot.block_y == 64 &&
               snapshot.block_z == -3,
           "negative and positive block coordinates use floor semantics");
    EXPECT(strcmp(snapshot.dimension_id, "minecraft:overworld") == 0 &&
               trace.dimension == &dimension,
           "snapshot copies the dimension name and retains no C++ object");
    struct mp_player_snapshot production_snapshot = {0};
    EXPECT(mp_player_get_snapshot(&player, &production_snapshot, nullptr, 0) &&
               memcmp(&production_snapshot, &snapshot, sizeof(snapshot)) == 0,
           "production snapshot symbol uses the measured C implementation");

    memset(&snapshot, 0xa5, sizeof(snapshot));
    memset(&trace, 0xa5, sizeof(trace));
    EXPECT(!mp_world_c_debug_player_get_snapshot(nullptr, &snapshot, &trace,
                                                 nullptr, 0),
           "null Player fails closed");
    struct mp_player_snapshot zero_snapshot = {0};
    struct mp_world_c_trace zero_trace = {0};
    EXPECT(memcmp(&snapshot, &zero_snapshot, sizeof(snapshot)) == 0 &&
               memcmp(&trace, &zero_trace, sizeof(trace)) == 0,
           "failure zeroes all snapshot and trace outputs");
    EXPECT(player.vtable == player_vtable && dimension.vtable == dimension_vtable,
           "borrowed Player and Dimension objects are not destroyed");
    return 1;
}

static int test_world_read_abi_block_lifetime_and_policy(void)
{
    void *player_vtable[ES_PLAYER_SLOT_GET_DIMENSION + 1];
    void *dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ + 1];
    void *block_vtable[ES_BLOCK_SLOT_GET_TYPE + 1];
    void *source_vtable[] = {(void *)0x1};
    struct { void **vtable; } source = {source_vtable};
    struct fake_world_player player;
    struct fake_world_dimension dimension;
    struct fake_world_block air, support, frame;
    setup_fake_world(&player, &dimension, &air, &support, &frame,
                     player_vtable, dimension_vtable, block_vtable, &source);

    g_world_block_delete_count = 0;
    g_world_block_delete_flags = 0;
    struct mp_world_block_probe probe;
    struct mp_world_c_trace trace;
    enum mp_world_result result = mp_world_c_debug_probe_block(
        &player, "minecraft:overworld", (struct screen_pos){10, 64, 0},
        &probe, &trace, nullptr, 0);
    EXPECT(result == MP_WORLD_OK && probe.block_found && probe.is_air &&
               !probe.support_candidate,
           "air block is classified by the policy layer");
    EXPECT(probe.block == nullptr && trace.block_address == &air,
           "temporary Block never escapes while diagnostics retain its old address");
    EXPECT(g_world_block_this == &dimension && g_world_block_out != nullptr &&
               g_world_type_this == &air && g_world_type_out != nullptr,
           "block and string hidden-return calls receive measured arguments");
    EXPECT(g_world_block_delete_count == 1 &&
               g_world_block_delete_flags == 1 &&
               trace.block_destroy_count == 1,
           "returned unique_ptr Block is scalar-deleted exactly once");
    EXPECT(probe.block_source == &source &&
               probe.block_source_vptr == source_vtable,
           "verified EndstoneBlock +8 BlockSource field is reported");

    struct mp_world_block_probe production_probe = {0};
    result = mp_world_probe_block(
        &player, "minecraft:overworld", (struct screen_pos){20, 64, 0},
        &production_probe, nullptr, 0);
    EXPECT(result == MP_WORLD_OK && production_probe.support_candidate &&
               strcmp(production_probe.block_type,
                      "minecraft:quartz_block") == 0 &&
               g_world_block_delete_count == 2,
           "production block-probe symbol uses C and destroys its temporary once");

    result = mp_world_c_debug_probe_block(
        &player, "minecraft:the_nether", (struct screen_pos){20, 64, 0},
        &probe, &trace, nullptr, 0);
    EXPECT(result == MP_WORLD_BAD_ARGUMENT && !probe.block_found &&
               trace.block_destroy_count == 0,
           "different dimension is rejected before block lookup");
    EXPECT(mp_world_c_type_is_support_candidate("minecraft:quartz_block") &&
               !mp_world_c_type_is_support_candidate("minecraft:water") &&
               mp_world_c_type_is_air("minecraft:void_air"),
           "air and backing policy remains independent of ABI dispatch");
    return 1;
}

static int test_world_read_abi_validation_inspection_and_map_lookup(void)
{
    void *player_vtable[ES_PLAYER_SLOT_GET_DIMENSION + 1];
    void *dimension_vtable[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ + 1];
    void *block_vtable[ES_BLOCK_SLOT_GET_TYPE + 1];
    void *source_vtable[] = {(void *)0x1};
    struct { void **vtable; } source = {source_vtable};
    struct fake_world_player player;
    struct fake_world_dimension dimension;
    struct fake_world_block air, support, frame;
    setup_fake_world(&player, &dimension, &air, &support, &frame,
                     player_vtable, dimension_vtable, block_vtable, &source);

    g_world_block_delete_count = 0;
    EXPECT(mp_world_validate_empty_tile(
               &player, "minecraft:overworld",
               (struct screen_pos){10, 64, 0},
               (struct screen_pos){20, 64, 0}, SCREEN_FACE_NORTH,
               nullptr, 0) == MP_WORLD_OK,
           "transaction validation accepts air plus a support candidate");
    EXPECT(g_world_block_delete_count == 2,
           "validation destroys both temporary Blocks exactly once");
    EXPECT(mp_world_validate_empty_tile(
               &player, "minecraft:overworld",
               (struct screen_pos){20, 64, 0},
               (struct screen_pos){20, 64, 0}, SCREEN_FACE_NORTH,
               nullptr, 0) == MP_WORLD_CELL_NOT_AIR,
           "validation rejects an occupied cell");
    EXPECT(mp_world_validate_empty_tile(
               &player, "minecraft:overworld",
               (struct screen_pos){10, 64, 0},
               (struct screen_pos){10, 64, 0}, SCREEN_FACE_NORTH,
               nullptr, 0) == MP_WORLD_BACKING_NOT_SOLID,
           "validation rejects an air backing block");

    struct mp_world_tile_state state;
    EXPECT(mp_world_inspect_tile(
               &player, "minecraft:overworld",
               (struct screen_pos){30, 64, 0},
               (struct screen_pos){20, 64, 0}, 777, &state,
               nullptr, 0) == MP_WORLD_MAP_ID_UNVERIFIABLE &&
               state.frame_present && state.backing_is_solid,
           "inspection matches C++ v0.11 frame/map-id semantics");

    void *server_vtable[ES_SERVER_SLOT_GET_MAP + 1] = {0};
    struct { void **vtable; } server = {server_vtable};
    int map;
    g_fake_map_view = &map;
    g_fake_get_map_calls = 0;
    g_fake_get_map_requested_id = 0;
    server_vtable[ES_SERVER_SLOT_GET_MAP] = (void *)fake_server_get_map;
    EXPECT(mp_world_get_map(&server, 1234567) == &map &&
               g_fake_get_map_calls == 1 &&
               g_fake_get_map_requested_id == 1234567,
           "MapView lookup reuses the verified Server ABI adapter");
    EXPECT(mp_world_get_map(nullptr, 1) == nullptr,
           "MapView lookup rejects a null borrowed Server");
    server_vtable[ES_SERVER_SLOT_GET_MAP] = nullptr;
    EXPECT(mp_world_get_map(&server, 1) == nullptr,
           "MapView lookup rejects a missing verified virtual target");
    return 1;
}

// ================================================================
// PURE-C WORLD WRITE ABI TESTS
// ================================================================

static int test_world_write_prepare_builds_frame_and_map_item(void)
{
    struct fake_ww_server server;
    struct fake_ww_player player;
    struct fake_ww_world world;
    struct fake_ww_inventory inventory;
    struct fake_ww_map_view map_view;
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_map_view_id = 4242;

    struct mp_world_prepared_tile *prepared = nullptr;
    char detail[192] = {0};
    enum mp_world_result result = mp_world_prepare_tile(
        &server, &player, "minecraft:overworld",
        (struct screen_pos){0, 64, 0}, (struct screen_pos){0, 64, 1},
        SCREEN_FACE_NORTH, &map_view, 4242, "demo", 0, 2, 0, 0,
        &prepared, detail, (int)sizeof(detail));
    EXPECT(result == MP_WORLD_OK && prepared,
           "prepare succeeds against the full fake write world");
    EXPECT(g_ww_create_states_calls == 1 && g_ww_states_shape_ok,
           "createBlockData receives the exact measured fake BlockStates shape");
    EXPECT(strcmp(g_ww_states_type, "minecraft:frame") == 0 &&
               g_ww_states_facing == 2,
           "frame block data uses SSO type id and NORTH facing state 2");
    EXPECT(g_ww_registry_identifier_ok && g_ww_create_stack_amount == 1,
           "registry Identifier holds minecraft/filled_map string_views");
    EXPECT(strcmp(g_ww_last_display_name,
                  "MediaPlayer demo - tile 1/2 (row 1, col 1)") == 0,
           "display name matches the C++ reference format");
    EXPECT(g_ww_set_lore_calls == 0,
           "setLore is deliberately never invoked by the pure-C path");
    EXPECT(g_ww_meta_live == 0,
           "prepare destroys its owned ItemMeta exactly once with flag 1");
    EXPECT(g_ww_impl_live == 1 && g_ww_block_data_live == 1,
           "prepared tile owns exactly one impl and one BlockData");
    EXPECT(g_ww_block_live == 0,
           "validation destroys every temporary Block");
    EXPECT(g_ww_bad_delete_flags == 0, "all deletes used flag 1");

    mp_world_prepared_destroy(prepared);
    EXPECT(g_ww_impl_live == 0 && g_ww_block_data_live == 0 &&
               g_ww_bad_delete_flags == 0,
           "prepared_destroy scalar-deletes the impl and BlockData once each");

    static const struct {
        enum screen_facing facing;
        int state;
    } facing_cases[] = {
        {SCREEN_FACE_NORTH, 2},
        {SCREEN_FACE_SOUTH, 3},
        {SCREEN_FACE_WEST, 4},
        {SCREEN_FACE_EAST, 5},
    };
    for (int i = 0; i < 4; i++) {
        prepared = nullptr;
        result = mp_world_prepare_tile(
            &server, &player, "minecraft:overworld",
            (struct screen_pos){0, 64, 0}, (struct screen_pos){0, 64, 1},
            facing_cases[i].facing, &map_view, 4242, "demo", 0, 1, 0, 0,
            &prepared, nullptr, 0);
        EXPECT(result == MP_WORLD_OK && prepared &&
                   g_ww_states_facing == facing_cases[i].state,
               "facing maps onto the measured facing_direction value");
        mp_world_prepared_destroy(prepared);
    }
    EXPECT(g_ww_impl_live == 0 && g_ww_block_data_live == 0 &&
               g_ww_block_live == 0,
           "facing sweep leaks no fake runtime objects");
    return 1;
}

static int test_world_write_prepare_failure_paths(void)
{
    struct fake_ww_server server;
    struct fake_ww_player player;
    struct fake_ww_world world;
    struct fake_ww_inventory inventory;
    struct fake_ww_map_view map_view;
    struct mp_world_prepared_tile *prepared = nullptr;
    char detail[192];
    struct screen_pos cell = {0, 64, 0};
    struct screen_pos backing = {0, 64, 1};

    // Non-map meta: the getType()==3 downcast must reject and free all.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_meta_type = 0;
    detail[0] = '\0';
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 7, "demo", 0, 1, 0, 0, &prepared, detail,
                                 (int)sizeof(detail)) ==
                   MP_WORLD_MAP_ITEM_FAILED &&
               !prepared,
           "non-map ItemMeta type fails the as<MapMeta> downcast");
    EXPECT(g_ww_impl_live == 0 && g_ww_meta_live == 0 &&
               g_ww_block_data_live == 0 && g_ww_bad_delete_flags == 0,
           "downcast failure frees impl, meta and BlockData exactly once");

    // Map id mismatch between the MapView and the requested id.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_map_view_id = 777;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 888, "demo", 0, 1, 0, 0, &prepared,
                                 nullptr, 0) == MP_WORLD_MAP_ID_MISMATCH,
           "map id mismatch is detected before any placement");
    EXPECT(g_ww_impl_live == 0 && g_ww_meta_live == 0 &&
               g_ww_block_data_live == 0,
           "map id mismatch leaks nothing");

    // setItemMeta returning false is also a mismatch, as in C++.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_map_view_id = 55;
    g_ww_set_item_meta_result = 0;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 55, "demo", 0, 1, 0, 0, &prepared,
                                 nullptr, 0) == MP_WORLD_MAP_ID_MISMATCH,
           "rejected setItemMeta reports a map id mismatch");

    // Missing ItemType registry.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_registry_missing = 1;
    detail[0] = '\0';
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 7, "demo", 0, 1, 0, 0, &prepared, detail,
                                 (int)sizeof(detail)) ==
               MP_WORLD_MAP_ITEM_FAILED,
           "missing registry fails map item creation");
    EXPECT(strcmp(detail, "ItemType registry is unavailable") == 0 &&
               g_ww_block_data_live == 0,
           "registry failure reports its detail and frees the frame data");

    // Occupied cell: validation runs before any block data creation.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    snprintf(world.cells[0].type, sizeof(world.cells[0].type),
             "minecraft:oak_planks");
    detail[0] = '\0';
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 7, "demo", 0, 1, 0, 0, &prepared, detail,
                                 (int)sizeof(detail)) == MP_WORLD_CELL_NOT_AIR,
           "occupied cell is rejected");
    EXPECT(g_ww_create_states_calls == 0 &&
               strcmp(detail, "screen cell is not air") == 0,
           "validation precedes createBlockData exactly like the C++ order");

    // Fluid backing.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    snprintf(world.cells[1].type, sizeof(world.cells[1].type),
             "minecraft:water");
    detail[0] = '\0';
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 7, "demo", 0, 1, 0, 0, &prepared, detail,
                                 (int)sizeof(detail)) ==
               MP_WORLD_BACKING_NOT_SOLID,
           "fluid backing is rejected");
    EXPECT(strncmp(detail, "backing type=minecraft:water", 28) == 0,
           "backing rejection reports the offending type");

    // Wrong dimension fails closed before touching the world.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    detail[0] = '\0';
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:the_nether",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 7, "demo", 0, 1, 0, 0, &prepared, detail,
                                 (int)sizeof(detail)) == MP_WORLD_BAD_ARGUMENT,
           "dimension mismatch is a bad argument");
    EXPECT(strcmp(detail, "Player is in a different dimension") == 0,
           "dimension mismatch reports the read-path detail");
    EXPECT(!prepared && g_ww_block_live == 0 && g_ww_impl_live == 0,
           "every failure path leaves zero live fake objects");
    return 1;
}

static int test_world_write_inventory_capacity(void)
{
    struct fake_ww_server server;
    struct fake_ww_player player;
    struct fake_ww_world world;
    struct fake_ww_inventory inventory;
    struct fake_ww_map_view map_view;
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    inventory.occupied[0] = 1;
    inventory.map_id[0] = 5;
    inventory.occupied[2] = 1;
    inventory.map_id[2] = -1;

    int available = -1;
    char detail[192] = {0};
    EXPECT(mp_world_check_inventory_capacity(&player, 4, &available, detail,
                                             (int)sizeof(detail)) ==
                   MP_WORLD_OK &&
               available == 4,
           "capacity counts empty slots through getSize+getItem");
    EXPECT(g_ww_impl_live == 0,
           "each occupied-slot optional copy is scalar-deleted once");

    detail[0] = '\0';
    EXPECT(mp_world_check_inventory_capacity(&player, 5, &available, detail,
                                             (int)sizeof(detail)) ==
                   MP_WORLD_INVENTORY_FULL &&
               available == 4,
           "insufficient empty slots report inventory full");
    EXPECT(strcmp(detail, "inventory has 4 empty slots; 5 required") == 0,
           "capacity detail matches the C++ reference text");

    EXPECT(mp_world_check_inventory_capacity(nullptr, 1, &available, nullptr, 0) ==
                   MP_WORLD_BAD_ARGUMENT &&
               available == 0,
           "null player is a bad argument");
    EXPECT(mp_world_check_inventory_capacity(&player, -1, &available, nullptr,
                                             0) == MP_WORLD_BAD_ARGUMENT,
           "negative requirement is a bad argument");
    EXPECT(g_ww_clear_all_calls == 0 && g_ww_bad_delete_flags == 0,
           "capacity checking never clears anything");
    return 1;
}

static int test_world_write_place_and_remove(void)
{
    struct fake_ww_server server;
    struct fake_ww_player player;
    struct fake_ww_world world;
    struct fake_ww_inventory inventory;
    struct fake_ww_map_view map_view;
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_map_view_id = 9;
    struct screen_pos cell = {0, 64, 0};
    struct screen_pos backing = {0, 64, 1};

    struct mp_world_prepared_tile *prepared = nullptr;
    char detail[192] = {0};
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 cell, backing, SCREEN_FACE_NORTH, &map_view,
                                 9, "demo", 0, 1, 0, 0, &prepared,
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare a placeable tile");
    EXPECT(mp_world_place_prepared(prepared, detail, (int)sizeof(detail)) ==
               MP_WORLD_OK,
           "placement verifies air, applies data and re-verifies the frame");
    EXPECT(world.set_data_calls == 1 && world.last_physics == 1,
           "setData is invoked once with apply_physics=true");
    EXPECT(((struct fake_ww_block_data *)world.last_set_data)->facing == 2,
           "the placed BlockData is the prepared facing_direction frame");
    EXPECT(strcmp(world.cells[0].type, "minecraft:frame") == 0,
           "the cell holds the frame after placement");

    detail[0] = '\0';
    EXPECT(mp_world_place_prepared(prepared, detail, (int)sizeof(detail)) ==
               MP_WORLD_CELL_NOT_AIR,
           "a second placement into the same cell is rejected");
    EXPECT(strcmp(detail, "screen cell changed before placement") == 0,
           "occupied-cell detail matches the C++ reference text");

    detail[0] = '\0';
    EXPECT(mp_world_remove_managed(&server, &player, "minecraft:overworld",
                                   cell, 9, detail, (int)sizeof(detail)) ==
               MP_WORLD_OK,
           "managed frame removal restores air");
    EXPECT(g_ww_create_air_calls == 1 &&
               strcmp(world.cells[0].type, "minecraft:air") == 0,
           "removal builds air block data through the states-free overload");

    // Removal of an already-air cell is a no-op success.
    EXPECT(mp_world_remove_managed(&server, &player, "minecraft:overworld",
                                   cell, 9, nullptr, 0) == MP_WORLD_OK &&
               g_ww_create_air_calls == 1,
           "air cell removal succeeds without creating block data");

    // Refuse to remove anything that is not a frame.
    detail[0] = '\0';
    EXPECT(mp_world_remove_managed(&server, &player, "minecraft:overworld",
                                   backing, 9, detail,
                                   (int)sizeof(detail)) ==
               MP_WORLD_NOT_MANAGED_FRAME,
           "non-frame block is never removed");
    EXPECT(strcmp(detail, "refusing to remove a non-frame block") == 0,
           "non-frame detail matches the C++ reference text");
    EXPECT(mp_world_remove_managed(&server, &player, "minecraft:overworld",
                                   (struct screen_pos){9, 9, 9}, 9,
                                   nullptr, 0) == MP_WORLD_BAD_ARGUMENT,
           "missing block is a bad argument");

    // setData that does not take effect must be detected.
    world.set_data_applies = 0;
    detail[0] = '\0';
    EXPECT(mp_world_place_prepared(prepared, detail, (int)sizeof(detail)) ==
               MP_WORLD_FRAME_PLACE_FAILED,
           "silent setData failure is caught by re-verification");
    EXPECT(strcmp(detail, "frame block was not present after setData") == 0,
           "silent failure detail matches the C++ reference text");

    // rollback_placed is the fire-and-forget removal wrapper.
    world.set_data_applies = 1;
    snprintf(world.cells[0].type, sizeof(world.cells[0].type),
             "minecraft:frame");
    mp_world_rollback_placed(&server, &player, "minecraft:overworld", cell);
    EXPECT(strcmp(world.cells[0].type, "minecraft:air") == 0,
           "rollback restores air through remove_managed");

    mp_world_prepared_destroy(prepared);
    EXPECT(g_ww_block_live == 0 && g_ww_block_data_live == 0 &&
               g_ww_impl_live == 0 && g_ww_bad_delete_flags == 0,
           "placement and removal leak no fake runtime objects");
    return 1;
}

static int test_world_write_deliver_retract_and_rollback(void)
{
    struct fake_ww_server server;
    struct fake_ww_player player;
    struct fake_ww_world world;
    struct fake_ww_inventory inventory;
    struct fake_ww_map_view map_view;
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);

    // Full prepare -> place -> deliver -> retract ordering.
    struct mp_world_prepared_tile *prepared[2] = {0};
    g_ww_map_view_id = 100;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 (struct screen_pos){0, 64, 0},
                                 (struct screen_pos){0, 64, 1},
                                 SCREEN_FACE_NORTH, &map_view, 100, "wall",
                                 0, 2, 0, 0, &prepared[0],
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare tile 0");
    g_ww_map_view_id = 101;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 (struct screen_pos){1, 64, 0},
                                 (struct screen_pos){1, 64, 1},
                                 SCREEN_FACE_NORTH, &map_view, 101, "wall",
                                 1, 2, 0, 1, &prepared[1],
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare tile 1");
    EXPECT(mp_world_place_prepared(prepared[0], nullptr, 0) == MP_WORLD_OK &&
               mp_world_place_prepared(prepared[1], nullptr, 0) == MP_WORLD_OK,
           "place both tiles");

    inventory.occupied[0] = 1; // unrelated junk item in the first slot
    inventory.map_id[0] = -1;
    char detail[192] = {0};
    EXPECT(mp_world_deliver_prepared_maps(&player, prepared, 2, detail,
                                          (int)sizeof(detail)) ==
               MP_WORLD_OK,
           "deliver both map items");
    EXPECT(inventory.occupied[1] && inventory.map_id[1] == 100 &&
               inventory.occupied[2] && inventory.map_id[2] == 101,
           "delivery fills the first empty slots in order and is verified");
    EXPECT(g_ww_impl_live == 0,
           "setItem consumed both impls; verification copies are destroyed");
    EXPECT(!g_ww_set_item_bad_param,
           "every setItem optional was engaged with a valid impl");

    mp_world_retract_prepared_maps(&player, prepared, 2);
    EXPECT(inventory.clear_calls == 2 && !inventory.occupied[1] &&
               !inventory.occupied[2],
           "retraction clears exactly the delivered, verified slots");
    EXPECT(g_ww_clear_all_calls == 0,
           "retraction uses clear(int), never the whole-inventory clear");
    mp_world_retract_prepared_maps(&player, prepared, 2);
    EXPECT(inventory.clear_calls == 2,
           "a second retraction is a no-op after delivered_slot reset");

    // Re-delivering moved-out tiles must fail without touching slots.
    EXPECT(mp_world_deliver_prepared_maps(&player, prepared, 2, nullptr, 0) ==
               MP_WORLD_MAP_DELIVERY_FAILED,
           "tiles whose items were already delivered cannot deliver again");
    mp_world_prepared_destroy(prepared[0]);
    mp_world_prepared_destroy(prepared[1]);

    // Verification failure at the second tile rolls back the first.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_map_view_id = 200;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 (struct screen_pos){0, 64, 0},
                                 (struct screen_pos){0, 64, 1},
                                 SCREEN_FACE_NORTH, &map_view, 200, "wall",
                                 0, 2, 0, 0, &prepared[0],
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare rollback tile 0");
    g_ww_map_view_id = 201;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 (struct screen_pos){1, 64, 0},
                                 (struct screen_pos){1, 64, 1},
                                 SCREEN_FACE_NORTH, &map_view, 201, "wall",
                                 1, 2, 0, 1, &prepared[1],
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare rollback tile 1");
    inventory.drop_set_item_at = 1; // tile 1's setItem silently fails
    detail[0] = '\0';
    EXPECT(mp_world_deliver_prepared_maps(&player, prepared, 2, detail,
                                          (int)sizeof(detail)) ==
               MP_WORLD_MAP_DELIVERY_FAILED,
           "failed post-delivery verification aborts the transaction");
    EXPECT(strcmp(detail,
                  "map item verification failed after inventory delivery") == 0,
           "delivery failure detail matches the C++ reference text");
    EXPECT(inventory.clear_calls == 1 && inventory.last_cleared == 0 &&
               !inventory.occupied[0],
           "automatic retraction clears only the verified delivered slot");
    mp_world_prepared_destroy(prepared[0]);
    mp_world_prepared_destroy(prepared[1]);

    // Capacity shrinking between check and delivery is detected.
    setup_fake_write_world(&server, &player, &world, &inventory, &map_view);
    g_ww_map_view_id = 300;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 (struct screen_pos){0, 64, 0},
                                 (struct screen_pos){0, 64, 1},
                                 SCREEN_FACE_NORTH, &map_view, 300, "wall",
                                 0, 2, 0, 0, &prepared[0],
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare capacity tile 0");
    g_ww_map_view_id = 301;
    EXPECT(mp_world_prepare_tile(&server, &player, "minecraft:overworld",
                                 (struct screen_pos){1, 64, 0},
                                 (struct screen_pos){1, 64, 1},
                                 SCREEN_FACE_NORTH, &map_view, 301, "wall",
                                 1, 2, 0, 1, &prepared[1],
                                 nullptr, 0) == MP_WORLD_OK,
           "prepare capacity tile 1");
    for (int slot = 0; slot < FAKE_WW_INV_SIZE - 1; slot++) {
        inventory.occupied[slot] = 1;
        inventory.map_id[slot] = -1;
    }
    detail[0] = '\0';
    EXPECT(mp_world_deliver_prepared_maps(&player, prepared, 2, detail,
                                          (int)sizeof(detail)) ==
               MP_WORLD_INVENTORY_FULL,
           "one remaining slot cannot take two deliveries");
    EXPECT(strcmp(detail,
                  "inventory capacity changed during screen creation") == 0,
           "capacity change detail matches the C++ reference text");
    EXPECT(g_ww_impl_live == 2,
           "aborted delivery leaves both prepared tiles owning their items");
    mp_world_prepared_destroy(prepared[0]);
    mp_world_prepared_destroy(prepared[1]);
    EXPECT(g_ww_impl_live == 0 && g_ww_meta_live == 0 &&
               g_ww_block_data_live == 0 && g_ww_block_live == 0 &&
               g_ww_bad_delete_flags == 0,
           "the whole transaction suite leaks no fake runtime objects");
    return 1;
}
#endif

// ================================================================
// MAIN
// ================================================================

static int check_external_mcv(const char *path)
{
    struct mcv_file file;
    enum mcv_error result = mcv_open(path, &file);
    if (result != MCV_OK) {
        fprintf(stderr, "mcv_open failed: %s\n", mcv_error_name(result));
        return 1;
    }
    if (file.raw_frame_size > SIZE_MAX) {
        mcv_close(&file);
        return 1;
    }
    uint8_t *frame = malloc((size_t)file.raw_frame_size);
    if (!frame) {
        mcv_close(&file);
        return 1;
    }
    result = mcv_read_frame(&file, 0, frame, (size_t)file.raw_frame_size);
    if (result == MCV_OK) {
        result = mcv_read_frame(&file,
                                (uint32_t)(file.header.frame_count - 1),
                                frame, (size_t)file.raw_frame_size);
    }
    if (result == MCV_OK) {
        printf("MCV reader check passed: frames=%llu codec=%u data=%llu "
               "index=%llu\n",
               (unsigned long long)file.header.frame_count,
               file.header.codec,
               (unsigned long long)file.header.frame_data_size,
               (unsigned long long)file.header.frame_index_offset);
    } else {
        fprintf(stderr, "mcv_read_frame failed: %s\n",
                mcv_error_name(result));
    }
    free(frame);
    mcv_close(&file);
    return result == MCV_OK ? 0 : 1;
}

// Sequential decode benchmark: mirrors the playback access pattern
// (every frame in order) so real .mcv files can be validated for
// throughput without a running server.
static int bench_external_mcv(const char *path)
{
    struct mcv_file file;
    enum mcv_error result = mcv_open(path, &file);
    if (result != MCV_OK) {
        fprintf(stderr, "mcv_open failed: %s\n", mcv_error_name(result));
        return 1;
    }
    uint8_t *frame = malloc((size_t)file.raw_frame_size);
    // Previous frame, used to report how many tiles a real video actually
    // changes per frame: that ratio is what the send path can skip.
    uint8_t *previous = malloc((size_t)file.raw_frame_size);
    if (!frame || !previous) {
        free(frame);
        free(previous);
        mcv_close(&file);
        return 1;
    }
    int tiles_wide = file.header.tile_width;
    int tiles_high = file.header.tile_height;
    int pixel_width = file.header.pixel_width;
    uint64_t tile_sends = 0;
    uint64_t tile_total = 0;
    bool have_previous = false;

    double budget_ms = mcv_frame_duration_ms(&file);
    clock_t started = clock();
    double worst_ms = 0.0;
    uint64_t over_budget = 0;
    uint64_t count = file.header.frame_count;
    for (uint64_t i = 0; i < count; i++) {
        clock_t frame_started = clock();
        result = mcv_read_frame(&file, (uint32_t)i, frame,
                                (size_t)file.raw_frame_size);
        if (result != MCV_OK) {
            fprintf(stderr, "frame %llu failed: %s\n",
                    (unsigned long long)i, mcv_error_name(result));
            free(frame);
            mcv_close(&file);
            return 1;
        }
        double frame_ms = (double)(clock() - frame_started) * 1000.0 /
                          CLOCKS_PER_SEC;
        if (frame_ms > worst_ms) worst_ms = frame_ms;
        if (frame_ms > budget_ms) over_budget++;

        for (int ty = 0; ty < tiles_high; ty++) {
            for (int tx = 0; tx < tiles_wide; tx++) {
                tile_total++;
                bool changed = !have_previous;
                for (int row = 0; row < SCREEN_TILE_SIZE && !changed; row++) {
                    size_t offset =
                        (((size_t)(ty * SCREEN_TILE_SIZE + row) *
                          (size_t)pixel_width) +
                         (size_t)tx * SCREEN_TILE_SIZE) * 4;
                    if (memcmp(frame + offset, previous + offset,
                               (size_t)SCREEN_TILE_SIZE * 4) != 0) {
                        changed = true;
                    }
                }
                if (changed) tile_sends++;
            }
        }
        memcpy(previous, frame, (size_t)file.raw_frame_size);
        have_previous = true;
    }
    double total_ms = (double)(clock() - started) * 1000.0 / CLOCKS_PER_SEC;
    double raw_mb = (double)count * (double)file.raw_frame_size /
                    (1024.0 * 1024.0);

    printf("MCV bench: %llu frames, codec=%u, %.0f ms total, "
           "%.3f ms/frame avg, %.3f ms worst, %.0f MB/s decoded\n",
           (unsigned long long)count, file.header.codec, total_ms,
           total_ms / (double)count, worst_ms,
           total_ms > 0.0 ? raw_mb / (total_ms / 1000.0) : 0.0);
    printf("  frame budget %.1f ms; %llu frame(s) over budget; "
           "index window %u entries\n",
           budget_ms, (unsigned long long)over_budget, file.cache_count);
    printf("  tiles changed %llu/%llu (%.1f%%): the send path skips the "
           "rest\n",
           (unsigned long long)tile_sends, (unsigned long long)tile_total,
           tile_total ? 100.0 * (double)tile_sends / (double)tile_total : 0.0);

    free(frame);
    free(previous);
    mcv_close(&file);
    return 0;
}

static int test_public_provider_lifecycle(void)
{
    const mp_api_v1 *inactive = endstone_mediaplayer_get_api(MP_API_V1);
    EXPECT(inactive != nullptr, "inactive provider table is exported");
    EXPECT(endstone_mediaplayer_get_api(MP_API_V1 + 1) == nullptr,
           "unsupported provider version is absent");
    EXPECT(strcmp(inactive->result_string(MP_ERR_BUSY), "busy") == 0,
           "result strings are stable while inactive");
    mp_screen_handle invalid = 99;
    EXPECT(inactive->screen_find("missing", &invalid) == MP_ERR_UNAVAILABLE,
           "stateful operations are unavailable while inactive");
    EXPECT(invalid == MP_INVALID_SCREEN_HANDLE, "failed find clears handle");
    mp_frame_handle inactive_frame = UINT64_C(99);
    EXPECT(inactive->frame_begin(MP_INVALID_SCREEN_HANDLE, &inactive_frame) ==
               MP_ERR_UNAVAILABLE && inactive_frame == MP_INVALID_FRAME_HANDLE,
           "inactive frame producer clears output handle");

    struct video_ctx *ctx = calloc(1, sizeof(*ctx));
    EXPECT(ctx != nullptr, "provider test context allocation");
    screen_registry_init(&ctx->registry);
    video_engine_init(&ctx->engine);
    mps_source_engine_init(&ctx->image_engine);
    screen_audio_engine_init(&ctx->audio);
    map_render_init(&ctx->render, nullptr, nullptr);
    ctx->active = 1;
    mp_api_provider_activate(ctx);
    const mp_api_v1 *api = endstone_mediaplayer_get_api(MP_API_V1);
    EXPECT(api->instance_id != 0, "activation publishes an instance id");

    mp_capabilities caps = {.struct_size = sizeof(caps), .flags = 0};
    EXPECT(api->capabilities_get(&caps) == MP_OK &&
           caps.max_width_tiles == 1024 && caps.max_pending_tiles == 256,
           "capabilities report fixed V1 limits");
    mp_screen_create_info create = {
        .struct_size = sizeof(create), .flags = 0, .name = "api-test",
        .width_tiles = 1, .height_tiles = 1, .backend = MP_BACKEND_LOGICAL,
    };
    mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
    EXPECT(api->screen_create(&create, &screen) == MP_OK && screen != 0,
           "logical screen creation returns a handle");
    mp_screen_handle found = 0;
    EXPECT(api->screen_find("api-test", &found) == MP_OK && found == screen,
           "screen find reuses the stable handle");
    create.name = "api-second";
    mp_screen_handle second = MP_INVALID_SCREEN_HANDLE;
    EXPECT(api->screen_create(&create, &second) == MP_OK && second != 0,
           "second logical screen for list capacity checks");
    uint32_t count = 0;
    EXPECT(api->screen_list(nullptr, 0, &count) == MP_OK && count == 2,
           "screen list supports count query");
    mp_screen_handle listed[2] = {0, 0};
    uint32_t short_count = 0;
    EXPECT(api->screen_list(listed, 1, &short_count) == MP_ERR_BUFFER_TOO_SMALL &&
           short_count == 2 && listed[0] == screen,
           "screen list short buffer returns required count and prefix");
    short_count = 0;
    EXPECT(api->screen_list(listed, 2, &short_count) == MP_OK &&
           short_count == 2 && listed[0] == screen && listed[1] == second,
           "screen list full buffer returns handles");
    EXPECT(api->screen_delete(second) == MP_OK, "list fixture cleanup");

    snprintf(ctx->save_path, sizeof(ctx->save_path),
             "provider_persistence_missing_parent/screens.json");
    create.name = "api-create-fail";
    mp_screen_handle failed_create = UINT64_C(99);
    EXPECT(api->screen_create(&create, &failed_create) == MP_ERR_IO &&
           failed_create == MP_INVALID_SCREEN_HANDLE &&
           api->screen_find("api-create-fail", &found) == MP_ERR_NOT_FOUND,
           "failed create rolls back the registry entry and handle");
    mp_screen_info info = {.struct_size = sizeof(info), .flags = 0};
    EXPECT(api->screen_rename(screen, "api-rename-fail") == MP_ERR_IO &&
           api->screen_get_info(screen, &info) == MP_OK &&
           strcmp(info.name, "api-test") == 0 &&
           api->screen_find("api-test", &found) == MP_OK && found == screen &&
           api->screen_find("api-rename-fail", &found) == MP_ERR_NOT_FOUND,
           "failed rename restores the old name and handle");
    ctx->save_path[0] = '\0';
    create.name = "api-delete-fail";
    mp_screen_handle failed_delete = MP_INVALID_SCREEN_HANDLE;
    EXPECT(api->screen_create(&create, &failed_delete) == MP_OK,
           "delete rollback fixture creation");
    snprintf(ctx->save_path, sizeof(ctx->save_path),
             "provider_persistence_missing_parent/screens.json");
    EXPECT(api->screen_delete(failed_delete) == MP_ERR_IO &&
           api->screen_get_info(failed_delete, &info) == MP_OK &&
           strcmp(info.name, "api-delete-fail") == 0 &&
           api->screen_find("api-delete-fail", &found) == MP_OK &&
           found == failed_delete,
           "failed delete preserves the live entry and handle");
    ctx->save_path[0] = '\0';
    EXPECT(api->screen_delete(failed_delete) == MP_OK,
           "delete rollback fixture cleanup");
    EXPECT(api->screen_get_info(screen, &info) == MP_OK &&
           info.width_tiles == 1 && info.backend == MP_BACKEND_LOGICAL,
           "screen info reports logical geometry");
    struct {
        mp_screen_info info;
        unsigned char tail[16];
    } oversized = {.info = {.struct_size = sizeof(oversized), .flags = 0}};
    memset(oversized.tail, 0xA5, sizeof(oversized.tail));
    EXPECT(api->screen_get_info(screen, &oversized.info) == MP_OK &&
           oversized.tail[0] == 0xA5 && oversized.tail[15] == 0xA5,
           "oversized output accepts V1 prefix and preserves trailing bytes");
    unsigned char undersized_bytes[sizeof(info)];
    memset(undersized_bytes, 0xA5, sizeof(undersized_bytes));
    ((mp_screen_info *)undersized_bytes)->struct_size = sizeof(uint32_t);
    EXPECT(api->screen_get_info(screen, (mp_screen_info *)undersized_bytes) ==
               MP_ERR_INVALID_ARGUMENT &&
           ((mp_screen_info *)undersized_bytes)->struct_size == sizeof(uint32_t) &&
           undersized_bytes[8] == 0xA5,
           "undersized output is rejected without overwrite");
    info.flags = 1;
    EXPECT(api->screen_get_info(screen, &info) == MP_ERR_INVALID_ARGUMENT,
           "nonzero output flags are rejected");
    info.flags = 0;
    uint8_t pixel[4] = {1, 2, 3, 4};
    EXPECT(api->screen_update_region(screen, 0, 0, 1, 1, pixel,
                                     sizeof(pixel), sizeof(pixel),
                                     MP_PIXEL_FORMAT_ABGR) == MP_OK,
           "region convenience update commits");
    struct screen_entry *entry = nullptr;
    int entry_index = screen_registry_find(&ctx->registry, "api-test");
    if (entry_index >= 0) entry = ctx->registry.screens[entry_index];
    EXPECT(entry != nullptr, "provider test resolves internal screen fixture");
    struct video_session *media = video_engine_acquire(
        &ctx->engine, entry->runtime_id);
    EXPECT(media != nullptr, "video fixture session allocation");
    media->active = 1;
    media->screen_runtime_id = entry->runtime_id;
    entry->playing = 1;
    EXPECT(api->screen_update_region(screen, 0, 0, 1, 1, pixel,
                                     sizeof(pixel), sizeof(pixel),
                                     MP_PIXEL_FORMAT_BGRA) == MP_OK &&
           video_engine_find(&ctx->engine, entry->runtime_id) == nullptr,
           "external update releases active video producer");
    mp_stats stats = {.struct_size = sizeof(stats), .flags = 0};
    EXPECT(api->screen_get_stats(screen, &stats) == MP_OK &&
           stats.generation > 0 && stats.resident_tiles == 1,
           "surface stats expose committed pixels");
    media = video_engine_acquire(&ctx->engine, entry->runtime_id);
    EXPECT(media != nullptr, "second video fixture session allocation");
    media->active = 1;
    media->screen_runtime_id = entry->runtime_id;
    entry->playing = 1;
    mp_frame_handle media_frame = MP_INVALID_FRAME_HANDLE;
    EXPECT(api->frame_begin(screen, &media_frame) == MP_OK &&
           video_engine_find(&ctx->engine, entry->runtime_id) == nullptr &&
           api->frame_abort(media_frame) == MP_OK,
           "frame begin replaces active video producer");
    mp_frame_handle frame = MP_INVALID_FRAME_HANDLE;
    EXPECT(api->frame_begin(screen, &frame) == MP_OK && frame != 0,
           "frame begin allocates a distinct handle");
    EXPECT(mp_api_provider_screen_has_active_frame(entry->runtime_id),
           "provider reports an active frame without aborting it");
    EXPECT(api->frame_update_region(frame, 0, 0, 1, 1, pixel,
                                     sizeof(pixel), sizeof(pixel),
                                     MP_PIXEL_FORMAT_RGBA) == MP_OK &&
           api->frame_commit(frame) == MP_OK,
           "frame update and commit work");
    EXPECT(!mp_api_provider_screen_has_active_frame(entry->runtime_id),
           "provider active-frame query clears after commit");
    EXPECT(api->frame_abort(frame) == MP_ERR_INVALID_HANDLE,
           "committed frame handle is stale");
    mp_frame_handle clear_frame = MP_INVALID_FRAME_HANDLE;
    EXPECT(api->frame_begin(screen, &clear_frame) == MP_OK &&
           api->screen_clear(screen) == MP_OK &&
           api->frame_abort(clear_frame) == MP_ERR_INVALID_HANDLE,
           "clear retires active frame handles safely");
    EXPECT(api->screen_delete(screen) == MP_OK &&
           api->screen_delete(screen) == MP_ERR_INVALID_HANDLE,
           "delete rejects stale handles");
    create.name = "command-stale";
    mp_screen_handle command_screen = MP_INVALID_SCREEN_HANDLE;
    EXPECT(api->screen_create(&create, &command_screen) == MP_OK,
           "command-side stale-handle fixture creation");
    mp_frame_handle stale_frame = MP_INVALID_FRAME_HANDLE;
    EXPECT(api->frame_begin(command_screen, &stale_frame) == MP_OK,
           "stale-handle fixture frame begin");
    EXPECT(screen_registry_delete(&ctx->registry, "command-stale") == SCREEN_OK,
           "simulate command-side deletion");
    EXPECT(api->frame_abort(stale_frame) == MP_ERR_INVALID_HANDLE,
           "command deletion rejects stale frame safely");
    create.name = "retained";
    mp_screen_handle retained = MP_INVALID_SCREEN_HANDLE;
    EXPECT(api->screen_create(&create, &retained) == MP_OK,
           "retained stale-handle fixture creation");
    uint64_t old_instance = api->instance_id;
    mp_api_provider_deactivate(ctx);
    EXPECT(api->screen_find("api-renamed", &found) == MP_ERR_UNAVAILABLE,
           "deactivation makes the provider unavailable");
    ctx->active = 1;
    mp_api_provider_activate(ctx);
    api = endstone_mediaplayer_get_api(MP_API_V1);
    EXPECT(api->instance_id != old_instance,
           "reactivation allocates a new instance id");
    EXPECT(api->screen_get_info(retained, &info) == MP_ERR_INVALID_HANDLE,
           "old screen handle is rejected after reactivation");
    EXPECT(api->screen_find("api-renamed", &found) == MP_ERR_NOT_FOUND,
           "reactivation rejects stale object handles");
    mp_api_provider_deactivate(ctx);
    ctx->active = 0;
    screen_registry_cleanup(&ctx->registry);
    free(ctx);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--provider") == 0)
        return test_public_provider_lifecycle() ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--check-mcv") == 0)
        return check_external_mcv(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--bench-mcv") == 0)
        return bench_external_mcv(argv[2]);

    printf("=== Video Screen System Tests ===\n\n");

    printf("[Screen Geometry]\n");
    RUN_TEST(test_geom_xy_plane_south);
    RUN_TEST(test_geom_xy_plane_north);
    RUN_TEST(test_geom_zy_plane_east);
    RUN_TEST(test_geom_zy_plane_west);
    RUN_TEST(test_geom_reversed_corners);
    RUN_TEST(test_geom_1x1);
    RUN_TEST(test_geom_7x4);
    RUN_TEST(test_geom_checked_logical_limits);
    RUN_TEST(test_geom_width_exceed);
    RUN_TEST(test_geom_height_exceed);
    RUN_TEST(test_geom_not_vertical);
    RUN_TEST(test_geom_dimension_mismatch);
    RUN_TEST(test_geom_facing_mismatch);
    RUN_TEST(test_geom_tile_index);
    RUN_TEST(test_geom_facing_from_player);
    RUN_TEST(test_geom_backing_opposes_facing);
    RUN_TEST(test_geom_facing_candidates_from_plane);
    RUN_TEST(test_status_error_names);

    printf("\n[Video Format]\n");
    RUN_TEST(test_mcv_valid_1x1);
    RUN_TEST(test_mcv_valid_7x4);
    RUN_TEST(test_mcv_bad_magic);
    RUN_TEST(test_mcv_bad_version);
    RUN_TEST(test_mcv_truncated_header);
    RUN_TEST(test_mcv_header_crc);
    RUN_TEST(test_mcv_flags_policy);
    RUN_TEST(test_mcv_frame_duration);
    RUN_TEST(test_mcv_multiple_uncompressed_frames);
    RUN_TEST(test_mcv_frame_reference_decode);
    RUN_TEST(test_mcv_invalid_data_offset);
    RUN_TEST(test_mcv_invalid_index_offset);
    RUN_TEST(test_mcv_invalid_index_entry_size);
    RUN_TEST(test_mcv_data_index_overlap);
    RUN_TEST(test_mcv_truncated_data_and_index);
    RUN_TEST(test_mcv_checked_arithmetic_overflow);
    RUN_TEST(test_mcv_read_range_failures);
    RUN_TEST(test_mcv_unknown_codec);
    RUN_TEST(test_mcv_wrong_uncompressed_stored_size);
    RUN_TEST(test_mcv_uncompressed_layout_enforced);
    RUN_TEST(test_mcv_tile_pixel_and_fps_validation);
    RUN_TEST(test_mcv_close_clears_state);
    RUN_TEST(test_mcv_frame_crc_mismatch);
    RUN_TEST(test_mcv_zlib_frame_crc_mismatch);
    RUN_TEST(test_mcv_utf8_filename);
    RUN_TEST(test_mcv_frame_flags_validation);
    RUN_TEST(test_mcv_zlib_roundtrip);
    RUN_TEST(test_mcv_index_window);

    printf("\n[Playback Clock]\n");
    RUN_TEST(test_clock_normal_20fps);
    RUN_TEST(test_clock_skip_frames);
    RUN_TEST(test_clock_pause_resume);
    RUN_TEST(test_clock_single_play);
    RUN_TEST(test_clock_infinite_loop);
    RUN_TEST(test_clock_multi_loop);
    RUN_TEST(test_clock_two_screens_independent);
    RUN_TEST(test_detailed_tick_reports_loop_boundary);

    printf("\n[Command Arguments]\n");
    RUN_TEST(test_args_parse_int);
    RUN_TEST(test_args_parse_loop);
    RUN_TEST(test_args_parse_index);
    RUN_TEST(test_args_video_fits_screen);

    printf("\n[Session Lifecycle]\n");
    RUN_TEST(test_session_multi_loop_counter);
    RUN_TEST(test_session_loop_bounds);
    RUN_TEST(test_engine_slot_lookup_by_runtime_id);
    RUN_TEST(test_engine_survives_other_screen_delete);
    RUN_TEST(test_session_finish_releases_resources);
    RUN_TEST(test_session_start_failure_releases_resources);

    printf("\n[Screen Registry]\n");
    RUN_TEST(test_registry_create_find);
    RUN_TEST(test_registry_duplicate_name);
    RUN_TEST(test_registry_invalid_name);
    RUN_TEST(test_registry_runtime_identity_survives_shifts);
    RUN_TEST(test_registry_lazy_tiles_and_cleanup);
    RUN_TEST(test_registry_delete);

    printf("\n[Persistence]\n");
    RUN_TEST(test_persistence_roundtrip);
    RUN_TEST(test_persistence_omits_and_ignores_viewers);
    RUN_TEST(test_persistence_facing_range);
    RUN_TEST(test_managed_map_identity_roundtrip);
    RUN_TEST(test_persistence_v1_migration_and_extremes);
    RUN_TEST(test_persistence_v2_roundtrip_and_logical_size);
    RUN_TEST(test_persistence_sidecar_corruption_matrix);
    RUN_TEST(test_persistence_corrupted);
    RUN_TEST(test_persistence_truncated);
    RUN_TEST(test_persistence_missing_file);
    RUN_TEST(test_persistence_invalid_dimensions);
    RUN_TEST(test_preferences_roundtrip);

    printf("\n[Screen Audio]\n");
    RUN_TEST(test_screen_audio_video_clock);

    printf("\n[Command and Public Viewer Policy]\n");
    RUN_TEST(test_command_permission_and_surface);
    RUN_TEST(test_mps_catalog_and_source_lifecycle);
    RUN_TEST(test_public_viewer_eligibility);
    RUN_TEST(test_public_viewer_collection_and_membership);

    printf("\n[Map Color]\n");
    RUN_TEST(test_map_test_patterns);

    printf("\n[Public Provider ABI]\n");
    RUN_TEST(test_public_provider_lifecycle);

#if defined(ES_PLATFORM_WINDOWS)
    printf("\n[Map ABI]\n");
    RUN_TEST(test_map_abi_dispatch);
    RUN_TEST(test_msvc_shared_ptr_release_contract);
    RUN_TEST(test_renderer_registration_callback_and_lifetime);
    RUN_TEST(test_render_multi_tile_dirty_tracking);
    printf("\n[Pure-C World Read ABI]\n");
    RUN_TEST(test_world_read_abi_snapshot_and_failure_safety);
    RUN_TEST(test_world_read_abi_block_lifetime_and_policy);
    RUN_TEST(test_world_read_abi_validation_inspection_and_map_lookup);
    printf("\n[Pure-C World Write ABI]\n");
    RUN_TEST(test_world_write_prepare_builds_frame_and_map_item);
    RUN_TEST(test_world_write_prepare_failure_paths);
    RUN_TEST(test_world_write_inventory_capacity);
    RUN_TEST(test_world_write_place_and_remove);
    RUN_TEST(test_world_write_deliver_retract_and_rollback);
#endif

    remove_fixtures();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

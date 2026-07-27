#include "mediaplayer/bedrock/world_read_abi.h"

#include "abi_helpers.h"
#include "mediaplayer/bedrock/map_abi.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void set_detail(char *detail, int size, const char *message)
{
    if (detail && size > 0) {
        snprintf(detail, (size_t)size, "%s", message ? message : "");
    }
}

bool mp_world_c_type_is_air(const char *type)
{
    return type &&
           (strcmp(type, "minecraft:air") == 0 ||
            strcmp(type, "minecraft:cave_air") == 0 ||
            strcmp(type, "minecraft:void_air") == 0);
}

bool mp_world_c_type_is_support_candidate(const char *type)
{
    return type && !mp_world_c_type_is_air(type) &&
           strcmp(type, "minecraft:water") != 0 &&
           strcmp(type, "minecraft:flowing_water") != 0 &&
           strcmp(type, "minecraft:lava") != 0 &&
           strcmp(type, "minecraft:flowing_lava") != 0 &&
           strcmp(type, "minecraft:fire") != 0 &&
           strcmp(type, "minecraft:soul_fire") != 0;
}

#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)

_Static_assert(sizeof(struct es_location) == ES_LOCATION_SIZE,
               "measured Location size mismatch");
_Static_assert(_Alignof(struct es_location) == ES_LOCATION_ALIGN,
               "measured Location alignment mismatch");
_Static_assert(offsetof(struct es_location, dimension) ==
                   ES_LOCATION_OFF_DIMENSION,
               "measured Location dimension offset mismatch");
_Static_assert(offsetof(struct es_location, x) == ES_LOCATION_OFF_X,
               "measured Location x offset mismatch");
_Static_assert(offsetof(struct es_location, y) == ES_LOCATION_OFF_Y,
               "measured Location y offset mismatch");
_Static_assert(offsetof(struct es_location, z) == ES_LOCATION_OFF_Z,
               "measured Location z offset mismatch");
_Static_assert(offsetof(struct es_location, pitch) == ES_LOCATION_OFF_PITCH,
               "measured Location pitch offset mismatch");
_Static_assert(offsetof(struct es_location, yaw) == ES_LOCATION_OFF_YAW,
               "measured Location yaw offset mismatch");

struct world_read_context {
    struct es_location location;
    void *dimension;
    char dimension_id[64];
};

static bool object_vtable(void *object, void ****out)
{
    if (!object || !out) return false;
    void ***vtable_address = (void ***)object;
    if (!*vtable_address) return false;
    *out = vtable_address;
    return true;
}

static bool read_string_return(void *object, size_t slot,
                               char *output, size_t output_size,
                               void **target)
{
    if (!object || !output || output_size == 0) return false;
    output[0] = '\0';
    void ***vtable_address = nullptr;
    if (!object_vtable(object, &vtable_address) ||
        !(*vtable_address)[slot]) {
        return false;
    }
    if (target) *target = (*vtable_address)[slot];

    _Alignas(void *) unsigned char result[ES_STRING_SIZE] = {0};
#if defined(ES_PLATFORM_LINUX)
    typedef void (*string_fn)(void *, void *);
    ((string_fn)(*vtable_address)[slot])(result, object);
#else
    typedef void *(*string_fn)(void *, void *);
    ((string_fn)(*vtable_address)[slot])(object, result);
#endif

    const char *data = cpp_string_str(result);
    size_t length = cpp_string_size(result);
    bool valid = data && length < output_size;
    if (valid) {
        memcpy(output, data, length);
        output[length] = '\0';
    }
    cpp_string_destroy(result);
    return valid;
}

static bool get_player_world(void *player, const char *expected_dimension,
                             struct world_read_context *world,
                             struct mp_world_c_trace *trace,
                             char *detail, int detail_size)
{
    if (!world) return false;
    memset(world, 0, sizeof(*world));
    if (!player) {
        set_detail(detail, detail_size, "Endstone Player is null");
        return false;
    }

    void ***player_vtable = nullptr;
    if (!object_vtable(player, &player_vtable) ||
        !(*player_vtable)[ES_PLAYER_SLOT_GET_LOCATION] ||
        !(*player_vtable)[ES_PLAYER_SLOT_GET_DIMENSION]) {
        set_detail(detail, detail_size, "Endstone Player vtable is unavailable");
        return false;
    }
    if (trace) {
        trace->player = player;
        trace->player_vptr = *player_vtable;
        trace->get_location_target =
            (*player_vtable)[ES_PLAYER_SLOT_GET_LOCATION];
        trace->get_dimension_target =
            (*player_vtable)[ES_PLAYER_SLOT_GET_DIMENSION];
    }

#if defined(ES_PLATFORM_LINUX)
    typedef struct es_location (*get_location_fn)(void *);
    world->location =
        ((get_location_fn)(*player_vtable)[ES_PLAYER_SLOT_GET_LOCATION])(
            player);
#else
    typedef void *(*get_location_fn)(void *, struct es_location *);
    ((get_location_fn)(*player_vtable)[ES_PLAYER_SLOT_GET_LOCATION])(
        player, &world->location);
#endif

    typedef void *(*get_dimension_fn)(void *);
    world->dimension =
        ((get_dimension_fn)(*player_vtable)[ES_PLAYER_SLOT_GET_DIMENSION])(
            player);
    if (!world->dimension || world->location.dimension != world->dimension) {
        set_detail(detail, detail_size,
                   "Player location and dimension wrappers disagree");
        return false;
    }

    void ***dimension_vtable = nullptr;
    if (!object_vtable(world->dimension, &dimension_vtable)) {
        set_detail(detail, detail_size, "Endstone Dimension vtable is unavailable");
        return false;
    }
    if (trace) {
        trace->dimension = world->dimension;
        trace->dimension_vptr = *dimension_vtable;
    }
    void *name_target = nullptr;
    if (!read_string_return(world->dimension, ES_DIMENSION_SLOT_GET_NAME,
                            world->dimension_id,
                            sizeof(world->dimension_id), &name_target)) {
        set_detail(detail, detail_size, "Unable to read dimension name");
        return false;
    }
    if (trace) trace->get_name_target = name_target;
    if (expected_dimension && expected_dimension[0] &&
        strcmp(expected_dimension, world->dimension_id) != 0) {
        set_detail(detail, detail_size, "Player is in a different dimension");
        return false;
    }
    return true;
}

bool mp_world_c_supported(void)
{
    return true;
}

bool mp_world_c_player_world(void *player, const char *expected_dimension,
                             void **dimension_out,
                             char *detail, int detail_size)
{
    if (dimension_out) *dimension_out = nullptr;
    if (!dimension_out) return false;
    struct world_read_context world;
    if (!get_player_world(player, expected_dimension, &world, nullptr,
                          detail, detail_size)) {
        return false;
    }
    *dimension_out = world.dimension;
    return true;
}

bool mp_world_c_debug_player_get_snapshot(
    void *player, struct mp_player_snapshot *snapshot,
    struct mp_world_c_trace *trace, char *detail, int detail_size)
{
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
    if (trace) memset(trace, 0, sizeof(*trace));
    if (!snapshot) {
        set_detail(detail, detail_size, "snapshot output is null");
        return false;
    }

    struct world_read_context world;
    if (!get_player_world(player, nullptr, &world, trace,
                          detail, detail_size)) {
        return false;
    }
    snapshot->x = world.location.x;
    snapshot->y = world.location.y;
    snapshot->z = world.location.z;
    snapshot->pitch = world.location.pitch;
    snapshot->yaw = world.location.yaw;
    snapshot->block_x = (int)floorf(world.location.x);
    snapshot->block_y = (int)floorf(world.location.y);
    snapshot->block_z = (int)floorf(world.location.z);
    snprintf(snapshot->dimension_id, sizeof(snapshot->dimension_id), "%s",
             world.dimension_id);
    return true;
}

bool mp_player_get_snapshot(void *player, struct mp_player_snapshot *snapshot,
                            char *detail, int detail_size)
{
    return mp_world_c_debug_player_get_snapshot(
        player, snapshot, nullptr, detail, detail_size);
}

static enum mp_world_result probe_block_in_world(
    const struct world_read_context *world, struct screen_pos position,
    struct mp_world_block_probe *probe, struct mp_world_c_trace *trace,
    char *detail, int detail_size)
{
    void ***dimension_vtable = nullptr;
    if (!world || !world->dimension || !probe ||
        !object_vtable(world->dimension, &dimension_vtable) ||
        !(*dimension_vtable)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ]) {
        set_detail(detail, detail_size, "Dimension block lookup is unavailable");
        return MP_WORLD_BAD_ARGUMENT;
    }
    if (trace) {
        trace->get_block_target =
            (*dimension_vtable)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ];
    }

    void *block = nullptr;
#if defined(ES_PLATFORM_LINUX)
    typedef void (*get_block_fn)(void **, void *, int, int, int);
    ((get_block_fn)(*dimension_vtable)[
        ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
            &block, world->dimension, position.x, position.y, position.z);
#else
    typedef void *(*get_block_fn)(void *, void **, int, int, int);
    ((get_block_fn)(*dimension_vtable)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
        world->dimension, &block, position.x, position.y, position.z);
#endif
    if (!block) {
        set_detail(detail, detail_size, "unable to access block");
        return MP_WORLD_BAD_ARGUMENT;
    }

    enum mp_world_result result = MP_WORLD_BAD_ARGUMENT;
    void ***block_vtable = nullptr;
    if (!object_vtable(block, &block_vtable) ||
        !(*block_vtable)[ES_BLOCK_SLOT_GET_TYPE] ||
        !(*block_vtable)[ES_BLOCK_SLOT_DELETE]) {
        set_detail(detail, detail_size, "Endstone Block vtable is unavailable");
        goto destroy_block;
    }
    probe->block_found = true;
    probe->block = nullptr;
    probe->block_vptr = *block_vtable;
    if (trace) {
        trace->block_address = block;
        trace->block_vptr = *block_vtable;
        trace->get_type_target = (*block_vtable)[ES_BLOCK_SLOT_GET_TYPE];
        trace->block_delete_target =
            (*block_vtable)[ES_BLOCK_SLOT_DELETE];
    }

    void *type_target = nullptr;
    if (!read_string_return(block, ES_BLOCK_SLOT_GET_TYPE,
                            probe->block_type,
                            sizeof(probe->block_type), &type_target)) {
        set_detail(detail, detail_size, "Unable to read block type");
        goto destroy_block;
    }
    if (trace) trace->get_type_target = type_target;
    probe->is_air = mp_world_c_type_is_air(probe->block_type);
    probe->support_candidate =
        mp_world_c_type_is_support_candidate(probe->block_type);

    // Read only the verified EndstoneBlock block-source field.
    probe->block_source = *(void **)((char *)block +
                                     ES_ENDSTONE_BLOCK_OFF_BLOCK_SOURCE);
    if (probe->block_source) {
        probe->block_source_vptr = *(void **)probe->block_source;
        if (trace) {
            trace->block_source = probe->block_source;
            trace->block_source_vptr = probe->block_source_vptr;
        }
    }
    result = MP_WORLD_OK;

destroy_block:
    if (block_vtable && *block_vtable &&
        (*block_vtable)[ES_BLOCK_SLOT_DELETE]) {
#if defined(ES_PLATFORM_LINUX)
        typedef void (*delete_fn)(void *);
        ((delete_fn)(*block_vtable)[ES_BLOCK_SLOT_DELETE])(block);
#else
        typedef void (*delete_fn)(void *, unsigned int);
        ((delete_fn)(*block_vtable)[ES_BLOCK_SLOT_DELETE])(block, 1);
#endif
        if (trace) trace->block_destroy_count++;
    }
    return result;
}

enum mp_world_result mp_world_c_debug_probe_block(
    void *player, const char *expected_dimension,
    struct screen_pos position, struct mp_world_block_probe *probe,
    struct mp_world_c_trace *trace, char *detail, int detail_size)
{
    if (probe) memset(probe, 0, sizeof(*probe));
    if (trace) memset(trace, 0, sizeof(*trace));
    if (!probe) {
        set_detail(detail, detail_size, "block probe output is null");
        return MP_WORLD_BAD_ARGUMENT;
    }
    struct world_read_context world;
    if (!get_player_world(player, expected_dimension, &world, trace,
                          detail, detail_size)) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    return probe_block_in_world(&world, position, probe, trace,
                                detail, detail_size);
}

enum mp_world_result mp_world_probe_block(
    void *player, const char *expected_dimension,
    struct screen_pos position, struct mp_world_block_probe *probe,
    char *detail, int detail_size)
{
    return mp_world_c_debug_probe_block(
        player, expected_dimension, position, probe, nullptr,
        detail, detail_size);
}

static bool facing_valid(enum screen_facing facing)
{
    return facing == SCREEN_FACE_NORTH || facing == SCREEN_FACE_SOUTH ||
           facing == SCREEN_FACE_WEST || facing == SCREEN_FACE_EAST;
}

enum mp_world_result mp_world_validate_empty_tile(
    void *player, const char *expected_dimension,
    struct screen_pos cell, struct screen_pos backing,
    enum screen_facing facing, char *detail, int detail_size)
{
    if (!facing_valid(facing)) return MP_WORLD_BAD_ARGUMENT;
    struct mp_world_block_probe cell_probe = {0};
    struct mp_world_block_probe backing_probe = {0};
    enum mp_world_result result = mp_world_probe_block(
        player, expected_dimension, cell, &cell_probe,
        detail, detail_size);
    if (result != MP_WORLD_OK) return result;
    result = mp_world_probe_block(player, expected_dimension, backing,
                                  &backing_probe, detail, detail_size);
    if (result != MP_WORLD_OK) return result;
    if (!cell_probe.is_air) {
        set_detail(detail, detail_size, "screen cell is not air");
        return MP_WORLD_CELL_NOT_AIR;
    }
    if (!backing_probe.support_candidate) {
        if (detail && detail_size > 0) {
            snprintf(detail, (size_t)detail_size, "backing type=%s",
                     backing_probe.block_type);
        }
        return MP_WORLD_BACKING_NOT_SOLID;
    }
    return MP_WORLD_OK;
}

enum mp_world_result mp_world_inspect_tile(
    void *player, const char *expected_dimension,
    struct screen_pos cell, struct screen_pos backing,
    int64_t expected_map_id, struct mp_world_tile_state *state,
    char *detail, int detail_size)
{
    if (state) memset(state, 0, sizeof(*state));
    if (!state) return MP_WORLD_BAD_ARGUMENT;
    struct mp_world_block_probe cell_probe = {0};
    struct mp_world_block_probe backing_probe = {0};
    enum mp_world_result result = mp_world_probe_block(
        player, expected_dimension, cell, &cell_probe,
        detail, detail_size);
    if (result != MP_WORLD_OK) return result;
    result = mp_world_probe_block(player, expected_dimension, backing,
                                  &backing_probe, detail, detail_size);
    if (result != MP_WORLD_OK) return result;

    state->cell_is_air = cell_probe.is_air;
    state->backing_is_solid = backing_probe.support_candidate;
    state->frame_present =
        strcmp(cell_probe.block_type, "minecraft:frame") == 0;
    if (!state->frame_present) {
        if (detail && detail_size > 0) {
            snprintf(detail, (size_t)detail_size, "cell holds %s",
                     cell_probe.block_type);
        }
        return MP_WORLD_NOT_MANAGED_FRAME;
    }
    (void)expected_map_id;
    set_detail(detail, detail_size,
               "frame and backing are valid; v0.11 cannot inspect the installed map id");
    return MP_WORLD_MAP_ID_UNVERIFIABLE;
}

void *mp_world_get_map(void *server, int64_t map_id)
{
    if (!server || !*(void ***)server ||
        !(*(void ***)server)[ES_SERVER_SLOT_GET_MAP]) {
        return nullptr;
    }
    return es_server_get_map(server, map_id);
}

#else

bool mp_world_c_supported(void)
{
    return false;
}

bool mp_world_c_player_world(void *player, const char *expected_dimension,
                             void **dimension_out,
                             char *detail, int detail_size)
{
    (void)player; (void)expected_dimension;
    if (dimension_out) *dimension_out = nullptr;
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return false;
}

bool mp_world_c_debug_player_get_snapshot(
    void *player, struct mp_player_snapshot *snapshot,
    struct mp_world_c_trace *trace, char *detail, int detail_size)
{
    (void)player;
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
    if (trace) memset(trace, 0, sizeof(*trace));
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return false;
}

bool mp_player_get_snapshot(void *player, struct mp_player_snapshot *snapshot,
                            char *detail, int detail_size)
{
    return mp_world_c_debug_player_get_snapshot(
        player, snapshot, nullptr, detail, detail_size);
}

enum mp_world_result mp_world_c_debug_probe_block(
    void *player, const char *dimension, struct screen_pos position,
    struct mp_world_block_probe *probe, struct mp_world_c_trace *trace,
    char *detail, int detail_size)
{
    (void)player; (void)dimension; (void)position;
    if (probe) memset(probe, 0, sizeof(*probe));
    if (trace) memset(trace, 0, sizeof(*trace));
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return MP_WORLD_UNSUPPORTED;
}

enum mp_world_result mp_world_probe_block(
    void *player, const char *dimension, struct screen_pos position,
    struct mp_world_block_probe *probe, char *detail, int detail_size)
{
    return mp_world_c_debug_probe_block(
        player, dimension, position, probe, nullptr, detail, detail_size);
}

enum mp_world_result mp_world_validate_empty_tile(
    void *player, const char *dimension, struct screen_pos cell,
    struct screen_pos backing, enum screen_facing facing,
    char *detail, int detail_size)
{
    (void)player; (void)dimension; (void)cell; (void)backing; (void)facing;
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return MP_WORLD_UNSUPPORTED;
}

enum mp_world_result mp_world_inspect_tile(
    void *player, const char *dimension, struct screen_pos cell,
    struct screen_pos backing, int64_t map_id,
    struct mp_world_tile_state *state, char *detail, int detail_size)
{
    (void)player; (void)dimension; (void)cell; (void)backing; (void)map_id;
    if (state) memset(state, 0, sizeof(*state));
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return MP_WORLD_UNSUPPORTED;
}

void *mp_world_get_map(void *server, int64_t map_id)
{
    (void)server; (void)map_id;
        return nullptr;
}

#endif

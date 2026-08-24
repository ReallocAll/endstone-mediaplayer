#include "mediaplayer/bedrock/world_read_abi.h"

#include "abi_helpers.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/bedrock/world_write_abi.h"

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

struct world_read_context {
    struct es_location location;
    struct es_shared_handle dimension;
    void *dimension_object;
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

static void world_read_context_destroy(struct world_read_context *world)
{
    if (!world) return;
    es_location_release(&world->location);
    es_shared_release(&world->dimension);
    world->dimension_object = nullptr;
}

static bool member_return_handle0(void *object, size_t slot,
                                  struct es_shared_handle *out)
{
    if (!object || !out || !VTABLE(object)[slot]) return false;
    memset(out, 0, sizeof(*out));
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(object)[slot])(object, out);
#else
    ((void (*)(void *, void *))VTABLE(object)[slot])(out, object);
#endif
    return es_shared_object(out) != nullptr;
}

static bool member_return_identifier0(void *object, size_t slot,
                                      struct es_identifier *out)
{
    if (!object || !out || !VTABLE(object)[slot]) return false;
    memset(out, 0, sizeof(*out));
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(object)[slot])(object, out);
#else
    *out = ((struct es_identifier (*)(void *))VTABLE(object)[slot])(object);
#endif
    return true;
}

static bool object_identifier(void *object, size_t slot,
                              char *output, size_t output_size,
                              void **target)
{
    struct es_identifier identifier;
    if (!object || !VTABLE(object)[slot] ||
        !member_return_identifier0(object, slot, &identifier)) {
        return false;
    }
    if (target) *target = VTABLE(object)[slot];
    return es_identifier_copy_text(&identifier, output, output_size);
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

#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))
        (*player_vtable)[ES_PLAYER_SLOT_GET_LOCATION])(
            player, &world->location);
#else
    ((void (*)(void *, void *))
        (*player_vtable)[ES_PLAYER_SLOT_GET_LOCATION])(
            &world->location, player);
#endif
    if (!member_return_handle0(player, ES_PLAYER_SLOT_GET_DIMENSION,
                               &world->dimension)) {
        es_location_release(&world->location);
        set_detail(detail, detail_size, "Player dimension is unavailable");
        return false;
    }
    world->dimension_object = es_shared_object(&world->dimension);
    void *location_dimension = es_pointer_at(
        world->location.bytes + ES_LOCATION_OFF_DIMENSION,
        ES_WEAK_PTR_OFF_OBJECT);
    if (!world->dimension_object ||
        location_dimension != world->dimension_object) {
        world_read_context_destroy(world);
        set_detail(detail, detail_size,
                   "Player location and dimension handles disagree");
        return false;
    }

    void ***dimension_vtable = nullptr;
    if (!object_vtable(world->dimension_object, &dimension_vtable) ||
        !(*dimension_vtable)[ES_DIMENSION_SLOT_GET_ID]) {
        world_read_context_destroy(world);
        set_detail(detail, detail_size, "Endstone Dimension vtable is unavailable");
        return false;
    }
    if (trace) {
        trace->dimension = world->dimension_object;
        trace->dimension_vptr = *dimension_vtable;
        trace->get_id_target = (*dimension_vtable)[ES_DIMENSION_SLOT_GET_ID];
    }
    if (!object_identifier(world->dimension_object, ES_DIMENSION_SLOT_GET_ID,
                           world->dimension_id,
                           sizeof(world->dimension_id), nullptr)) {
        world_read_context_destroy(world);
        set_detail(detail, detail_size, "Unable to read dimension identifier");
        return false;
    }
    if (expected_dimension && expected_dimension[0] &&
        strcmp(expected_dimension, world->dimension_id) != 0) {
        world_read_context_destroy(world);
        set_detail(detail, detail_size, "Player is in a different dimension");
        return false;
    }
    return true;
}

static bool dimension_block(void *dimension, struct screen_pos position,
                            struct es_shared_handle *block)
{
    if (!dimension || !block ||
        !VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ]) {
        return false;
    }
    memset(block, 0, sizeof(*block));
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *, int, int, int))
        VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
            dimension, block, position.x, position.y, position.z);
#else
    ((void (*)(void *, void *, int, int, int))
        VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
            block, dimension, position.x, position.y, position.z);
#endif
    return es_shared_object(block) != nullptr;
}

static bool block_type_identifier(void *block, char *output,
                                  size_t output_size,
                                  struct mp_world_c_trace *trace)
{
    if (!block || !VTABLE(block)[ES_BLOCK_SLOT_GET_TYPE]) return false;
    void *block_type =
        ((void *(*)(void *))VTABLE(block)[ES_BLOCK_SLOT_GET_TYPE])(block);
    if (!block_type || !VTABLE(block_type)[ES_BLOCK_TYPE_SLOT_GET_ID]) {
        return false;
    }
    if (trace) {
        trace->get_type_target = VTABLE(block)[ES_BLOCK_SLOT_GET_TYPE];
        trace->block_type = block_type;
        trace->block_type_vptr = VTABLE(block_type);
        trace->block_type_get_id_target =
            VTABLE(block_type)[ES_BLOCK_TYPE_SLOT_GET_ID];
    }
    return object_identifier(block_type, ES_BLOCK_TYPE_SLOT_GET_ID,
                             output, output_size, nullptr);
}

bool mp_world_c_supported(void)
{
    return true;
}

bool mp_world_c_player_world(void *player, const char *expected_dimension,
                             struct es_shared_handle *dimension_out,
                             char *detail, int detail_size)
{
    if (!dimension_out) return false;
    memset(dimension_out, 0, sizeof(*dimension_out));
    struct world_read_context world;
    if (!get_player_world(player, expected_dimension, &world, nullptr,
                          detail, detail_size)) {
        return false;
    }
    memcpy(dimension_out, &world.dimension, sizeof(*dimension_out));
    memset(&world.dimension, 0, sizeof(world.dimension));
    es_location_release(&world.location);
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
    snapshot->x = es_location_float(&world.location, ES_LOCATION_OFF_X);
    snapshot->y = es_location_float(&world.location, ES_LOCATION_OFF_Y);
    snapshot->z = es_location_float(&world.location, ES_LOCATION_OFF_Z);
    snapshot->pitch = es_location_float(&world.location, ES_LOCATION_OFF_PITCH);
    snapshot->yaw = es_location_float(&world.location, ES_LOCATION_OFF_YAW);
    snapshot->block_x = (int)floorf(snapshot->x);
    snapshot->block_y = (int)floorf(snapshot->y);
    snapshot->block_z = (int)floorf(snapshot->z);
    snprintf(snapshot->dimension_id, sizeof(snapshot->dimension_id), "%s",
             world.dimension_id);
    world_read_context_destroy(&world);
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
    if (!world || !world->dimension_object || !probe ||
        !VTABLE(world->dimension_object)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ]) {
        set_detail(detail, detail_size, "Dimension block lookup is unavailable");
        return MP_WORLD_BAD_ARGUMENT;
    }
    if (trace) {
        trace->get_block_target = VTABLE(world->dimension_object)[
            ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ];
    }
    struct es_shared_handle block_handle = {0};
    if (!dimension_block(world->dimension_object, position, &block_handle)) {
        set_detail(detail, detail_size, "unable to access block");
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *block = es_shared_object(&block_handle);
    probe->block_found = true;
    probe->block = nullptr;
    probe->block_vptr = VTABLE(block);
    if (trace) {
        trace->block_address = block;
        trace->block_vptr = VTABLE(block);
    }
    enum mp_world_result result = MP_WORLD_BAD_ARGUMENT;
    if (block_type_identifier(block, probe->block_type,
                              sizeof(probe->block_type), trace)) {
        probe->is_air = mp_world_c_type_is_air(probe->block_type);
        probe->support_candidate =
            mp_world_c_type_is_support_candidate(probe->block_type);
        result = MP_WORLD_OK;
    }
    else {
        set_detail(detail, detail_size, "Unable to read block type identifier");
    }
    es_shared_release(&block_handle);
    if (trace) trace->handle_release_count++;
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
    enum mp_world_result result = probe_block_in_world(
        &world, position, probe, trace, detail, detail_size);
    world_read_context_destroy(&world);
    return result;
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
        player, expected_dimension, cell, &cell_probe, detail, detail_size);
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

static void item_impl_destroy(void *impl)
{
    if (!impl || !VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE]) return;
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, unsigned int))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE])(impl, 1);
#else
    ((void (*)(void *))VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE])(impl);
#endif
}

static bool optional_has_value(const struct es_optional_item_stack *optional)
{
    uint8_t engaged = 0;
    memcpy(&engaged, optional->bytes + ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE,
           sizeof(engaged));
    return engaged != 0;
}

static void *optional_item_impl(const struct es_optional_item_stack *optional)
{
    if (!optional_has_value(optional)) return nullptr;
    return es_pointer_at(optional->bytes + ES_OPTIONAL_ITEM_STACK_OFF_VALUE,
                         ES_ITEM_STACK_OFF_IMPL);
}

static bool item_impl_type_equals(void *impl, const char *expected)
{
    if (!impl || !VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_TYPE]) return false;
    void *type = ((void *(*)(void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_TYPE])(impl);
    char identifier[64];
    return type && object_identifier(type, ES_ITEM_TYPE_SLOT_GET_ID,
                                     identifier, sizeof(identifier), nullptr) &&
           strcmp(identifier, expected) == 0;
}

static bool item_impl_map_id(void *impl, int64_t *map_id)
{
    if (!map_id || !item_impl_type_equals(impl, "minecraft:filled_map") ||
        !VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_ITEM_META]) {
        return false;
    }
    struct es_shared_handle meta = {0};
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_ITEM_META])(impl, &meta);
#else
    ((void (*)(void *, void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_ITEM_META])(&meta, impl);
#endif
    void *object = es_shared_object(&meta);
    bool valid = object && VTABLE(object)[ES_MAP_META_SLOT_HAS_MAP_ID] &&
                 VTABLE(object)[ES_MAP_META_SLOT_GET_MAP_ID] &&
                 VCALL0(object, ES_MAP_META_SLOT_HAS_MAP_ID, bool);
    if (valid) {
        *map_id = VCALL0(object, ES_MAP_META_SLOT_GET_MAP_ID, int64_t);
    }
    es_shared_release(&meta);
    return valid;
}

static bool block_frame_map_id(void *block, int64_t *map_id)
{
    if (!block || !map_id || !VTABLE(block)[ES_BLOCK_SLOT_CAPTURE_STATE]) {
        return false;
    }
    struct es_shared_handle state = {0};
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(block)[ES_BLOCK_SLOT_CAPTURE_STATE])(
        block, &state);
#else
    ((void (*)(void *, void *))VTABLE(block)[ES_BLOCK_SLOT_CAPTURE_STATE])(
        &state, block);
#endif
    void *frame = es_shared_object(&state);
    struct es_optional_item_stack item = {0};
    if (!frame || !VTABLE(frame)[ES_ITEM_FRAME_SLOT_GET_ITEM]) {
        es_shared_release(&state);
        return false;
    }
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(frame)[ES_ITEM_FRAME_SLOT_GET_ITEM])(
        frame, &item);
#else
    ((void (*)(void *, void *))VTABLE(frame)[ES_ITEM_FRAME_SLOT_GET_ITEM])(
        &item, frame);
#endif
    void *impl = optional_item_impl(&item);
    bool valid = impl && item_impl_map_id(impl, map_id);
    item_impl_destroy(impl);
    es_shared_release(&state);
    return valid;
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
        player, expected_dimension, cell, &cell_probe, detail, detail_size);
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

    struct world_read_context world;
    if (!get_player_world(player, expected_dimension, &world, nullptr,
                          detail, detail_size)) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    struct es_shared_handle block = {0};
    bool found = dimension_block(world.dimension_object, cell, &block);
    bool readable = found && block_frame_map_id(es_shared_object(&block),
                                                &state->map_id);
    es_shared_release(&block);
    world_read_context_destroy(&world);
    if (!readable) {
        set_detail(detail, detail_size,
                   "item frame does not contain a readable filled map");
        return MP_WORLD_MAP_ID_MISMATCH;
    }
    if (state->map_id != expected_map_id) {
        set_detail(detail, detail_size, "item frame map id does not match");
        return MP_WORLD_MAP_ID_MISMATCH;
    }
    return MP_WORLD_OK;
}

void *mp_world_get_map(void *server, int64_t map_id)
{
    if (!server || !VTABLE(server)[ES_SERVER_SLOT_GET_MAP]) return nullptr;
    return es_server_get_map(server, map_id);
}

#else

bool mp_world_c_supported(void) { return false; }

bool mp_world_c_player_world(void *player, const char *expected_dimension,
                             struct es_shared_handle *dimension_out,
                             char *detail, int detail_size)
{
    (void)player;
    (void)expected_dimension;
    if (dimension_out) memset(dimension_out, 0, sizeof(*dimension_out));
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
    (void)player;
    (void)dimension;
    (void)position;
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
    (void)player;
    (void)dimension;
    (void)cell;
    (void)backing;
    (void)facing;
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return MP_WORLD_UNSUPPORTED;
}

enum mp_world_result mp_world_inspect_tile(
    void *player, const char *dimension, struct screen_pos cell,
    struct screen_pos backing, int64_t map_id,
    struct mp_world_tile_state *state, char *detail, int detail_size)
{
    (void)player;
    (void)dimension;
    (void)cell;
    (void)backing;
    (void)map_id;
    if (state) memset(state, 0, sizeof(*state));
    set_detail(detail, detail_size,
               "pure-C world ABI is not measured on this platform");
    return MP_WORLD_UNSUPPORTED;
}

void *mp_world_get_map(void *server, int64_t map_id)
{
    (void)server;
    (void)map_id;
    return nullptr;
}

#endif

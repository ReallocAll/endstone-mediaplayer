// Pure-C Endstone 0.12 managed ItemFrame placement and filled-map insertion.

#include "mediaplayer/bedrock/world_write_abi.h"

#include "abi_helpers.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/bedrock/world_bridge.h"
#include "mediaplayer/bedrock/world_read_abi.h"
#include "mediaplayer/endstone_api.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_detail(char *detail, int size, const char *message)
{
    if (detail && size > 0) {
        snprintf(detail, (size_t)size, "%s", message ? message : "");
    }
}

const char *mp_world_result_name(enum mp_world_result result)
{
    switch (result) {
    case MP_WORLD_OK: return "ok";
    case MP_WORLD_UNSUPPORTED: return "unsupported";
    case MP_WORLD_BAD_ARGUMENT: return "bad argument";
    case MP_WORLD_CELL_NOT_AIR: return "cell is not air";
    case MP_WORLD_BACKING_NOT_SOLID: return "backing is not solid";
    case MP_WORLD_MAP_ITEM_FAILED: return "map item creation failed";
    case MP_WORLD_FRAME_PLACE_FAILED: return "item-frame placement failed";
    case MP_WORLD_MAP_ID_MISMATCH: return "map id mismatch";
    case MP_WORLD_NOT_MANAGED_FRAME: return "managed frame missing";
    case MP_WORLD_INVENTORY_FULL: return "inventory capacity is not required";
    case MP_WORLD_MAP_DELIVERY_FAILED: return "item-frame map insertion failed";
    case MP_WORLD_MAP_ID_UNVERIFIABLE: return "map id is not readable";
    }
    return "unknown";
}

#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)

struct mp_world_prepared_tile {
    void *server;
    struct es_shared_handle dimension;
    struct es_shared_handle frame_data;
    struct screen_pos cell;
    struct screen_pos backing;
    enum screen_facing facing;
    int64_t map_id;
    void *map_item_impl;
    bool placed;
    bool installed;
};

struct es_item_stack_storage {
    _Alignas(ES_ITEM_STACK_ALIGN) unsigned char bytes[ES_ITEM_STACK_SIZE];
};

static_assert(sizeof(struct es_item_stack_storage) == ES_ITEM_STACK_SIZE);
static_assert(_Alignof(struct es_item_stack_storage) == ES_ITEM_STACK_ALIGN);

static void *slot_target(void *object, size_t slot)
{
    if (!object || !*(void ***)object) return nullptr;
    return VTABLE(object)[slot];
}

static int facing_state(enum screen_facing facing)
{
    switch (facing) {
    case SCREEN_FACE_NORTH: return 2;
    case SCREEN_FACE_SOUTH: return 3;
    case SCREEN_FACE_WEST: return 4;
    case SCREEN_FACE_EAST: return 5;
    }
    return -1;
}

static bool member_return_handle0(void *object, size_t slot,
                                  struct es_shared_handle *out)
{
    if (!object || !out || !slot_target(object, slot)) return false;
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
    if (!object || !out || !slot_target(object, slot)) return false;
    memset(out, 0, sizeof(*out));
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(object)[slot])(object, out);
#else
    *out = ((struct es_identifier (*)(void *))VTABLE(object)[slot])(object);
#endif
    return true;
}

static bool object_identifier_equals(void *object, size_t slot,
                                     const char *expected)
{
    struct es_identifier identifier;
    char text[64];
    return member_return_identifier0(object, slot, &identifier) &&
           es_identifier_copy_text(&identifier, text, sizeof(text)) &&
           strcmp(text, expected) == 0;
}

static bool block_at(void *dimension, struct screen_pos position,
                     struct es_shared_handle *block)
{
    if (!dimension || !block ||
        !slot_target(dimension, ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ)) {
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

static bool block_type_equals(void *block, const char *expected)
{
    if (!block || !slot_target(block, ES_BLOCK_SLOT_GET_TYPE)) return false;
    void *type = ((void *(*)(void *))
        VTABLE(block)[ES_BLOCK_SLOT_GET_TYPE])(block);
    return type && object_identifier_equals(
        type, ES_BLOCK_TYPE_SLOT_GET_ID, expected);
}

static void block_set_data(void *block, const struct es_shared_handle *data,
                           bool apply_physics)
{
    void *object = es_shared_object(data);
    if (block && object && slot_target(block, ES_BLOCK_SLOT_SET_DATA)) {
        ((void (*)(void *, const void *, bool))
            VTABLE(block)[ES_BLOCK_SLOT_SET_DATA])(
                block, object, apply_physics);
    }
}

static void block_states_destroy_local(struct es_block_states *states)
{
    if (!states) return;
#if ES_PLATFORM_WINDOWS
    struct es_block_state_node *sentinel = states->head;
    struct es_block_state_node *node = sentinel ? sentinel->next : nullptr;
    if (node && node != sentinel) {
        cpp_string_destroy(node->key);
        free(node);
    }
    free(sentinel);
#else
    struct es_block_state_node *node = states->first_node;
    if (node) {
        cpp_string_destroy(node->key);
        free(node);
    }
    free(states->buckets);
#endif
    memset(states, 0, sizeof(*states));
}

static bool frame_block_states(struct es_block_states *states,
                               int facing_value)
{
    if (!states) return false;
    memset(states, 0, sizeof(*states));
    struct es_block_state_node *node = calloc(1, sizeof(*node));
    if (!node) return false;
#if ES_PLATFORM_WINDOWS
    struct es_block_state_node *sentinel = calloc(1, sizeof(*sentinel));
    if (!sentinel) {
        free(node);
        return false;
    }
    cpp_string_construct(node->key, "facing_direction");
    node->next = sentinel;
    node->prev = sentinel;
    sentinel->next = node;
    sentinel->prev = node;
    states->max_load_factor = 1.0f;
    states->head = sentinel;
    states->size = 1;
    states->mask = 7;
    states->maxidx = 8;
#else
    void **buckets = calloc(2, sizeof(*buckets));
    if (!buckets) {
        free(node);
        return false;
    }
    cpp_string_construct(node->key, "facing_direction");
    node->hash = 1;
    states->buckets = buckets;
    states->bucket_count = 2;
    states->first_node = node;
    states->size = 1;
    states->max_load_factor = 1.0f;
    buckets[node->hash % states->bucket_count] = &states->first_node;
#endif
    *(int32_t *)node->variant_storage = facing_value;
    node->variant_index = ES_BLOCK_STATE_WHICH_INT;
    return true;
}

static bool create_block_data(void *server, const char *type_name,
                              struct es_block_states *states,
                              struct es_shared_handle *out)
{
    if (!server || !out) return false;
    size_t slot = states ? ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES
                         : ES_SERVER_SLOT_CREATE_BLOCK_DATA;
    if (!slot_target(server, slot)) return false;
    struct es_identifier type;
    es_identifier_init(&type, type_name);
    memset(out, 0, sizeof(*out));
    if (states) {
#if ES_PLATFORM_WINDOWS
        ((void (*)(void *, void *, struct es_identifier, void *))
            VTABLE(server)[slot])(server, out, type, states);
#else
        ((void (*)(void *, void *, struct es_identifier, void *))
            VTABLE(server)[slot])(out, server, type, states);
#endif
        if (!ES_C_ABI_BLOCK_STATES_CALLEE_DESTROYS) {
            block_states_destroy_local(states);
        }
        else {
            memset(states, 0, sizeof(*states));
        }
    }
    else {
#if ES_PLATFORM_WINDOWS
        ((void (*)(void *, void *, struct es_identifier))
            VTABLE(server)[slot])(server, out, type);
#else
        ((void (*)(void *, void *, struct es_identifier))
            VTABLE(server)[slot])(out, server, type);
#endif
    }
    return es_shared_object(out) != nullptr;
}

static bool create_frame_block_data(void *server, int facing_value,
                                    struct es_shared_handle *out)
{
    struct es_block_states states;
    if (!frame_block_states(&states, facing_value)) return false;
    bool created = create_block_data(server, "minecraft:frame", &states, out);
#if ES_PLATFORM_WINDOWS
    if (!created && states.head) {
#else
    if (!created && states.buckets) {
#endif
        block_states_destroy_local(&states);
    }
    return created;
}

static bool create_air_block_data(void *server, struct es_shared_handle *out)
{
    return create_block_data(server, "minecraft:air", nullptr, out);
}

static void item_impl_destroy(void *impl)
{
    if (!impl || !slot_target(impl, ES_ITEM_STACK_SLOT_DELETE)) return;
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, unsigned int))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE])(impl, 1);
#else
    ((void (*)(void *))VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE])(impl);
#endif
}

static void *create_filled_map_item(void *server, char *detail,
                                    int detail_size)
{
    void *base = endstone_expected_image_base();
    if (!base || !endstone_expected_image_matches(base)) {
        set_detail(detail, detail_size,
                   "exact Endstone runtime image identity does not match ABI headers");
        return nullptr;
    }
    const void *type_info =
        (const char *)base + ES_ITEM_TYPE_TYPEINFO_RVA;
    void *registry = ((void *(*)(void *, const void *))
        VTABLE(server)[ES_SERVER_SLOT_GET_REGISTRY])(server, type_info);
    if (!registry) {
        set_detail(detail, detail_size, "ItemType registry is unavailable");
        return nullptr;
    }
    struct es_identifier identifier;
    es_identifier_init(&identifier, "minecraft:filled_map");
    void *item_type = ((void *(*)(void *, struct es_identifier))
        VTABLE(registry)[ES_ITEM_REGISTRY_SLOT_GET])(registry, identifier);
    if (!item_type) {
        set_detail(detail, detail_size, "filled_map item type is unknown");
        return nullptr;
    }
    struct es_item_stack_storage stack = {0};
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *, int))
        VTABLE(item_type)[ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK])(
            item_type, &stack, 1);
#else
    ((void (*)(void *, void *, int))
        VTABLE(item_type)[ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK])(
            &stack, item_type, 1);
#endif
    void *impl = es_pointer_at(stack.bytes, ES_ITEM_STACK_OFF_IMPL);
    es_store_pointer(stack.bytes, ES_ITEM_STACK_OFF_IMPL, nullptr);
    if (!impl) {
        set_detail(detail, detail_size, "createItemStack returned no item");
    }
    return impl;
}

static bool item_get_meta(void *impl, struct es_shared_handle *meta)
{
    return member_return_handle0(impl, ES_ITEM_STACK_SLOT_GET_ITEM_META, meta);
}

static bool item_type_equals(void *impl, const char *expected)
{
    if (!impl || !slot_target(impl, ES_ITEM_STACK_SLOT_GET_TYPE)) return false;
    void *type = ((void *(*)(void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_TYPE])(impl);
    return type && object_identifier_equals(
        type, ES_ITEM_TYPE_SLOT_GET_ID, expected);
}

static bool meta_has_map_id(void *meta)
{
    return VCALL0(meta, ES_MAP_META_SLOT_HAS_MAP_ID, bool);
}

static int64_t meta_get_map_id(void *meta)
{
    return VCALL0(meta, ES_MAP_META_SLOT_GET_MAP_ID, int64_t);
}

static void meta_set_map_view(void *meta, void *map_view)
{
    ((void (*)(void *, const void *))
        VTABLE(meta)[ES_MAP_META_SLOT_SET_MAP_VIEW])(meta, map_view);
}

static bool item_set_meta(void *impl, void *meta)
{
    return ((bool (*)(void *, const void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_SET_ITEM_META])(impl, meta);
}

static bool item_impl_map_id(void *impl, int64_t *map_id)
{
    if (!map_id || !item_type_equals(impl, "minecraft:filled_map")) {
        return false;
    }
    struct es_shared_handle meta = {0};
    if (!item_get_meta(impl, &meta)) return false;
    void *object = es_shared_object(&meta);
    bool valid = object && meta_has_map_id(object);
    if (valid) *map_id = meta_get_map_id(object);
    es_shared_release(&meta);
    return valid;
}

static void optional_item_init(struct es_optional_item_stack *optional,
                               void *impl)
{
    memset(optional, 0, sizeof(*optional));
    es_store_pointer(optional->bytes + ES_OPTIONAL_ITEM_STACK_OFF_VALUE,
                     ES_ITEM_STACK_OFF_IMPL, impl);
    optional->bytes[ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE] = 1;
}

static void *optional_item_impl(const struct es_optional_item_stack *optional)
{
    if (!optional ||
        optional->bytes[ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE] == 0) {
        return nullptr;
    }
    return es_pointer_at(optional->bytes + ES_OPTIONAL_ITEM_STACK_OFF_VALUE,
                         ES_ITEM_STACK_OFF_IMPL);
}

static bool capture_frame(void *block, struct es_shared_handle *state)
{
    return member_return_handle0(block, ES_BLOCK_SLOT_CAPTURE_STATE, state);
}

static bool frame_set_item(void *frame, void *impl)
{
    if (!frame || !impl || !slot_target(frame, ES_ITEM_FRAME_SLOT_SET_ITEM)) {
        return false;
    }
    struct es_optional_item_stack optional;
    optional_item_init(&optional, impl);
    ((void (*)(void *, const void *))
        VTABLE(frame)[ES_ITEM_FRAME_SLOT_SET_ITEM])(frame, &optional);
    return true;
}

static bool frame_get_map_id(void *frame, int64_t *map_id)
{
    if (!frame || !map_id ||
        !slot_target(frame, ES_ITEM_FRAME_SLOT_GET_ITEM)) {
        return false;
    }
    struct es_optional_item_stack optional = {0};
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(frame)[ES_ITEM_FRAME_SLOT_GET_ITEM])(
        frame, &optional);
#else
    ((void (*)(void *, void *))VTABLE(frame)[ES_ITEM_FRAME_SLOT_GET_ITEM])(
        &optional, frame);
#endif
    void *impl = optional_item_impl(&optional);
    bool valid = impl && item_impl_map_id(impl, map_id);
    item_impl_destroy(impl);
    return valid;
}

static bool block_frame_map_id(void *block, int64_t *map_id)
{
    struct es_shared_handle state = {0};
    if (!capture_frame(block, &state)) return false;
    bool valid = frame_get_map_id(es_shared_object(&state), map_id);
    es_shared_release(&state);
    return valid;
}

#if defined(MP_TESTING)
bool mp_world_test_item_frame_roundtrip(void *block, void *item_impl,
                                        int64_t expected_map_id)
{
    struct es_shared_handle state = {0};
    if (!capture_frame(block, &state)) return false;
    bool installed = frame_set_item(es_shared_object(&state), item_impl);
    es_shared_release(&state);
    if (!installed) return false;
    int64_t actual_map_id = -1;
    return block_frame_map_id(block, &actual_map_id) &&
           actual_map_id == expected_map_id;
}
#endif

static bool set_cell_air(void *server, void *dimension,
                         struct screen_pos cell)
{
    struct es_shared_handle block = {0};
    struct es_shared_handle air = {0};
    if (!block_at(dimension, cell, &block) ||
        !create_air_block_data(server, &air)) {
        es_shared_release(&block);
        es_shared_release(&air);
        return false;
    }
    block_set_data(es_shared_object(&block), &air, true);
    es_shared_release(&air);
    es_shared_release(&block);
    struct es_shared_handle verify = {0};
    bool result = block_at(dimension, cell, &verify) &&
                  block_type_equals(es_shared_object(&verify),
                                    "minecraft:air");
    es_shared_release(&verify);
    return result;
}

bool mp_world_managed_frames_supported(void)
{
    return endstone_expected_image_base() != nullptr;
}

enum mp_world_result mp_world_check_inventory_capacity(
    void *player, int required_slots, int *available_slots,
    char *detail, int detail_size)
{
    if (available_slots) *available_slots = 0;
    if (!player || required_slots < 0) return MP_WORLD_BAD_ARGUMENT;
    if (available_slots) *available_slots = INT_MAX;
    set_detail(detail, detail_size,
               "inventory is not used; maps are inserted into ItemFrame state");
    return MP_WORLD_OK;
}

void *mp_world_create_map_for_player(void *server, void *player,
                                     const char *expected_dimension,
                                     char *detail, int detail_size)
{
    if (!server) {
        set_detail(detail, detail_size, "Endstone Server is null");
        return nullptr;
    }
    struct es_shared_handle dimension = {0};
    if (!mp_world_c_player_world(player, expected_dimension, &dimension,
                                 detail, detail_size)) {
        return nullptr;
    }
    void *map_view = es_server_create_map(server, &dimension);
    es_shared_release(&dimension);
    if (!map_view) {
        set_detail(detail, detail_size, "Server createMap returned no MapView");
    }
    return map_view;
}

enum mp_world_result mp_world_prepare_tile(
    void *server, void *player, const char *expected_dimension,
    struct screen_pos cell, struct screen_pos backing,
    enum screen_facing facing, void *map_view, int64_t map_id,
    const char *screen_name, int tile_index, int tile_count, int row,
    int column, struct mp_world_prepared_tile **out,
    char *detail, int detail_size)
{
    if (!out || !map_view || !server) return MP_WORLD_BAD_ARGUMENT;
    (void)screen_name;
    (void)tile_index;
    (void)tile_count;
    (void)row;
    (void)column;
    *out = nullptr;
    enum mp_world_result validation = mp_world_validate_empty_tile(
        player, expected_dimension, cell, backing, facing,
        detail, detail_size);
    if (validation != MP_WORLD_OK) return validation;

    struct mp_world_prepared_tile *prepared = calloc(1, sizeof(*prepared));
    if (!prepared) return MP_WORLD_MAP_ITEM_FAILED;
    prepared->server = server;
    prepared->cell = cell;
    prepared->backing = backing;
    prepared->facing = facing;
    prepared->map_id = map_id;
    if (!mp_world_c_player_world(player, expected_dimension,
                                 &prepared->dimension,
                                 detail, detail_size) ||
        !create_frame_block_data(server, facing_state(facing),
                                 &prepared->frame_data)) {
        mp_world_prepared_destroy(prepared);
        set_detail(detail, detail_size,
                   "unable to create item-frame block data");
        return MP_WORLD_FRAME_PLACE_FAILED;
    }

    void *impl = create_filled_map_item(server, detail, detail_size);
    struct es_shared_handle meta = {0};
    if (!impl || !item_get_meta(impl, &meta)) {
        item_impl_destroy(impl);
        mp_world_prepared_destroy(prepared);
        return MP_WORLD_MAP_ITEM_FAILED;
    }
    void *meta_object = es_shared_object(&meta);
    meta_set_map_view(meta_object, map_view);
    bool valid = meta_has_map_id(meta_object) &&
                 meta_get_map_id(meta_object) == map_id &&
                 item_set_meta(impl, meta_object);
    es_shared_release(&meta);
    if (!valid) {
        item_impl_destroy(impl);
        mp_world_prepared_destroy(prepared);
        return MP_WORLD_MAP_ID_MISMATCH;
    }
    prepared->map_item_impl = impl;
    *out = prepared;
    return MP_WORLD_OK;
}

enum mp_world_result mp_world_place_prepared(
    struct mp_world_prepared_tile *prepared, char *detail, int detail_size)
{
    if (!prepared || !es_shared_object(&prepared->dimension) ||
        !es_shared_object(&prepared->frame_data) ||
        !prepared->map_item_impl) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *dimension = es_shared_object(&prepared->dimension);
    struct es_shared_handle block = {0};
    if (!block_at(dimension, prepared->cell, &block) ||
        !block_type_equals(es_shared_object(&block), "minecraft:air")) {
        es_shared_release(&block);
        set_detail(detail, detail_size, "screen cell changed before placement");
        return MP_WORLD_CELL_NOT_AIR;
    }
    block_set_data(es_shared_object(&block), &prepared->frame_data, true);
    es_shared_release(&block);
    prepared->placed = true;

    struct es_shared_handle placed = {0};
    struct es_shared_handle state = {0};
    bool installed = block_at(dimension, prepared->cell, &placed) &&
        block_type_equals(es_shared_object(&placed), "minecraft:frame") &&
        capture_frame(es_shared_object(&placed), &state) &&
        frame_set_item(es_shared_object(&state), prepared->map_item_impl);
    es_shared_release(&state);
    es_shared_release(&placed);
    if (!installed) {
        (void)set_cell_air(prepared->server, dimension, prepared->cell);
        prepared->placed = false;
        set_detail(detail, detail_size,
                   "unable to insert filled map into captured ItemFrame state");
        return MP_WORLD_MAP_DELIVERY_FAILED;
    }

    struct es_shared_handle verify_block = {0};
    int64_t installed_map_id = -1;
    bool verified = block_at(dimension, prepared->cell, &verify_block) &&
        block_type_equals(es_shared_object(&verify_block), "minecraft:frame") &&
        block_frame_map_id(es_shared_object(&verify_block),
                           &installed_map_id) &&
        installed_map_id == prepared->map_id;
    es_shared_release(&verify_block);
    if (!verified) {
        (void)set_cell_air(prepared->server, dimension, prepared->cell);
        prepared->placed = false;
        set_detail(detail, detail_size,
                   "ItemFrame getItem verification returned a different map id");
        return MP_WORLD_MAP_ID_MISMATCH;
    }
    prepared->installed = true;
    return MP_WORLD_OK;
}

enum mp_world_result mp_world_deliver_prepared_maps(
    void *player, struct mp_world_prepared_tile *const *prepared, int count,
    char *detail, int detail_size)
{
    if (!player || !prepared || count < 0) return MP_WORLD_BAD_ARGUMENT;
    for (int index = 0; index < count; index++) {
        if (!prepared[index] || !prepared[index]->installed) {
            set_detail(detail, detail_size,
                       "one or more ItemFrame map insertions were not verified");
            return MP_WORLD_MAP_DELIVERY_FAILED;
        }
    }
    set_detail(detail, detail_size,
               "all maps were inserted and verified through ItemFrame state");
    return MP_WORLD_OK;
}

void mp_world_retract_prepared_maps(
    void *player, struct mp_world_prepared_tile *const *prepared, int count)
{
    (void)player;
    (void)prepared;
    (void)count;
}

void mp_world_prepared_destroy(struct mp_world_prepared_tile *prepared)
{
    if (!prepared) return;
    es_shared_release(&prepared->frame_data);
    es_shared_release(&prepared->dimension);
    item_impl_destroy(prepared->map_item_impl);
    free(prepared);
}

enum mp_world_result mp_world_remove_managed(
    void *server, void *player, const char *expected_dimension,
    struct screen_pos cell, int64_t expected_map_id,
    char *detail, int detail_size)
{
    if (!server) return MP_WORLD_BAD_ARGUMENT;
    struct es_shared_handle dimension = {0};
    if (!mp_world_c_player_world(player, expected_dimension, &dimension,
                                 detail, detail_size)) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *dimension_object = es_shared_object(&dimension);
    struct es_shared_handle block = {0};
    if (!block_at(dimension_object, cell, &block)) {
        es_shared_release(&dimension);
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *block_object = es_shared_object(&block);
    if (block_type_equals(block_object, "minecraft:air")) {
        es_shared_release(&block);
        es_shared_release(&dimension);
        return MP_WORLD_OK;
    }
    if (!block_type_equals(block_object, "minecraft:frame")) {
        es_shared_release(&block);
        es_shared_release(&dimension);
        set_detail(detail, detail_size, "refusing to remove a non-frame block");
        return MP_WORLD_NOT_MANAGED_FRAME;
    }
    if (expected_map_id >= 0) {
        int64_t actual_map_id = -1;
        if (!block_frame_map_id(block_object, &actual_map_id) ||
            actual_map_id != expected_map_id) {
            es_shared_release(&block);
            es_shared_release(&dimension);
            set_detail(detail, detail_size,
                       "refusing to remove an ItemFrame with a different map id");
            return MP_WORLD_MAP_ID_MISMATCH;
        }
    }
    es_shared_release(&block);
    bool removed = set_cell_air(server, dimension_object, cell);
    es_shared_release(&dimension);
    return removed ? MP_WORLD_OK : MP_WORLD_FRAME_PLACE_FAILED;
}

void mp_world_rollback_placed(void *server, void *player,
                              const char *expected_dimension,
                              struct screen_pos cell)
{
    char ignored[1] = {0};
    (void)mp_world_remove_managed(server, player, expected_dimension, cell,
                                  -1, ignored, 0);
}

#else

static enum mp_world_result unsupported(char *detail, int detail_size)
{
    set_detail(detail, detail_size,
               "managed ItemFrame API is not measured on this platform");
    return MP_WORLD_UNSUPPORTED;
}

bool mp_world_managed_frames_supported(void) { return false; }

enum mp_world_result mp_world_check_inventory_capacity(
    void *player, int required_slots, int *available_slots,
    char *detail, int detail_size)
{
    (void)player;
    (void)required_slots;
    if (available_slots) *available_slots = 0;
    return unsupported(detail, detail_size);
}

void *mp_world_create_map_for_player(void *server, void *player,
                                     const char *expected_dimension,
                                     char *detail, int detail_size)
{
    (void)server;
    (void)player;
    (void)expected_dimension;
    unsupported(detail, detail_size);
    return nullptr;
}

enum mp_world_result mp_world_prepare_tile(
    void *server, void *player, const char *expected_dimension,
    struct screen_pos cell, struct screen_pos backing,
    enum screen_facing facing, void *map_view, int64_t map_id,
    const char *screen_name, int tile_index, int tile_count, int row,
    int column, struct mp_world_prepared_tile **out,
    char *detail, int detail_size)
{
    (void)server; (void)player; (void)expected_dimension; (void)cell;
    (void)backing; (void)facing; (void)map_view; (void)map_id;
    (void)screen_name; (void)tile_index; (void)tile_count; (void)row;
    (void)column;
    if (out) *out = nullptr;
    return unsupported(detail, detail_size);
}

enum mp_world_result mp_world_place_prepared(
    struct mp_world_prepared_tile *prepared, char *detail, int detail_size)
{
    (void)prepared;
    return unsupported(detail, detail_size);
}

enum mp_world_result mp_world_deliver_prepared_maps(
    void *player, struct mp_world_prepared_tile *const *prepared, int count,
    char *detail, int detail_size)
{
    (void)player; (void)prepared; (void)count;
    return unsupported(detail, detail_size);
}

void mp_world_retract_prepared_maps(
    void *player, struct mp_world_prepared_tile *const *prepared, int count)
{
    (void)player; (void)prepared; (void)count;
}

void mp_world_prepared_destroy(struct mp_world_prepared_tile *prepared)
{
    (void)prepared;
}

enum mp_world_result mp_world_remove_managed(
    void *server, void *player, const char *expected_dimension,
    struct screen_pos cell, int64_t expected_map_id,
    char *detail, int detail_size)
{
    (void)server; (void)player; (void)expected_dimension; (void)cell;
    (void)expected_map_id;
    return unsupported(detail, detail_size);
}

void mp_world_rollback_placed(void *server, void *player,
                              const char *expected_dimension,
                              struct screen_pos cell)
{
    (void)server; (void)player; (void)expected_dimension; (void)cell;
}

#endif

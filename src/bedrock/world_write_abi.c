// Pure-C managed-frame placement and map-item delivery.

#include "mediaplayer/bedrock/world_write_abi.h"

#include "abi_helpers.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/bedrock/world_bridge.h"
#include "mediaplayer/bedrock/world_read_abi.h"

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
    case MP_WORLD_INVENTORY_FULL: return "not enough empty inventory slots";
    case MP_WORLD_MAP_DELIVERY_FAILED: return "map delivery failed";
    case MP_WORLD_MAP_ID_UNVERIFIABLE:
        return "map id cannot be inspected on Endstone 0.11";
    }
    return "unknown";
}

#if defined(ES_PLATFORM_WINDOWS) || defined(ES_PLATFORM_LINUX)

// Opaque prepared mutation with a borrowed Dimension.
struct mp_world_prepared_tile {
    void *dimension;
    struct screen_pos cell;
    struct screen_pos backing;
    enum screen_facing facing;
    int64_t map_id;
    void *frame_data;    // owned unique_ptr<BlockData> payload
    void *map_item_impl; // owned EndstoneItemStack impl (unique_ptr<Impl>)
    int delivered_slot;
};

// Returns a virtual target or nullptr.
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

// --- Block helpers ---

// Returns an owned EndstoneBlock.
static void *block_at(void *dimension, struct screen_pos position)
{
    if (!slot_target(dimension, ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ)) {
        return nullptr;
    }
    void *block = nullptr;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void **, void *, int, int, int))
        VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
        &block, dimension, position.x, position.y, position.z);
#else
    ((void *(*)(void *, void **, int, int, int))
        VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
        dimension, &block, position.x, position.y, position.z);
#endif
    return block;
}

static void block_destroy(void *block)
{
    if (!block || !slot_target(block, ES_BLOCK_SLOT_DELETE)) return;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void *))VTABLE(block)[ES_BLOCK_SLOT_DELETE])(block);
#else
    ((void (*)(void *, unsigned int))VTABLE(block)[ES_BLOCK_SLOT_DELETE])(
        block, 1);
#endif
}

// Performs an exact block-type check.
static bool block_type_equals(void *block, const char *expected)
{
    if (!slot_target(block, ES_BLOCK_SLOT_GET_TYPE)) return false;
    _Alignas(void *) unsigned char type[ES_STRING_SIZE] = {0};
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void *, void *))VTABLE(block)[ES_BLOCK_SLOT_GET_TYPE])(
        type, block);
#else
    ((void *(*)(void *, void *))VTABLE(block)[ES_BLOCK_SLOT_GET_TYPE])(
        block, type);
#endif
    bool matched = strcmp(cpp_string_str(type), expected) == 0;
    cpp_string_destroy(type);
    return matched;
}

static void block_set_data(void *block, void *block_data, bool apply_physics)
{
    ((void (*)(void *, void *, bool))VTABLE(block)[ES_BLOCK_SLOT_SET_DATA])(
        block, block_data, apply_physics);
}

static void block_data_destroy(void *block_data)
{
    if (!block_data || !slot_target(block_data, ES_BLOCK_DATA_SLOT_DELETE)) {
        return;
    }
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void *))VTABLE(block_data)[ES_BLOCK_DATA_SLOT_DELETE])(
        block_data);
#else
    ((void (*)(void *, unsigned int))
        VTABLE(block_data)[ES_BLOCK_DATA_SLOT_DELETE])(block_data, 1);
#endif
}

// Creates owned frame BlockData with the requested facing.
static void *create_frame_block_data(void *server, int facing_value)
{
    if (!slot_target(server, ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES)) {
        return nullptr;
    }
    // The callee consumes the state nodes and key buffer.
    struct es_block_state_node *node = calloc(1, sizeof(*node));
#if defined(ES_PLATFORM_WINDOWS)
    struct es_block_state_node *sentinel = calloc(1, sizeof(*sentinel));
    if (!node || !sentinel) {
        free(node);
        free(sentinel);
        return nullptr;
    }
    cpp_string_construct(node->key, "facing_direction");
    // Reject a failed heap-backed key construction.
    if (*(size_t *)(node->key + 24) != 31) {
        cpp_string_destroy(node->key);
        free(node);
        free(sentinel);
        return nullptr;
    }
#else
    void **buckets = calloc(2, sizeof(*buckets));
    if (!node || !buckets) {
        free(node);
        free(buckets);
        return nullptr;
    }
    cpp_string_construct(node->key, "facing_direction");
#endif
    *(int32_t *)node->variant_storage = facing_value;
    node->variant_index = ES_BLOCK_STATE_WHICH_INT;
#if defined(ES_PLATFORM_WINDOWS)
    node->next = sentinel;
    node->prev = sentinel;
    sentinel->next = node;
    sentinel->prev = node;

    struct es_block_states states = {
        .max_load_factor = 1.0f,
        .head = sentinel,
        .size = 1,
        .mask = 7,
        .maxidx = 8,
    };
#else
    node->hash = 1;
    struct es_block_states states = {
        .buckets = buckets,
        .bucket_count = 2,
        .first_node = node,
        .size = 1,
        .max_load_factor = 1.0f,
    };
    buckets[node->hash % states.bucket_count] = &states.first_node;
#endif

    // The callee consumes both by-value parameters.
    _Alignas(void *) unsigned char type_name[ES_STRING_SIZE];
    cpp_string_construct(type_name, "minecraft:frame");
    void *block_data = nullptr;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void **, void *, void *, void *))
        VTABLE(server)[ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES])(
        &block_data, server, type_name, &states);
    memset(type_name, 0, sizeof(type_name));
#else
    STR_GUARD(type_name,
              ((void *(*)(void *, void **, void *, void *))
                  VTABLE(server)[ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES])(
                  server, &block_data, type_name, &states));
#endif
    return block_data;
}

// Creates owned air BlockData.
static void *create_air_block_data(void *server)
{
    if (!slot_target(server, ES_SERVER_SLOT_CREATE_BLOCK_DATA)) {
        return nullptr;
    }
    _Alignas(void *) unsigned char type_name[ES_STRING_SIZE];
    cpp_string_construct(type_name, "minecraft:air"); // 13 chars: SSO
    void *block_data = nullptr;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void **, void *, void *))
        VTABLE(server)[ES_SERVER_SLOT_CREATE_BLOCK_DATA])(
        &block_data, server, type_name);
    memset(type_name, 0, sizeof(type_name));
#else
    STR_GUARD(type_name,
              ((void *(*)(void *, void **, void *))
                  VTABLE(server)[ES_SERVER_SLOT_CREATE_BLOCK_DATA])(
                  server, &block_data, type_name));
#endif
    return block_data;
}

// --- ItemStack and ItemMeta helpers ---

// Creates an owned one-item filled-map stack.
static void *create_filled_map_item(void *server, char *detail, int detail_size)
{
    if (!slot_target(server, ES_SERVER_SLOT_GET_REGISTRY)) {
        set_detail(detail, detail_size, "Server registry lookup is unavailable");
        return nullptr;
    }
    // The registry name remains caller-owned.
    _Alignas(void *) unsigned char registry_name[ES_STRING_SIZE];
    cpp_string_construct(registry_name, "ItemType");
    void *registry =
        ((void *(*)(void *, void *))
            VTABLE(server)[ES_SERVER_SLOT_GET_REGISTRY])(
            server, registry_name);
    cpp_string_destroy(registry_name);
    if (!registry) {
        set_detail(detail, detail_size, "ItemType registry is unavailable");
        return nullptr;
    }

    struct es_identifier identifier = {
        .ns = "minecraft",
        .ns_len = 9,
        .key = "filled_map",
        .key_len = 10,
    };
#if defined(ES_PLATFORM_LINUX)
    void *item_type =
        ((void *(*)(void *, struct es_identifier))
            VTABLE(registry)[ES_ITEM_REGISTRY_SLOT_GET])(
            registry, identifier);
#else
    void *item_type =
        ((void *(*)(void *, struct es_identifier *))
            VTABLE(registry)[ES_ITEM_REGISTRY_SLOT_GET])(
            registry, &identifier);
#endif
    if (!item_type) {
        set_detail(detail, detail_size, "filled_map item type is unknown");
        return nullptr;
    }

    void *impl = nullptr;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void **, void *, int))VTABLE(item_type)[
        ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK])(&impl, item_type, 1);
#else
    ((void *(*)(void *, void **, int))
        VTABLE(item_type)[ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK])(
        item_type, &impl, 1);
#endif
    if (!impl) {
        set_detail(detail, detail_size, "createItemStack returned no item");
    }
    return impl;
}

static void item_impl_destroy(void *impl)
{
    if (!impl || !slot_target(impl, ES_ITEM_STACK_SLOT_DELETE)) return;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void *))VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE])(impl);
#else
    ((void (*)(void *, unsigned int))VTABLE(impl)[ES_ITEM_STACK_SLOT_DELETE])(
        impl, 1);
#endif
}

// Returns owned ItemMeta.
static void *item_get_meta(void *impl)
{
    void *meta = nullptr;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void **, void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_ITEM_META])(
        &meta, impl);
#else
    ((void *(*)(void *, void **))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_GET_ITEM_META])(impl, &meta);
#endif
    return meta;
}

static void meta_destroy(void *meta)
{
    if (!meta || !slot_target(meta, ES_ITEM_META_SLOT_DELETE)) return;
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void *))VTABLE(meta)[ES_ITEM_META_SLOT_DELETE])(meta);
#else
    ((void (*)(void *, unsigned int))VTABLE(meta)[ES_ITEM_META_SLOT_DELETE])(
        meta, 1);
#endif
}

// Verifies that ItemMeta is MapMeta.
static bool meta_is_map(void *meta)
{
    return ((int (*)(void *))VTABLE(meta)[ES_ITEM_META_SLOT_GET_TYPE])(meta) ==
           ES_ITEM_META_TYPE_MAP;
}

static bool meta_has_map_id(void *meta)
{
    return ((bool (*)(void *))VTABLE(meta)[ES_MAP_META_SLOT_HAS_MAP_ID])(meta);
}

static int64_t meta_get_map_id(void *meta)
{
    return ((int64_t (*)(void *))VTABLE(meta)[ES_MAP_META_SLOT_GET_MAP_ID])(
        meta);
}

static void meta_set_map_view(void *meta, void *map_view)
{
    // Borrowed MapView; the callee reads MapView slot 1 getId internally.
    ((void (*)(void *, const void *))
        VTABLE(meta)[ES_MAP_META_SLOT_SET_MAP_VIEW])(meta, map_view);
}

static void meta_set_display_name(void *meta, const char *name)
{
    // The callee consumes the optional string.
    struct es_optional_string parameter = {0};
    cpp_string_construct(parameter.value, name);
    parameter.has_value = 1;
    STR_GUARD(parameter.value,
              ((void (*)(void *, void *))
                  VTABLE(meta)[ES_ITEM_META_SLOT_SET_DISPLAY_NAME])(
                  meta, &parameter));
}

// Returns whether an item carries the expected map ID.
static bool item_impl_has_map_id(void *impl, int64_t map_id)
{
    void *meta = item_get_meta(impl);
    if (!meta) return false;
    bool matched = meta_is_map(meta) && meta_has_map_id(meta) &&
                   meta_get_map_id(meta) == map_id;
    meta_destroy(meta);
    return matched;
}

static bool item_set_meta(void *impl, void *meta)
{
    // Borrows and copies the meta; the caller still owns and frees it.
    return ((bool (*)(void *, const void *))
        VTABLE(impl)[ES_ITEM_STACK_SLOT_SET_ITEM_META])(
        impl, meta);
}

// --- Inventory helpers ---

// Returns the player's borrowed inventory.
static void *player_inventory(void *player)
{
    if (!slot_target(player, ES_PLAYER_SLOT_GET_INVENTORY)) return nullptr;
    return ((void *(*)(void *))VTABLE(player)[ES_PLAYER_SLOT_GET_INVENTORY])(
        player);
}

static int inventory_size(void *inventory)
{
    return ((int (*)(void *))VTABLE(inventory)[ES_INVENTORY_SLOT_GET_SIZE])(
        inventory);
}

// Returns an optional owned item implementation.
static void inventory_get_item(void *inventory, int slot,
                               struct es_optional_item_stack *out)
{
    memset(out, 0, sizeof(*out));
#if defined(ES_PLATFORM_LINUX)
    ((void (*)(void *, void *, int))
        VTABLE(inventory)[ES_INVENTORY_SLOT_GET_ITEM])(
        out, inventory, slot);
#else
    ((void *(*)(void *, void *, int))
        VTABLE(inventory)[ES_INVENTORY_SLOT_GET_ITEM])(
        inventory, out, slot);
#endif
}

// The callee consumes the by-value optional: the impl moves into the
// inventory and must not be destroyed by the caller afterwards.
static void inventory_set_item(void *inventory, int slot,
                               struct es_optional_item_stack *parameter)
{
    ((void (*)(void *, int, void *))
        VTABLE(inventory)[ES_INVENTORY_SLOT_SET_ITEM])(
        inventory, slot, parameter);
}

static void inventory_clear_slot(void *inventory, int slot)
{
    // Slot 23 is clear(int); slot 22 would clear the entire inventory.
    ((void (*)(void *, int))VTABLE(inventory)[ES_INVENTORY_SLOT_CLEAR_SLOT])(
        inventory, slot);
}

// --- Public write API ---

bool mp_world_managed_frames_supported(void)
{
    return true;
}

enum mp_world_result mp_world_check_inventory_capacity(
    void *player, int required_slots, int *available_slots,
    char *detail, int detail_size)
{
    if (available_slots) *available_slots = 0;
    if (!player || required_slots < 0) return MP_WORLD_BAD_ARGUMENT;
    void *inventory = player_inventory(player);
    if (!inventory) {
        set_detail(detail, detail_size, "player inventory is unavailable");
        return MP_WORLD_BAD_ARGUMENT;
    }
    // Inspect owned item copies one slot at a time.
    int size = inventory_size(inventory);
    int available = 0;
    for (int slot = 0; slot < size; slot++) {
        struct es_optional_item_stack item;
        inventory_get_item(inventory, slot, &item);
        if (item.has_value) {
            if (item.impl) item_impl_destroy(item.impl);
        } else {
            available++;
        }
    }
    if (available_slots) *available_slots = available;
    if (available < required_slots) {
        if (detail && detail_size > 0) {
            snprintf(detail, (size_t)detail_size,
                     "inventory has %d empty slots; %d required",
                     available, required_slots);
        }
        return MP_WORLD_INVENTORY_FULL;
    }
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
    void *dimension = nullptr;
    if (!mp_world_c_player_world(player, expected_dimension, &dimension,
                                 detail, detail_size)) {
        return nullptr;
    }
    void *map_view = es_server_create_map(server, dimension);
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
    if (!out || !map_view) return MP_WORLD_BAD_ARGUMENT;
    *out = nullptr;
    if (!server) {
        set_detail(detail, detail_size, "Endstone Server is null");
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *dimension = nullptr;
    if (!mp_world_c_player_world(player, expected_dimension, &dimension,
                                 detail, detail_size)) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    enum mp_world_result validation = mp_world_validate_empty_tile(
        player, expected_dimension, cell, backing, facing,
        detail, detail_size);
    if (validation != MP_WORLD_OK) return validation;

    struct mp_world_prepared_tile *prepared = calloc(1, sizeof(*prepared));
    if (!prepared) {
        set_detail(detail, detail_size, "out of memory preparing world tile");
        return MP_WORLD_MAP_ITEM_FAILED;
    }
    prepared->dimension = dimension;
    prepared->cell = cell;
    prepared->backing = backing;
    prepared->facing = facing;
    prepared->map_id = map_id;
    prepared->delivered_slot = -1;

    // validate_empty already rejected any facing without a frame state.
    prepared->frame_data = create_frame_block_data(server,
                                                   facing_state(facing));
    if (!prepared->frame_data) {
        set_detail(detail, detail_size,
                   "unable to create item-frame block data");
        mp_world_prepared_destroy(prepared);
        return MP_WORLD_FRAME_PLACE_FAILED;
    }

    void *impl = create_filled_map_item(server, detail, detail_size);
    if (!impl) {
        mp_world_prepared_destroy(prepared);
        return MP_WORLD_MAP_ITEM_FAILED;
    }
    void *meta = item_get_meta(impl);
    if (!meta || !meta_is_map(meta)) {
        meta_destroy(meta);
        item_impl_destroy(impl);
        mp_world_prepared_destroy(prepared);
        return MP_WORLD_MAP_ITEM_FAILED;
    }
    // map_view originates only from public Server::createMap/getMap.
    meta_set_map_view(meta, map_view);
    char display_name[160];
    snprintf(display_name, sizeof(display_name),
             "MediaPlayer %s - tile %d/%d (row %d, col %d)",
             screen_name ? screen_name : "screen", tile_index + 1,
             tile_count, row + 1, column + 1);
    meta_set_display_name(meta, display_name);
    if (!meta_has_map_id(meta) || meta_get_map_id(meta) != map_id ||
        !item_set_meta(impl, meta)) {
        meta_destroy(meta);
        item_impl_destroy(impl);
        mp_world_prepared_destroy(prepared);
        return MP_WORLD_MAP_ID_MISMATCH;
    }
    meta_destroy(meta);
    prepared->map_item_impl = impl;
    *out = prepared;
    return MP_WORLD_OK;
}

enum mp_world_result mp_world_place_prepared(
    struct mp_world_prepared_tile *prepared, char *detail, int detail_size)
{
    if (!prepared || !prepared->dimension || !prepared->frame_data) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *block = block_at(prepared->dimension, prepared->cell);
    if (!block || !block_type_equals(block, "minecraft:air")) {
        block_destroy(block);
        set_detail(detail, detail_size, "screen cell changed before placement");
        return MP_WORLD_CELL_NOT_AIR;
    }
    block_set_data(block, prepared->frame_data, true);
    block_destroy(block);
    void *placed = block_at(prepared->dimension, prepared->cell);
    bool frame_present = placed &&
                         block_type_equals(placed, "minecraft:frame");
    block_destroy(placed);
    if (!frame_present) {
        set_detail(detail, detail_size,
                   "frame block was not present after setData");
        return MP_WORLD_FRAME_PLACE_FAILED;
    }
    return MP_WORLD_OK;
}

void mp_world_retract_prepared_maps(
    void *player, struct mp_world_prepared_tile *const *prepared, int count)
{
    if (!player || !prepared || count < 0) return;
    void *inventory = player_inventory(player);
    if (!inventory) return;
    for (int i = 0; i < count; i++) {
        struct mp_world_prepared_tile *tile = prepared[i];
        if (!tile || tile->delivered_slot < 0) continue;
        struct es_optional_item_stack current;
        inventory_get_item(inventory, tile->delivered_slot, &current);
        if (current.has_value && current.impl) {
            // Never clear an unverified slot during best-effort rollback.
            if (item_impl_has_map_id(current.impl, tile->map_id)) {
                inventory_clear_slot(inventory, tile->delivered_slot);
            }
            item_impl_destroy(current.impl);
        }
        tile->delivered_slot = -1;
    }
}

enum mp_world_result mp_world_deliver_prepared_maps(
    void *player, struct mp_world_prepared_tile *const *prepared, int count,
    char *detail, int detail_size)
{
    if (!player || !prepared || count < 0) return MP_WORLD_BAD_ARGUMENT;
    void *inventory = player_inventory(player);
    if (!inventory) {
        set_detail(detail, detail_size, "player inventory is unavailable");
        return MP_WORLD_BAD_ARGUMENT;
    }
    int *empty_slots = nullptr;
    if (count > 0) {
        empty_slots = calloc((size_t)count, sizeof(*empty_slots));
        if (!empty_slots) {
            set_detail(detail, detail_size,
                       "out of memory delivering map items");
            return MP_WORLD_MAP_DELIVERY_FAILED;
        }
    }
    int size = inventory_size(inventory);
    int empty_count = 0;
    for (int slot = 0; slot < size && empty_count < count; slot++) {
        struct es_optional_item_stack item;
        inventory_get_item(inventory, slot, &item);
        if (item.has_value) {
            if (item.impl) item_impl_destroy(item.impl);
            continue;
        }
        empty_slots[empty_count++] = slot;
    }
    if (empty_count < count) {
        free(empty_slots);
        set_detail(detail, detail_size,
                   "inventory capacity changed during screen creation");
        return MP_WORLD_INVENTORY_FULL;
    }
    for (int i = 0; i < count; i++) {
        if (!prepared[i] || !prepared[i]->map_item_impl) {
            free(empty_slots);
            mp_world_retract_prepared_maps(player, prepared, count);
            return MP_WORLD_MAP_DELIVERY_FAILED;
        }
        // setItem consumes the optional and its item implementation.
        struct es_optional_item_stack parameter = {
            .impl = prepared[i]->map_item_impl,
            .has_value = 1,
        };
        prepared[i]->map_item_impl = nullptr;
        inventory_set_item(inventory, empty_slots[i], &parameter);
        prepared[i]->delivered_slot = empty_slots[i];
        struct es_optional_item_stack delivered;
        inventory_get_item(inventory, empty_slots[i], &delivered);
        bool verified = delivered.has_value && delivered.impl &&
                        item_impl_has_map_id(delivered.impl,
                                             prepared[i]->map_id);
        if (delivered.has_value && delivered.impl) {
            item_impl_destroy(delivered.impl);
        }
        if (!verified) {
            free(empty_slots);
            mp_world_retract_prepared_maps(player, prepared, count);
            set_detail(detail, detail_size,
                       "map item verification failed after inventory delivery");
            return MP_WORLD_MAP_DELIVERY_FAILED;
        }
    }
    free(empty_slots);
    return MP_WORLD_OK;
}

void mp_world_prepared_destroy(struct mp_world_prepared_tile *prepared)
{
    if (!prepared) return;
    block_data_destroy(prepared->frame_data);
    item_impl_destroy(prepared->map_item_impl);
    free(prepared);
}

enum mp_world_result mp_world_remove_managed(
    void *server, void *player, const char *expected_dimension,
    struct screen_pos cell, int64_t expected_map_id,
    char *detail, int detail_size)
{
    // The installed map ID is not available for verification.
    (void)expected_map_id;
    if (!server) {
        set_detail(detail, detail_size, "Endstone Server is null");
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *dimension = nullptr;
    if (!mp_world_c_player_world(player, expected_dimension, &dimension,
                                 detail, detail_size)) {
        return MP_WORLD_BAD_ARGUMENT;
    }
    void *block = block_at(dimension, cell);
    if (!block) return MP_WORLD_BAD_ARGUMENT;
    if (block_type_equals(block, "minecraft:air")) {
        block_destroy(block);
        return MP_WORLD_OK;
    }
    if (!block_type_equals(block, "minecraft:frame")) {
        block_destroy(block);
        set_detail(detail, detail_size, "refusing to remove a non-frame block");
        return MP_WORLD_NOT_MANAGED_FRAME;
    }
    void *air = create_air_block_data(server);
    if (!air) {
        block_destroy(block);
        set_detail(detail, detail_size, "unable to create air block data");
        return MP_WORLD_BAD_ARGUMENT;
    }
    block_set_data(block, air, true);
    block_data_destroy(air);
    block_destroy(block);
    void *verify = block_at(dimension, cell);
    bool is_air = verify && block_type_equals(verify, "minecraft:air");
    block_destroy(verify);
    return is_air ? MP_WORLD_OK : MP_WORLD_FRAME_PLACE_FAILED;
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

// Managed writes require the Windows MSVC ABI.

static enum mp_world_result unsupported(char *detail, int detail_size)
{
    set_detail(detail, detail_size,
               "managed item-frame API is not verified on this platform");
    return MP_WORLD_UNSUPPORTED;
}

bool mp_world_managed_frames_supported(void)
{
    return false;
}

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
    (void)player;
    (void)prepared;
    (void)count;
    return unsupported(detail, detail_size);
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
    (void)server;
    (void)player;
    (void)expected_dimension;
    (void)cell;
}

#endif

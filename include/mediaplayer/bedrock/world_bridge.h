#ifndef ENDSTONE_MEDIAPLAYER_BEDROCK_WORLD_BRIDGE_H
#define ENDSTONE_MEDIAPLAYER_BEDROCK_WORLD_BRIDGE_H

#include "mediaplayer/screen/screen_geometry.h"
#include <stdbool.h>
#include <stdint.h>

enum mp_world_result {
    MP_WORLD_OK = 0,
    MP_WORLD_UNSUPPORTED,
    MP_WORLD_BAD_ARGUMENT,
    MP_WORLD_CELL_NOT_AIR,
    MP_WORLD_BACKING_NOT_SOLID,
    MP_WORLD_MAP_ITEM_FAILED,
    MP_WORLD_FRAME_PLACE_FAILED,
    MP_WORLD_MAP_ID_MISMATCH,
    MP_WORLD_NOT_MANAGED_FRAME,
    MP_WORLD_INVENTORY_FULL,
    MP_WORLD_MAP_DELIVERY_FAILED,
    MP_WORLD_MAP_ID_UNVERIFIABLE,
};

struct mp_world_tile_state {
    bool cell_is_air;
    bool backing_is_solid;
    bool frame_present;
    int64_t map_id;
    void *block_actor;
    void *block_actor_vptr;
};

struct mp_player_snapshot {
    float x;
    float y;
    float z;
    float pitch;
    float yaw;
    int block_x;
    int block_y;
    int block_z;
    char dimension_id[64];
};

struct mp_world_block_probe {
    bool block_found;
    bool is_air;
    bool support_candidate;
    char block_type[96];
    void *block;
    void *block_vptr;
    void *block_source;
    void *block_source_vptr;
};

struct mp_world_prepared_tile;

bool mp_player_get_snapshot(void *endstone_player,
                            struct mp_player_snapshot *snapshot,
                            char *detail, int detail_size);

// True when managed frame placement, delivery, and rollback are supported.
bool mp_world_managed_frames_supported(void);

enum mp_world_result mp_world_check_inventory_capacity(
    void *endstone_player, int required_slots, int *available_slots,
    char *detail, int detail_size);

enum mp_world_result mp_world_probe_block(
    void *endstone_player, const char *expected_dimension,
    struct screen_pos position, struct mp_world_block_probe *probe,
    char *detail, int detail_size);

// Creates a map through the borrowed EndstoneServer.
void *mp_world_create_map_for_player(void *endstone_server,
                                     void *endstone_player,
                                     const char *expected_dimension,
                                     char *detail, int detail_size);
void *mp_world_get_map(void *endstone_server, int64_t map_id);

enum mp_world_result mp_world_validate_empty_tile(
    void *endstone_player, const char *expected_dimension,
    struct screen_pos cell,
    struct screen_pos backing, enum screen_facing facing,
    char *detail, int detail_size);

enum mp_world_result mp_world_prepare_tile(
    void *endstone_server, void *endstone_player,
    const char *expected_dimension,
    struct screen_pos cell,
    struct screen_pos backing, enum screen_facing facing,
    void *map_view, int64_t map_id, const char *screen_name,
    int tile_index, int tile_count, int row, int column,
    struct mp_world_prepared_tile **out,
    char *detail, int detail_size);

enum mp_world_result mp_world_place_prepared(
    struct mp_world_prepared_tile *prepared,
    char *detail, int detail_size);

enum mp_world_result mp_world_deliver_prepared_maps(
    void *endstone_player, struct mp_world_prepared_tile *const *prepared,
    int count, char *detail, int detail_size);

void mp_world_retract_prepared_maps(
    void *endstone_player, struct mp_world_prepared_tile *const *prepared,
    int count);

void mp_world_prepared_destroy(struct mp_world_prepared_tile *prepared);

// Inspects a managed frame without assuming its map ID is readable.
enum mp_world_result mp_world_inspect_tile(
    void *endstone_player, const char *expected_dimension,
    struct screen_pos cell, struct screen_pos backing,
    int64_t expected_map_id, struct mp_world_tile_state *state,
    char *detail, int detail_size);

// Removes a managed frame from the cell.
enum mp_world_result mp_world_remove_managed(
    void *endstone_server, void *endstone_player,
    const char *expected_dimension,
    struct screen_pos cell, int64_t expected_map_id,
    char *detail, int detail_size);

// Rollback-only; the caller must prove it changed an air cell.
void mp_world_rollback_placed(void *endstone_server, void *endstone_player,
                              const char *expected_dimension,
                              struct screen_pos cell);

const char *mp_world_result_name(enum mp_world_result result);

#endif

#ifndef ENDSTONE_MEDIAPLAYER_BEDROCK_WORLD_READ_ABI_H
#define ENDSTONE_MEDIAPLAYER_BEDROCK_WORLD_READ_ABI_H

#include "endstone_abi.h"
#include "mediaplayer/bedrock/world_bridge.h"
#include <stdbool.h>
#include <stdint.h>

// Borrowed diagnostic pointers captured during one pure-C world call.
struct mp_world_c_trace {
    void *player;
    void *player_vptr;
    void *get_location_target;
    void *get_dimension_target;
    void *dimension;
    void *dimension_vptr;
    void *get_id_target;
    void *get_block_target;
    void *block_address;
    void *block_vptr;
    void *get_type_target;
    void *block_type;
    void *block_type_vptr;
    void *block_type_get_id_target;
    unsigned int handle_release_count;
};

bool mp_world_c_supported(void);

bool mp_world_c_debug_player_get_snapshot(
    void *endstone_player, struct mp_player_snapshot *snapshot,
    struct mp_world_c_trace *trace, char *detail, int detail_size);

enum mp_world_result mp_world_c_debug_probe_block(
    void *endstone_player, const char *expected_dimension,
    struct screen_pos position, struct mp_world_block_probe *probe,
    struct mp_world_c_trace *trace, char *detail, int detail_size);

// Pure policy helpers that perform no ABI calls.
bool mp_world_c_type_is_air(const char *block_type);
bool mp_world_c_type_is_support_candidate(const char *block_type);

// Resolves an owned Dimension handle for a verified player. The caller must
// release it with es_shared_release().
bool mp_world_c_player_world(void *endstone_player,
                             const char *expected_dimension,
                             struct es_shared_handle *dimension_out,
                             char *detail, int detail_size);

#endif

#ifndef ENDSTONE_MEDIAPLAYER_BEDROCK_WORLD_WRITE_ABI_H
#define ENDSTONE_MEDIAPLAYER_BEDROCK_WORLD_WRITE_ABI_H

#include "endstone_abi.h"

#include <stddef.h>
#include <stdint.h>

struct es_identifier {
    const char *ns;
    size_t ns_len;
    const char *key;
    size_t key_len;
};

_Static_assert(sizeof(struct es_identifier) == 32,
               "measured Identifier size mismatch");

#define ES_ITEM_META_TYPE_MAP 3
#define ES_BLOCK_STATE_WHICH_BOOL 0
#define ES_BLOCK_STATE_WHICH_STRING 1
#define ES_BLOCK_STATE_WHICH_INT 2

struct es_optional_string {
    _Alignas(8) unsigned char value[ES_STRING_SIZE];
    uint8_t has_value;
    uint8_t padding[
        ES_OPTIONAL_STRING_SIZE - ES_STRING_SIZE - sizeof(uint8_t)];
};

_Static_assert(sizeof(struct es_optional_string) == ES_OPTIONAL_STRING_SIZE,
               "measured optional<string> size mismatch");
_Static_assert(offsetof(struct es_optional_string, has_value) ==
                   ES_OPTIONAL_STRING_OFF_HAS_VALUE,
               "measured optional<string> engaged flag offset mismatch");

struct es_optional_item_stack {
    void *impl;
    uint8_t has_value;
    uint8_t padding[
        ES_OPTIONAL_ITEM_STACK_SIZE - sizeof(void *) - sizeof(uint8_t)];
};

_Static_assert(sizeof(struct es_optional_item_stack) ==
                   ES_OPTIONAL_ITEM_STACK_SIZE,
               "measured optional<ItemStack> size mismatch");
_Static_assert(offsetof(struct es_optional_item_stack, has_value) ==
                   ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE,
               "measured optional<ItemStack> engaged flag offset mismatch");

#if defined(ES_PLATFORM_WINDOWS)

struct es_block_states {
    float max_load_factor;
    uint32_t padding0;
    void *head;
    size_t size;
    void *vec_first;
    void *vec_last;
    void *vec_end;
    size_t mask;
    size_t maxidx;
};

_Static_assert(sizeof(struct es_block_states) == ES_BLOCK_STATES_SIZE,
               "measured BlockStates size mismatch");
_Static_assert(offsetof(struct es_block_states, head) ==
                   ES_BLOCK_STATES_OFF_HEAD &&
               offsetof(struct es_block_states, size) ==
                   ES_BLOCK_STATES_OFF_SIZE &&
               offsetof(struct es_block_states, vec_first) ==
                   ES_BLOCK_STATES_OFF_VECTOR &&
               offsetof(struct es_block_states, mask) ==
                   ES_BLOCK_STATES_OFF_MASK &&
               offsetof(struct es_block_states, maxidx) ==
                   ES_BLOCK_STATES_OFF_MAX_INDEX,
               "measured BlockStates field offsets mismatch");

struct es_block_state_node {
    struct es_block_state_node *next;
    struct es_block_state_node *prev;
    unsigned char key[32];
    unsigned char variant_storage[32];
    uint8_t variant_index;
    uint8_t padding[7];
};

_Static_assert(sizeof(struct es_block_state_node) == ES_BLOCK_STATE_NODE_SIZE,
               "measured BlockStates node size mismatch");
_Static_assert(offsetof(struct es_block_state_node, key) ==
                   ES_BLOCK_STATE_NODE_OFF_KEY &&
               offsetof(struct es_block_state_node, variant_storage) ==
                   ES_BLOCK_STATE_NODE_OFF_VARIANT &&
               offsetof(struct es_block_state_node, variant_index) ==
                   ES_BLOCK_STATE_NODE_OFF_VARIANT_INDEX,
               "measured BlockStates node offsets mismatch");

#else

struct es_block_state_node {
    struct es_block_state_node *next;
    size_t hash;
    unsigned char key[ES_STRING_SIZE];
    unsigned char variant_storage[24];
    uint8_t variant_index;
    uint8_t padding[7];
};

_Static_assert(sizeof(struct es_block_state_node) == ES_BLOCK_STATE_NODE_SIZE,
               "measured BlockStates node size mismatch");
_Static_assert(offsetof(struct es_block_state_node, next) ==
                   ES_BLOCK_STATE_NODE_OFF_NEXT &&
               offsetof(struct es_block_state_node, hash) ==
                   ES_BLOCK_STATE_NODE_OFF_HASH &&
               offsetof(struct es_block_state_node, key) ==
                   ES_BLOCK_STATE_NODE_OFF_KEY &&
               offsetof(struct es_block_state_node, variant_storage) ==
                   ES_BLOCK_STATE_NODE_OFF_VARIANT &&
               offsetof(struct es_block_state_node, variant_index) ==
                   ES_BLOCK_STATE_NODE_OFF_VARIANT_INDEX,
               "measured BlockStates node offsets mismatch");

struct es_block_states {
    void **buckets;
    size_t bucket_count;
    struct es_block_state_node *first_node;
    size_t size;
    float max_load_factor;
    uint32_t padding;
};

_Static_assert(sizeof(struct es_block_states) == ES_BLOCK_STATES_SIZE,
               "measured BlockStates size mismatch");
_Static_assert(offsetof(struct es_block_states, buckets) ==
                   ES_BLOCK_STATES_OFF_BUCKETS &&
               offsetof(struct es_block_states, bucket_count) ==
                   ES_BLOCK_STATES_OFF_BUCKET_COUNT &&
               offsetof(struct es_block_states, first_node) ==
                   ES_BLOCK_STATES_OFF_FIRST_NODE &&
               offsetof(struct es_block_states, size) ==
                   ES_BLOCK_STATES_OFF_SIZE &&
               offsetof(struct es_block_states, max_load_factor) ==
                   ES_BLOCK_STATES_OFF_MAX_LOAD_FACTOR,
               "measured BlockStates field offsets mismatch");

#endif

#endif

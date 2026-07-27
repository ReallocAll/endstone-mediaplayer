#ifndef ENDSTONE_MEDIAPLAYER_BEDROCK_MAP_ABI_H
#define ENDSTONE_MEDIAPLAYER_BEDROCK_MAP_ABI_H

#include "endstone_abi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Platform-neutral read-only BlockActor diagnostics.
struct es_block_actor_probe {
    bool block_found;
    bool block_actor_found;
    void *block_actor;
    void *block_actor_vptr;
    void *image_base;
    uintptr_t block_actor_vptr_rva;
};

#if defined(ES_PLATFORM_WINDOWS)

struct es_msvc_shared_ptr {
    void *ptr;
    void *control;
};

_Static_assert(sizeof(struct es_msvc_shared_ptr) == ES_SHARED_PTR_SIZE,
               "MSVC shared_ptr ABI size mismatch");

#endif

bool es_map_abi_supported(void);
void *es_server_create_map(void *server, void *dimension);
void *es_server_get_map(void *server, int64_t map_id);
int64_t es_map_view_get_id(void *map_view);
void es_map_view_set_locked(void *map_view, bool locked);
void es_map_view_add_renderer(void *map_view, void *shared_ptr_storage);
bool es_map_view_remove_renderer(void *map_view, const void *shared_ptr_storage);
void es_player_send_map(void *player, void *map_view);
bool es_player_is_op(void *player);
void *es_player_send_map_target(void *player);
bool es_player_uuid_string(void *player, char output[37]);
bool es_probe_block_actor(void *dimension, int x, int y, int z,
                          struct es_block_actor_probe *probe);

#if defined(ES_PLATFORM_WINDOWS)
void es_msvc_shared_ptr_add_ref(struct es_msvc_shared_ptr *shared);
void es_msvc_shared_ptr_release(struct es_msvc_shared_ptr *shared);
#else
struct es_libcxx_shared_ptr {
    void *ptr;
    void *control;
};

_Static_assert(sizeof(struct es_libcxx_shared_ptr) == ES_SHARED_PTR_SIZE,
               "libc++ shared_ptr ABI size mismatch");

void es_libcxx_shared_ptr_add_ref(struct es_libcxx_shared_ptr *shared);
void es_libcxx_shared_ptr_release(struct es_libcxx_shared_ptr *shared);
#endif

#endif

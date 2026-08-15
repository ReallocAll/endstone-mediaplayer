#include "mediaplayer/bedrock/map_abi.h"
#include "abi_helpers.h"
#include <string.h>

#if defined(ES_PLATFORM_WINDOWS)
#include <windows.h>

bool es_map_abi_supported(void)
{
    return true;
}

void *es_server_create_map(void *server, void *dimension)
{
    return ((void *(*)(void *, void *))
        VTABLE(server)[ES_SERVER_SLOT_CREATE_MAP])(server, dimension);
}

void *es_server_get_map(void *server, int64_t map_id)
{
    return ((void *(*)(void *, int64_t))
        VTABLE(server)[ES_SERVER_SLOT_GET_MAP])(server, map_id);
}

int64_t es_map_view_get_id(void *map_view)
{
    return ((int64_t (*)(void *))VTABLE(map_view)[ES_MAPVIEW_SLOT_GET_ID])(
        map_view);
}

void es_map_view_set_locked(void *map_view, bool locked)
{
    ((void (*)(void *, bool))VTABLE(map_view)[ES_MAPVIEW_SLOT_SET_LOCKED])(
        map_view, locked);
}

void es_map_view_add_renderer(void *map_view, void *shared_ptr_storage)
{
    // addRenderer(std::shared_ptr<MapRenderer>) receives a pointer to the
    // 16-byte by-value parameter in RDX. The callee destroys that parameter.
    ((void (*)(void *, void *))VTABLE(map_view)[ES_MAPVIEW_SLOT_ADD_RENDERER])(
        map_view, shared_ptr_storage);
}

bool es_map_view_remove_renderer(void *map_view, const void *shared_ptr_storage)
{
    // removeRenderer(const std::shared_ptr<MapRenderer>&) receives the address
    // of caller-owned storage and does not consume that ownership.
    return ((bool (*)(void *, const void *))
        VTABLE(map_view)[ES_MAPVIEW_SLOT_REMOVE_RENDERER])(
        map_view, shared_ptr_storage);
}

void es_player_send_map(void *player, void *map_view)
{
    // EndstonePlayer::sendMap(MapView&): RCX=this, RDX=&map, void return.
    ((void (*)(void *, void *))VTABLE(player)[ES_PLAYER_SLOT_SEND_MAP])(
        player, map_view);
}

bool es_player_is_op(void *player)
{
    return ((bool (*)(void *))VTABLE(player)[ES_PLAYER_SLOT_IS_OP])(player);
}

void *es_player_send_map_target(void *player)
{
    return player ? VTABLE(player)[ES_PLAYER_SLOT_SEND_MAP] : nullptr;
}

bool es_player_uuid_string(void *player, char output[37])
{
    if (!player || !output) {
        return false;
    }
    void *offline_player = (char *)player +
                           ES_ENDSTONE_PLAYER_OFF_OFFLINE_PLAYER;
    unsigned char uuid[ES_UUID_SIZE] = {0};
    ((void *(*)(void *, void *))
        VTABLE(offline_player)[ES_OFFLINE_PLAYER_SLOT_GET_UNIQUE_ID])(
        offline_player, uuid);

    static const char digits[] = "0123456789abcdef";
    int destination = 0;
    for (int source = 0; source < ES_UUID_SIZE; source++) {
        output[destination++] = digits[uuid[source] >> 4];
        output[destination++] = digits[uuid[source] & 0x0f];
        if (source == 3 || source == 5 || source == 7 || source == 9) {
            output[destination++] = '-';
        }
    }
    output[destination] = '\0';
    return true;
}

bool es_probe_block_actor(void *dimension, int x, int y, int z,
                          struct es_block_actor_probe *probe)
{
    if (!dimension || !probe) {
        return false;
    }
    memset(probe, 0, sizeof(*probe));

    void *block = nullptr;
    ((void *(*)(void *, void **, int, int, int))
        VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
        dimension, &block, x, y, z);
    if (!block) {
        return false;
    }

    probe->block_found = true;
    void *block_source = *(void **)((char *)block +
                                    ES_ENDSTONE_BLOCK_OFF_BLOCK_SOURCE);
    if (block_source) {
        int block_pos[3] = {x, y, z};
        void *actor = ((void *(*)(void *, const int *))
            VTABLE(block_source)[ES_BLOCK_SOURCE_SLOT_GET_BLOCK_ENTITY])(
                block_source, block_pos);
        probe->block_actor = actor;
        if (actor) {
            probe->block_actor_found = true;
            probe->block_actor_vptr = *(void **)actor;
            probe->image_base = GetModuleHandleW(nullptr);
            if ((uintptr_t)probe->block_actor_vptr >=
                (uintptr_t)probe->image_base) {
                probe->block_actor_vptr_rva =
                    (uintptr_t)probe->block_actor_vptr -
                    (uintptr_t)probe->image_base;
            }
        }
    }

    // Destroy the temporary unique_ptr<Block> exactly as MSVC delete would.
    ((void (*)(void *, unsigned int))VTABLE(block)[0])(block, 1);
    return probe->block_actor_found;
}

void es_msvc_shared_ptr_add_ref(struct es_msvc_shared_ptr *shared)
{
    if (shared && shared->control) {
        InterlockedIncrement(
            (volatile LONG *)((char *)shared->control + ES_REFCOUNT_OFF_USES));
    }
}

void es_msvc_shared_ptr_release(struct es_msvc_shared_ptr *shared)
{
    if (!shared || !shared->control) {
        return;
    }

    void *control = shared->control;
    if (InterlockedDecrement(
            (volatile LONG *)((char *)control + ES_REFCOUNT_OFF_USES)) == 0) {
        void **vtable = *(void ***)control;
        ((void (*)(void *))vtable[ES_REFCOUNT_SLOT_DESTROY_RESOURCE])(
            control);
        if (InterlockedDecrement(
                (volatile LONG *)((char *)control + ES_REFCOUNT_OFF_WEAKS)) == 0) {
            ((void (*)(void *))vtable[ES_REFCOUNT_SLOT_DELETE_THIS])(
                control);
        }
    }

    shared->ptr = nullptr;
    shared->control = nullptr;
}

#else

bool es_map_abi_supported(void)
{
    return true;
}

void *es_server_create_map(void *server, void *dimension)
{
    return ((void *(*)(void *, void *))
        VTABLE(server)[ES_SERVER_SLOT_CREATE_MAP])(
        server, dimension);
}

void *es_server_get_map(void *server, int64_t map_id)
{
    return ((void *(*)(void *, int64_t))
        VTABLE(server)[ES_SERVER_SLOT_GET_MAP])(server, map_id);
}

int64_t es_map_view_get_id(void *map_view)
{
    return ((int64_t (*)(void *))VTABLE(map_view)[ES_MAPVIEW_SLOT_GET_ID])(
        map_view);
}

void es_map_view_set_locked(void *map_view, bool locked)
{
    ((void (*)(void *, bool))VTABLE(map_view)[ES_MAPVIEW_SLOT_SET_LOCKED])(
        map_view, locked);
}

void es_map_view_add_renderer(void *map_view, void *shared_ptr_storage)
{
    ((void (*)(void *, void *))VTABLE(map_view)[ES_MAPVIEW_SLOT_ADD_RENDERER])(
        map_view, shared_ptr_storage);
}

bool es_map_view_remove_renderer(void *map_view, const void *shared_ptr_storage)
{
    return ((bool (*)(void *, const void *))
        VTABLE(map_view)[ES_MAPVIEW_SLOT_REMOVE_RENDERER])(
        map_view, shared_ptr_storage);
}

void es_player_send_map(void *player, void *map_view)
{
    ((void (*)(void *, void *))VTABLE(player)[ES_PLAYER_SLOT_SEND_MAP])(
        player, map_view);
}

bool es_player_is_op(void *player)
{
    return ((bool (*)(void *))VTABLE(player)[ES_PLAYER_SLOT_IS_OP])(player);
}

void *es_player_send_map_target(void *player)
{
    return player ? VTABLE(player)[ES_PLAYER_SLOT_SEND_MAP] : nullptr;
}

bool es_player_uuid_string(void *player, char output[37])
{
    if (!player || !output) {
        return false;
    }
    void *offline_player = (char *)player +
                           ES_ENDSTONE_PLAYER_OFF_OFFLINE_PLAYER;
    struct es_uuid_value {
        unsigned char bytes[ES_UUID_SIZE];
    };
    struct es_uuid_value uuid =
        ((struct es_uuid_value (*)(void *))VTABLE(offline_player)[
            ES_OFFLINE_PLAYER_SLOT_GET_UNIQUE_ID])(offline_player);

    static const char digits[] = "0123456789abcdef";
    int destination = 0;
    for (int source = 0; source < ES_UUID_SIZE; source++) {
        output[destination++] = digits[uuid.bytes[source] >> 4];
        output[destination++] = digits[uuid.bytes[source] & 0x0f];
        if (source == 3 || source == 5 || source == 7 || source == 9) {
            output[destination++] = '-';
        }
    }
    output[destination] = '\0';
    return true;
}

bool es_probe_block_actor(void *dimension, int x, int y, int z,
                          struct es_block_actor_probe *probe)
{
    if (!dimension || !probe) {
        return false;
    }
    memset(probe, 0, sizeof(*probe));
    void *block = nullptr;
    ((void (*)(void **, void *, int, int, int))
        VTABLE(dimension)[ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ])(
        &block, dimension, x, y, z);
    if (!block) {
        return false;
    }
    probe->block_found = true;
    ((void (*)(void *))VTABLE(block)[ES_BLOCK_SLOT_DELETE])(block);
    return false;
}

void es_libcxx_shared_ptr_add_ref(struct es_libcxx_shared_ptr *shared)
{
    if (shared && shared->control) {
        __atomic_fetch_add(
            (long *)((char *)shared->control + ES_REFCOUNT_OFF_USES),
            1, __ATOMIC_RELAXED);
    }
}

void es_libcxx_shared_ptr_release(struct es_libcxx_shared_ptr *shared)
{
    if (!shared || !shared->control) {
        return;
    }
    void *control = shared->control;
    long previous = __atomic_fetch_sub(
        (long *)((char *)control + ES_REFCOUNT_OFF_USES),
        1, __ATOMIC_ACQ_REL);
    if (previous == 0) {
        void **vtable = *(void ***)control;
        ((void (*)(void *))
            vtable[ES_REFCOUNT_SLOT_DESTROY_RESOURCE])(control);
        previous = __atomic_fetch_sub(
            (long *)((char *)control + ES_REFCOUNT_OFF_WEAKS),
            1, __ATOMIC_ACQ_REL);
        if (previous == 0) {
            ((void (*)(void *))
                vtable[ES_REFCOUNT_SLOT_DELETE_THIS])(control);
        }
    }
    shared->ptr = nullptr;
    shared->control = nullptr;
}

#endif

#include "mediaplayer/bedrock/map_abi.h"

#include "abi_helpers.h"

#include <string.h>

struct es_uuid_value {
    _Alignas(ES_UUID_ALIGN) unsigned char bytes[ES_UUID_SIZE];
};

static_assert(sizeof(struct es_uuid_value) == ES_UUID_SIZE);
static_assert(_Alignof(struct es_uuid_value) == ES_UUID_ALIGN);
static_assert(ES_UUID_OFF_DATA + 16 <= ES_UUID_SIZE);

bool es_map_abi_supported(void)
{
    return true;
}

void *es_server_create_map(void *server,
                           const struct es_shared_handle *dimension)
{
    if (!server || !dimension || !es_shared_object(dimension) ||
        !VTABLE(server)[ES_SERVER_SLOT_CREATE_MAP]) {
        return nullptr;
    }
    return ((void *(*)(void *, const void *))
        VTABLE(server)[ES_SERVER_SLOT_CREATE_MAP])(server, dimension);
}

void *es_server_get_map(void *server, int64_t map_id)
{
    if (!server || !VTABLE(server)[ES_SERVER_SLOT_GET_MAP]) return nullptr;
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

void es_map_view_add_renderer(void *map_view, void *handle_storage)
{
    ((void (*)(void *, void *))VTABLE(map_view)[ES_MAPVIEW_SLOT_ADD_RENDERER])(
        map_view, handle_storage);
}

bool es_map_view_remove_renderer(void *map_view, const void *handle_storage)
{
    return ((bool (*)(void *, const void *))
        VTABLE(map_view)[ES_MAPVIEW_SLOT_REMOVE_RENDERER])(
            map_view, handle_storage);
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
    if (!player || !output || !VTABLE(player)[ES_PLAYER_SLOT_GET_UNIQUE_ID]) {
        return false;
    }
    struct es_uuid_value uuid = {0};
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))VTABLE(player)[ES_PLAYER_SLOT_GET_UNIQUE_ID])(
        player, &uuid);
#else
    uuid = ((struct es_uuid_value (*)(void *))
        VTABLE(player)[ES_PLAYER_SLOT_GET_UNIQUE_ID])(player);
#endif
    const unsigned char *data = uuid.bytes + ES_UUID_OFF_DATA;
    static const char digits[] = "0123456789abcdef";
    int destination = 0;
    for (int source = 0; source < 16; source++) {
        output[destination++] = digits[data[source] >> 4];
        output[destination++] = digits[data[source] & 0x0f];
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
    (void)dimension;
    (void)x;
    (void)y;
    (void)z;
    if (probe) memset(probe, 0, sizeof(*probe));
    return false;
}

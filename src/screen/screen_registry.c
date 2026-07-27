#include "mediaplayer/screen/screen_registry.h"
#include <string.h>

void screen_registry_init(struct screen_registry *reg)
{
    memset(reg, 0, sizeof(*reg));
    reg->next_runtime_id = 1;
}

const char *screen_error_name(enum screen_error error)
{
    switch (error) {
    case SCREEN_OK: return "ok";
    case SCREEN_ERR_NOT_FOUND: return "screen not found";
    case SCREEN_ERR_NAME_EXISTS: return "screen name already exists";
    case SCREEN_ERR_NAME_INVALID: return "invalid screen name";
    case SCREEN_ERR_FULL: return "screen registry is full";
    }
    return "unknown screen error";
}

int screen_name_valid(const char *name)
{
    if (!name || !name[0])
        return 0;
    size_t len = strlen(name);
    if (len > SCREEN_NAME_MAX - 1)
        return 0;
    if (name[0] == '.')
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '_' || c == '-') continue;
        if ((unsigned char)c >= 0x80) continue;
        return 0;
    }
    return 1;
}

int screen_registry_find(const struct screen_registry *reg, const char *name)
{
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->screens[i].name, name) == 0)
            return i;
    }
    return -1;
}

enum screen_error screen_registry_create(
    struct screen_registry *reg,
    const char *name,
    const char *owner_uuid,
    const struct screen_geom *geom,
    int *out_index)
{
    if (!screen_name_valid(name))
        return SCREEN_ERR_NAME_INVALID;

    if (screen_registry_find(reg, name) >= 0)
        return SCREEN_ERR_NAME_EXISTS;

    if (reg->count >= SCREEN_REGISTRY_MAX)
        return SCREEN_ERR_FULL;

    int idx = reg->count;
    struct screen_entry *e = &reg->screens[idx];
    memset(e, 0, sizeof(*e));

    size_t nlen = strlen(name);
    if (nlen >= SCREEN_NAME_MAX)
        nlen = SCREEN_NAME_MAX - 1;
    memcpy(e->name, name, nlen);
    e->name[nlen] = '\0';

    if (owner_uuid) {
        size_t ulen = strlen(owner_uuid);
        if (ulen > SCREEN_UUID_LEN)
            ulen = SCREEN_UUID_LEN;
        memcpy(e->owner_uuid, owner_uuid, ulen);
        e->owner_uuid[ulen] = '\0';
    }

    e->geom = *geom;
    e->runtime_id = reg->next_runtime_id++;
    if (reg->next_runtime_id == 0)
        reg->next_runtime_id = 1;
    e->plugin_managed = 0;
    e->tiles_initialized = 0;
    e->playing = 0;
    for (int i = 0; i < SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT; i++)
        e->tiles[i].map_id = -1;

    reg->count++;
    if (out_index)
        *out_index = idx;
    return SCREEN_OK;
}

enum screen_error screen_registry_delete(struct screen_registry *reg, const char *name)
{
    int idx = screen_registry_find(reg, name);
    if (idx < 0)
        return SCREEN_ERR_NOT_FOUND;

    for (int i = idx; i < reg->count - 1; i++)
        reg->screens[i] = reg->screens[i + 1];

    reg->count--;
    memset(&reg->screens[reg->count], 0, sizeof(struct screen_entry));
    return SCREEN_OK;
}

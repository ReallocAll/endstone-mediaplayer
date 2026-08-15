#include "mediaplayer/screen/screen_registry.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void screen_registry_init(struct screen_registry *reg)
{
    memset(reg, 0, sizeof(*reg));
    reg->next_runtime_id = 1;
}

void screen_entry_cleanup_tiles(struct screen_entry *entry)
{
    if (!entry)
        return;
    if (entry->tiles) {
        free(entry->tiles);
    }
    entry->tiles = nullptr;
    entry->tiles_capacity = 0;
    entry->tiles_initialized = 0;
}

void screen_entry_cleanup(struct screen_entry *entry)
{
    if (!entry)
        return;
    surface_destroy(entry->surface);
    entry->surface = nullptr;
    screen_entry_cleanup_tiles(entry);
    free(entry);
}

void screen_registry_cleanup(struct screen_registry *reg)
{
    if (!reg)
        return;
    for (int i = 0; i < reg->count; i++)
        screen_entry_cleanup(reg->screens[i]);
    memset(reg->screens, 0, sizeof(reg->screens));
    reg->count = 0;
}

enum screen_error screen_entry_materialize_tiles(struct screen_entry *entry)
{
    if (!entry)
        return SCREEN_ERR_DIMENSION_INVALID;

    enum screen_geom_err dimension_error = screen_geom_validate_dimensions(
        entry->geom.width, entry->geom.height);
    if (dimension_error != SCREEN_GEOM_OK)
        return SCREEN_ERR_DIMENSION_INVALID;

    int tile_count = screen_geom_tile_count(&entry->geom);
    if (tile_count <= 0 || (size_t)tile_count > SIZE_MAX / sizeof(*entry->tiles))
        return SCREEN_ERR_DIMENSION_INVALID;
    if (tile_count > SCREEN_MATERIALIZED_MAX_TILES)
        return SCREEN_ERR_MATERIALIZATION_LIMIT;
    if (entry->tiles && entry->tiles_capacity >= (size_t)tile_count)
        return SCREEN_OK;

    struct screen_tile_rt *tiles = calloc((size_t)tile_count,
                                          sizeof(*tiles));
    if (!tiles)
        return SCREEN_ERR_NO_MEMORY;
    for (int i = 0; i < tile_count; i++)
        tiles[i].map_id = -1;

    // An entry is not expected to be resized, but keep this helper safe if a
    // caller materializes a larger geometry after prior compatibility setup.
    if (entry->tiles) {
        size_t copy_count = entry->tiles_capacity < (size_t)tile_count
                                ? entry->tiles_capacity
                                : (size_t)tile_count;
        memcpy(tiles, entry->tiles, copy_count * sizeof(*tiles));
        free(entry->tiles);
    }
    entry->tiles = tiles;
    entry->tiles_capacity = (size_t)tile_count;
    return SCREEN_OK;
}

const char *screen_error_name(enum screen_error error)
{
    switch (error) {
    case SCREEN_OK: return "ok";
    case SCREEN_ERR_NOT_FOUND: return "screen not found";
    case SCREEN_ERR_NAME_EXISTS: return "screen name already exists";
    case SCREEN_ERR_NAME_INVALID: return "invalid screen name";
    case SCREEN_ERR_FULL: return "screen registry is full";
    case SCREEN_ERR_DIMENSION_INVALID: return "invalid screen dimensions";
    case SCREEN_ERR_MATERIALIZATION_LIMIT: return "screen materialization limit exceeded";
    case SCREEN_ERR_NO_MEMORY: return "out of memory";
    case SCREEN_ERR_RUNTIME_ID_EXHAUSTED: return "runtime identity exhausted";
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
        if (strcmp(reg->screens[i]->name, name) == 0)
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

    if (!geom || screen_geom_validate_dimensions(geom->width, geom->height) !=
                     SCREEN_GEOM_OK)
        return SCREEN_ERR_DIMENSION_INVALID;
    if (reg->next_runtime_id == 0 || reg->next_runtime_id == UINT64_MAX)
        return SCREEN_ERR_RUNTIME_ID_EXHAUSTED;

    int idx = reg->count;
    struct screen_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return SCREEN_ERR_NO_MEMORY;

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
    size_t tile_count = (size_t)screen_geom_tile_count(geom);
    size_t resident_limit = tile_count < SURFACE_MAX_RESIDENT_TILES
                                ? tile_count
                                : SURFACE_MAX_RESIDENT_TILES;
    size_t pending_limit = resident_limit < 256 ? resident_limit : 256;
    if (surface_create((uint32_t)geom->width, (uint32_t)geom->height,
                       resident_limit, pending_limit, true,
                       &e->surface) != SURFACE_OK) {
        free(e);
        return SCREEN_ERR_NO_MEMORY;
    }
    e->runtime_id = reg->next_runtime_id++;
    e->plugin_managed = 0;
    e->tiles_initialized = 0;
    e->playing = 0;

    reg->screens[idx] = e;
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

    screen_entry_cleanup(reg->screens[idx]);
    for (int i = idx; i < reg->count - 1; i++)
        reg->screens[i] = reg->screens[i + 1];

    reg->count--;
    reg->screens[reg->count] = nullptr;
    return SCREEN_OK;
}

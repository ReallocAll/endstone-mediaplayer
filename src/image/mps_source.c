#include "mediaplayer/image/mps_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void source_clear(struct mps_source *source)
{
    if (!source)
        return;
    if (source->active)
        mps_close(&source->file);
    free(source->tile_cache);
    memset(source, 0, sizeof(*source));
}

void mps_source_engine_init(struct mps_source_engine *engine)
{
    if (!engine)
        return;
    memset(engine, 0, sizeof(*engine));
    engine->next_attachment_id = 1;
}

void mps_source_engine_shutdown(struct mps_source_engine *engine)
{
    if (!engine)
        return;
    for (int i = 0; i < MPS_SOURCE_MAX; i++)
        source_clear(&engine->sources[i]);
}

struct mps_source *mps_source_find(struct mps_source_engine *engine,
                                   uint64_t screen_runtime_id)
{
    if (!engine || screen_runtime_id == 0)
        return nullptr;
    for (int i = 0; i < MPS_SOURCE_MAX; i++) {
        if (engine->sources[i].active &&
            engine->sources[i].screen_runtime_id == screen_runtime_id)
            return &engine->sources[i];
    }
    return nullptr;
}

enum mps_source_error mps_source_start(
    struct mps_source_engine *engine, uint64_t screen_runtime_id,
    const char *path, const char *name, int expected_width,
    int expected_height, struct mps_source **out)
{
    if (out)
        *out = nullptr;
    if (!engine || screen_runtime_id == 0 || !path || !path[0] ||
        expected_width <= 0 || expected_height <= 0)
        return MPS_SOURCE_ERR_ARGUMENT;

    struct mps_source *slot = mps_source_find(engine, screen_runtime_id);
    if (!slot) {
        for (int i = 0; i < MPS_SOURCE_MAX; i++) {
            if (!engine->sources[i].active) {
                slot = &engine->sources[i];
                break;
            }
        }
    }
    if (!slot)
        return MPS_SOURCE_ERR_ALLOC;
    source_clear(slot);

    enum mps_error error = mps_open(path, &slot->file);
    if (error != MPS_OK)
        return MPS_SOURCE_ERR_OPEN;
    if (slot->file.header.tile_width != (uint32_t)expected_width ||
        slot->file.header.tile_height != (uint32_t)expected_height) {
        mps_close(&slot->file);
        return MPS_SOURCE_ERR_DIMENSION;
    }

    uint64_t attachment_id = engine->next_attachment_id++;
    if (attachment_id == 0)
        attachment_id = engine->next_attachment_id++;
    slot->active = true;
    slot->screen_runtime_id = screen_runtime_id;
    slot->attachment_id = attachment_id;
    slot->initial_cursor = 0;
    slot->cached_tile_index = 0;
    slot->cache_valid = false;
    if (name)
        snprintf(slot->image_name, sizeof(slot->image_name), "%s", name);
    if (out)
        *out = slot;
    return MPS_SOURCE_OK;
}

void mps_source_release(struct mps_source_engine *engine,
                        uint64_t screen_runtime_id)
{
    struct mps_source *source = mps_source_find(engine, screen_runtime_id);
    if (source)
        source_clear(source);
}

enum mps_source_error mps_source_read(
    struct mps_source *source, uint64_t tile_index,
    struct presenter_stream_tile *out)
{
    if (!source || !source->active || !out ||
        tile_index >= source->file.header.tile_count)
        return MPS_SOURCE_ERR_ARGUMENT;
    if (!source->cache_valid || source->cached_tile_index != tile_index) {
        if (!source->tile_cache) {
            source->tile_cache = malloc(MPS_TILE_BYTES);
            if (!source->tile_cache)
                return MPS_SOURCE_ERR_ALLOC;
        }
        if (mps_read_tile(&source->file, tile_index, source->tile_cache,
                          MPS_TILE_BYTES) != MPS_OK) {
            source->cache_valid = false;
            return MPS_SOURCE_ERR_READ;
        }
        source->tile_read_count++;
        source->cached_tile_index = tile_index;
        source->cache_valid = true;
    }
    out->pixels = source->tile_cache;
    out->bytes = MPS_TILE_BYTES;
    out->generation = tile_index + 1;
    out->content_hash = 0;
    return MPS_SOURCE_OK;
}

bool mps_source_presenter_read(
    void *context, uint64_t tile_index, uint32_t tile_x, uint32_t tile_y,
    struct presenter_stream_tile *out)
{
    (void)tile_x;
    (void)tile_y;
    return mps_source_read(context, tile_index, out) == MPS_SOURCE_OK;
}

const char *mps_source_error_name(enum mps_source_error error)
{
    switch (error) {
    case MPS_SOURCE_OK: return "ok";
    case MPS_SOURCE_ERR_ARGUMENT: return "invalid MPS source argument";
    case MPS_SOURCE_ERR_OPEN: return "unable to open MPS image";
    case MPS_SOURCE_ERR_DIMENSION: return "MPS image dimensions do not match screen";
    case MPS_SOURCE_ERR_ALLOC: return "unable to allocate MPS source";
    case MPS_SOURCE_ERR_READ: return "unable to read MPS tile";
    }
    return "unknown MPS source error";
}

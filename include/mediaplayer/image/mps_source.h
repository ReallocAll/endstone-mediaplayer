#ifndef ENDSTONE_MEDIAPLAYER_IMAGE_MPS_SOURCE_H
#define ENDSTONE_MEDIAPLAYER_IMAGE_MPS_SOURCE_H

#include "mediaplayer/image/mps_format.h"
#include "mediaplayer/image/mps_catalog.h"
#include "mediaplayer/screen/presenter.h"
#include "mediaplayer/screen/screen_registry.h"

#include <stdbool.h>
#include <stdint.h>

#define MPS_SOURCE_MAX SCREEN_REGISTRY_MAX

enum mps_source_error {
    MPS_SOURCE_OK = 0,
    MPS_SOURCE_ERR_ARGUMENT,
    MPS_SOURCE_ERR_OPEN,
    MPS_SOURCE_ERR_DIMENSION,
    MPS_SOURCE_ERR_ALLOC,
    MPS_SOURCE_ERR_READ,
};

struct mps_source {
    bool active;
    uint64_t screen_runtime_id;
    uint64_t attachment_id;
    struct mps_file file;
    char image_name[MPS_IMAGE_NAME_MAX];
    uint8_t *tile_cache;
    uint64_t cached_tile_index;
    bool cache_valid;
    uint64_t initial_cursor;
    uint64_t tile_read_count;
};

struct mps_source_engine {
    struct mps_source sources[MPS_SOURCE_MAX];
    uint64_t next_attachment_id;
};

void mps_source_engine_init(struct mps_source_engine *engine);
void mps_source_engine_shutdown(struct mps_source_engine *engine);

struct mps_source *mps_source_find(struct mps_source_engine *engine,
                                   uint64_t screen_runtime_id);

enum mps_source_error mps_source_start(
    struct mps_source_engine *engine, uint64_t screen_runtime_id,
    const char *path, const char *name, int expected_width,
    int expected_height, struct mps_source **out);
void mps_source_release(struct mps_source_engine *engine,
                        uint64_t screen_runtime_id);

enum mps_source_error mps_source_read(
    struct mps_source *source, uint64_t tile_index,
    struct presenter_stream_tile *out);

bool mps_source_presenter_read(
    void *context, uint64_t tile_index, uint32_t tile_x, uint32_t tile_y,
    struct presenter_stream_tile *out);

const char *mps_source_error_name(enum mps_source_error error);

#endif // ENDSTONE_MEDIAPLAYER_IMAGE_MPS_SOURCE_H

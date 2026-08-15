#ifndef ENDSTONE_MEDIAPLAYER_IMAGE_MPS_CATALOG_H
#define ENDSTONE_MEDIAPLAYER_IMAGE_MPS_CATALOG_H

#include <stdint.h>

#define MPS_IMAGE_NAME_MAX 128
#define MPS_IMAGE_PATH_MAX 512
#define MPS_IMAGE_CATALOG_MAX 256

struct mps_image_entry {
    char name[MPS_IMAGE_NAME_MAX];
    char path[MPS_IMAGE_PATH_MAX];
    uint32_t tile_width;
    uint32_t tile_height;
    uint64_t tile_count;
};

struct mps_catalog {
    struct mps_image_entry entries[MPS_IMAGE_CATALOG_MAX];
    int count;
    char image_dir[MPS_IMAGE_PATH_MAX];
};

void mps_catalog_init(struct mps_catalog *catalog, const char *image_dir);
void mps_catalog_refresh(struct mps_catalog *catalog);

#endif // ENDSTONE_MEDIAPLAYER_IMAGE_MPS_CATALOG_H

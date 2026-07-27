#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_CATALOG_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_CATALOG_H

#include <stdint.h>

#define VIDEO_NAME_MAX 128
#define VIDEO_CATALOG_MAX 256

struct video_entry {
    char name[VIDEO_NAME_MAX]; // File name without extension.
    char path[512];
    uint16_t tile_width;
    uint16_t tile_height;
    uint64_t frame_count;
    uint16_t fps_num;
    uint16_t fps_den;
};

struct video_catalog {
    struct video_entry entries[VIDEO_CATALOG_MAX];
    int count;
    char video_dir[512];
};

// Initializes the catalog and scans the video directory.
void video_catalog_init(struct video_catalog *cat, const char *video_dir);

// Rescans the video directory.
void video_catalog_refresh(struct video_catalog *cat);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_CATALOG_H

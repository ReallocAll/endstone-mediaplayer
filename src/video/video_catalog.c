#include "mediaplayer/video/video_catalog.h"
#include "mediaplayer/video/video_format.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>

#if defined(ES_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <dirent.h>
#endif

static void scan_video_dir(struct video_catalog *cat)
{
    cat->count = 0;
    char pattern[600];

#if defined(ES_PLATFORM_WINDOWS)
    snprintf(pattern, sizeof(pattern), "%s\\*.mcv", cat->video_dir);

    wchar_t wpattern[600];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, 600);
    if (wlen <= 0) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (cat->count >= VIDEO_CATALOG_MAX)
            break;

        char fname[256];
        int flen = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, fname, 256, nullptr, nullptr);
        if (flen <= 0) continue;

        size_t nlen = strlen(fname);
        if (nlen > 4 && strcmp(fname + nlen - 4, ".mcv") == 0)
            fname[nlen - 4] = '\0';
        else
            continue;

        struct video_entry *e = &cat->entries[cat->count];
        memset(e, 0, sizeof(*e));

        size_t copy_len = strlen(fname);
        if (copy_len >= VIDEO_NAME_MAX) copy_len = VIDEO_NAME_MAX - 1;
        memcpy(e->name, fname, copy_len);
        e->name[copy_len] = '\0';

        snprintf(e->path, sizeof(e->path), "%s/%s.mcv", cat->video_dir, fname);

        struct mcv_file mf;
        if (mcv_open(e->path, &mf) == MCV_OK) {
            e->tile_width = mf.header.tile_width;
            e->tile_height = mf.header.tile_height;
            e->frame_count = mf.header.frame_count;
            e->fps_num = mf.header.fps_num;
            e->fps_den = mf.header.fps_den;
            mcv_close(&mf);
        }

        cat->count++;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
#else
    snprintf(pattern, sizeof(pattern), "%s", cat->video_dir);
    DIR *dir = opendir(pattern);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr && cat->count < VIDEO_CATALOG_MAX) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".mcv") != 0)
            continue;

        struct video_entry *e = &cat->entries[cat->count];
        memset(e, 0, sizeof(*e));

        size_t copy_len = nlen - 4;
        if (copy_len >= VIDEO_NAME_MAX) copy_len = VIDEO_NAME_MAX - 1;
        memcpy(e->name, ent->d_name, copy_len);
        e->name[copy_len] = '\0';

        snprintf(e->path, sizeof(e->path), "%s/%s", cat->video_dir, ent->d_name);

        struct mcv_file mf;
        if (mcv_open(e->path, &mf) == MCV_OK) {
            e->tile_width = mf.header.tile_width;
            e->tile_height = mf.header.tile_height;
            e->frame_count = mf.header.frame_count;
            e->fps_num = mf.header.fps_num;
            e->fps_den = mf.header.fps_den;
            mcv_close(&mf);
        }

        cat->count++;
    }
    closedir(dir);
#endif
}

void video_catalog_init(struct video_catalog *cat, const char *video_dir)
{
    memset(cat, 0, sizeof(*cat));
    size_t dlen = strlen(video_dir);
    if (dlen >= sizeof(cat->video_dir)) dlen = sizeof(cat->video_dir) - 1;
    memcpy(cat->video_dir, video_dir, dlen);
    cat->video_dir[dlen] = '\0';
    scan_video_dir(cat);
}

void video_catalog_refresh(struct video_catalog *cat)
{
    scan_video_dir(cat);
}

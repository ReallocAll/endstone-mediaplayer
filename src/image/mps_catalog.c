#include "mediaplayer/image/mps_catalog.h"
#include "mediaplayer/image/mps_format.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>

#if defined(ES_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <dirent.h>
#endif

static void add_entry(struct mps_catalog *catalog, const char *filename,
                      const char *name)
{
    if (catalog->count >= MPS_IMAGE_CATALOG_MAX || !filename || !name)
        return;

    char path[MPS_IMAGE_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s", catalog->image_dir,
                           filename);
    if (written < 0 || (size_t)written >= sizeof(path))
        return;

    struct mps_file file = {0};
    if (mps_open(path, &file) != MPS_OK)
        return;

    struct mps_image_entry *entry = &catalog->entries[catalog->count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->tile_width = file.header.tile_width;
    entry->tile_height = file.header.tile_height;
    entry->tile_count = file.header.tile_count;
    mps_close(&file);
    catalog->count++;
}

static void scan_directory(struct mps_catalog *catalog)
{
    catalog->count = 0;
#if defined(ES_PLATFORM_WINDOWS)
    char pattern[MPS_IMAGE_PATH_MAX + 8];
    int written = snprintf(pattern, sizeof(pattern), "%s\\*.mps",
                           catalog->image_dir);
    if (written < 0 || (size_t)written >= sizeof(pattern))
        return;

    wchar_t wide_pattern[MPS_IMAGE_PATH_MAX + 8];
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, pattern, -1,
                                          wide_pattern,
                                          (int)(sizeof(wide_pattern) /
                                                sizeof(wide_pattern[0])));
    if (wide_length <= 0)
        return;

    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(wide_pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (catalog->count >= MPS_IMAGE_CATALOG_MAX)
            break;

        char filename[MPS_IMAGE_NAME_MAX];
        int length = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1,
                                         filename, sizeof(filename), nullptr,
                                         nullptr);
        if (length <= 0)
            continue;
        size_t name_length = strlen(filename);
        if (name_length < 5 ||
            strcmp(filename + name_length - 4, ".mps") != 0)
            continue;
        char name[MPS_IMAGE_NAME_MAX];
        size_t base_length = name_length - 4;
        if (base_length >= sizeof(name))
            base_length = sizeof(name) - 1;
        memcpy(name, filename, base_length);
        name[base_length] = '\0';
        add_entry(catalog, filename, name);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
#else
    DIR *directory = opendir(catalog->image_dir);
    if (!directory)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr &&
           catalog->count < MPS_IMAGE_CATALOG_MAX) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".mps") != 0)
            continue;
        char name[MPS_IMAGE_NAME_MAX];
        size_t name_length = length - 4;
        if (name_length >= sizeof(name))
            name_length = sizeof(name) - 1;
        memcpy(name, entry->d_name, name_length);
        name[name_length] = '\0';
        add_entry(catalog, entry->d_name, name);
    }
    closedir(directory);
#endif
}

void mps_catalog_init(struct mps_catalog *catalog, const char *image_dir)
{
    if (!catalog)
        return;
    memset(catalog, 0, sizeof(*catalog));
    if (!image_dir)
        return;
    snprintf(catalog->image_dir, sizeof(catalog->image_dir), "%s", image_dir);
    scan_directory(catalog);
}

void mps_catalog_refresh(struct mps_catalog *catalog)
{
    if (catalog)
        scan_directory(catalog);
}

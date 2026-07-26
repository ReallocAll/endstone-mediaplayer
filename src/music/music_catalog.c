#define _POSIX_C_SOURCE 200809L

#include "mediaplayer/music/music_catalog.h"
#include <stb_ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

void music_catalog_init(struct music_catalog *catalog, const char *data_dir)
{
    snprintf(catalog->data_dir, sizeof(catalog->data_dir), "%s", data_dir);
    snprintf(catalog->nbs_dir, sizeof(catalog->nbs_dir),
             "%s/nbs", catalog->data_dir);
#if defined(_WIN32)
    CreateDirectoryA(catalog->data_dir, nullptr);
    CreateDirectoryA(catalog->nbs_dir, nullptr);
#else
    mkdir(catalog->data_dir, 0755);
    mkdir(catalog->nbs_dir, 0755);
#endif
}

int music_catalog_list(const struct music_catalog *catalog, char ***names)
{
    *names = nullptr;
    int count = 0;
#if defined(_WIN32)
    wchar_t directory[ENDSTONE_MEDIAPLAYER_PATH_MAX];
    MultiByteToWideChar(CP_UTF8, 0, catalog->nbs_dir, -1, directory,
                        ENDSTONE_MEDIAPLAYER_PATH_MAX);

    wchar_t pattern[ENDSTONE_MEDIAPLAYER_PATH_MAX];
    swprintf(pattern, ENDSTONE_MEDIAPLAYER_PATH_MAX, L"%s\\*.nbs",
             directory);

    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char name_buffer[512];
            WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1,
                                name_buffer, sizeof(name_buffer),
                                nullptr, nullptr);
            char *name = _strdup(name_buffer);
            arrput(*names, name);
            count++;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
#else
    DIR *directory = opendir(catalog->nbs_dir);
    if (!directory) return 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        size_t length = strlen(entry->d_name);
        if (length > 4 &&
            strcmp(entry->d_name + length - 4, ".nbs") == 0) {
            char *name = strdup(entry->d_name);
            arrput(*names, name);
            count++;
        }
    }
    closedir(directory);
#endif
    return count;
}

void music_catalog_free_list(char **names, int count)
{
    for (int i = 0; i < count; i++) free(names[i]);
    arrfree(names);
}

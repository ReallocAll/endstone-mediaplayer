#include "mediaplayer/video/video_preferences.h"
#include "mediaplayer/endstone_api.h"
#include "cJSON.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ES_PLATFORM_WINDOWS)
#include <windows.h>
#endif

void mpv_preferences_init(struct mpv_preferences *preferences)
{
    memset(preferences, 0, sizeof(*preferences));
}

static int find_disabled(const struct mpv_preferences *preferences,
                         const char *uuid)
{
    if (!preferences || !uuid)
        return -1;
    for (int i = 0; i < preferences->count; i++) {
        if (strcmp(preferences->disabled[i], uuid) == 0)
            return i;
    }
    return -1;
}

bool mpv_preferences_enabled(const struct mpv_preferences *preferences,
                             const char *uuid)
{
    return find_disabled(preferences, uuid) < 0;
}

bool mpv_preferences_set_enabled(struct mpv_preferences *preferences,
                                 const char *uuid, bool enabled)
{
    if (!preferences || !uuid || !uuid[0] ||
        strlen(uuid) > SCREEN_UUID_LEN) {
        return false;
    }

    int index = find_disabled(preferences, uuid);
    if (enabled) {
        if (index < 0)
            return true;
        for (int i = index; i < preferences->count - 1; i++) {
            memcpy(preferences->disabled[i], preferences->disabled[i + 1],
                   sizeof(preferences->disabled[i]));
        }
        preferences->count--;
        memset(preferences->disabled[preferences->count], 0,
               sizeof(preferences->disabled[preferences->count]));
        return true;
    }

    if (index >= 0)
        return true;
    if (preferences->count >= MPV_PREFERENCE_MAX)
        return false;
    snprintf(preferences->disabled[preferences->count],
             sizeof(preferences->disabled[preferences->count]), "%s", uuid);
    preferences->count++;
    return true;
}

int mpv_preferences_save(const struct mpv_preferences *preferences,
                         const char *path)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return -1;
    cJSON_AddNumberToObject(root, "format_version", MPV_PREFERENCE_VERSION);
    cJSON *disabled = cJSON_AddArrayToObject(root, "disabled");
    if (!disabled) {
        cJSON_Delete(root);
        return -1;
    }
    for (int i = 0; i < preferences->count; i++) {
        cJSON_AddItemToArray(
            disabled, cJSON_CreateString(preferences->disabled[i]));
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json)
        return -1;

    char temporary[600];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen_utf8(temporary, "wb");
    if (!file) {
        free(json);
        return -1;
    }
    size_t length = strlen(json);
    bool written = fwrite(json, 1, length, file) == length;
    bool closed = fclose(file) == 0;
    free(json);
    if (!written || !closed) {
        remove(temporary);
        return -1;
    }

#if defined(ES_PLATFORM_WINDOWS)
    if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING)) {
        remove(temporary);
        return -1;
    }
#else
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return -1;
    }
#endif
    return 0;
}

int mpv_preferences_load(struct mpv_preferences *preferences,
                         const char *path)
{
    mpv_preferences_init(preferences);
    FILE *file = fopen_utf8(path, "rb");
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long file_size = ftell(file);
    if (file_size < 0 || file_size > 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    char *json = calloc((size_t)file_size + 1, 1);
    if (!json) {
        fclose(file);
        return -1;
    }
    bool read = fread(json, 1, (size_t)file_size, file) ==
                (size_t)file_size;
    fclose(file);
    if (!read) {
        free(json);
        return -1;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root)
        return -1;
    cJSON *version = cJSON_GetObjectItem(root, "format_version");
    cJSON *disabled = cJSON_GetObjectItem(root, "disabled");
    if (!cJSON_IsNumber(version) ||
        version->valueint != MPV_PREFERENCE_VERSION ||
        !cJSON_IsArray(disabled)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(disabled);
    for (int i = 0; i < count && preferences->count < MPV_PREFERENCE_MAX; i++) {
        cJSON *entry = cJSON_GetArrayItem(disabled, i);
        if (cJSON_IsString(entry) && entry->valuestring &&
            entry->valuestring[0] &&
            strlen(entry->valuestring) <= SCREEN_UUID_LEN) {
            mpv_preferences_set_enabled(preferences, entry->valuestring,
                                        false);
        }
    }
    cJSON_Delete(root);
    return 0;
}

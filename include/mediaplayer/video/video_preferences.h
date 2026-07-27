#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_PREFERENCES_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_PREFERENCES_H

#include "mediaplayer/screen/screen_registry.h"
#include <stdbool.h>

#define MPV_PREFERENCE_MAX 1024
#define MPV_PREFERENCE_VERSION 1

struct mpv_preferences {
    char disabled[MPV_PREFERENCE_MAX][SCREEN_UUID_LEN + 1];
    int count;
};

void mpv_preferences_init(struct mpv_preferences *preferences);
bool mpv_preferences_enabled(const struct mpv_preferences *preferences,
                             const char *uuid);
bool mpv_preferences_set_enabled(struct mpv_preferences *preferences,
                                 const char *uuid, bool enabled);
int mpv_preferences_load(struct mpv_preferences *preferences,
                         const char *path);
int mpv_preferences_save(const struct mpv_preferences *preferences,
                         const char *path);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_PREFERENCES_H

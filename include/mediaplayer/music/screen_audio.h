#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_SCREEN_AUDIO_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_SCREEN_AUDIO_H

#include "mediaplayer/music/music_cache.h"
#include "mediaplayer/music/music_catalog.h"
#include "mediaplayer/screen/screen_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum screen_audio_start_result {
    SCREEN_AUDIO_OK = 0,
    SCREEN_AUDIO_NOT_FOUND,
    SCREEN_AUDIO_FILE_ERROR,
    SCREEN_AUDIO_PARSE_ERROR,
    SCREEN_AUDIO_VERSION_ERROR,
    SCREEN_AUDIO_LIMIT_ERROR,
    SCREEN_AUDIO_NO_SLOT,
};

struct screen_audio_session {
    bool active;
    uint64_t screen_runtime_id;
    int song_index;
    size_t cursor;
    int loop_current;
};

struct screen_audio_engine {
    struct screen_audio_session sessions[SCREEN_REGISTRY_MAX];
};

void screen_audio_engine_init(struct screen_audio_engine *engine);
void screen_audio_engine_shutdown(struct screen_audio_engine *engine);
struct screen_audio_session *screen_audio_find(
    struct screen_audio_engine *engine, uint64_t screen_runtime_id);
enum screen_audio_start_result screen_audio_start(
    struct screen_audio_engine *engine, struct music_cache *cache,
    const struct music_catalog *catalog, uint64_t screen_runtime_id,
    const char *video_name, struct nbs_error_info *error);
void screen_audio_stop(struct screen_audio_engine *engine,
                       uint64_t screen_runtime_id);
void screen_audio_tick(struct screen_audio_session *session,
                       struct music_cache *cache, int loop_current,
                       int64_t loop_elapsed_ms, bool loop_changed,
                       void **players, int player_count);
const char *screen_audio_start_result_name(
    enum screen_audio_start_result result);

#endif // ENDSTONE_MEDIAPLAYER_MUSIC_SCREEN_AUDIO_H

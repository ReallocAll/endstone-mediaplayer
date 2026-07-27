#include "mediaplayer/music/screen_audio.h"
#include "mediaplayer/music/music_sound.h"
#include "mediaplayer/endstone_api.h"
#include <stb_ds.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_AUDIO_LOOP_CATCHUP_MS 100

void screen_audio_engine_init(struct screen_audio_engine *engine)
{
    memset(engine, 0, sizeof(*engine));
}

void screen_audio_engine_shutdown(struct screen_audio_engine *engine)
{
    memset(engine, 0, sizeof(*engine));
}

struct screen_audio_session *screen_audio_find(
    struct screen_audio_engine *engine, uint64_t screen_runtime_id)
{
    if (!engine || screen_runtime_id == 0)
        return nullptr;
    for (int i = 0; i < SCREEN_REGISTRY_MAX; i++) {
        if (engine->sessions[i].active &&
            engine->sessions[i].screen_runtime_id == screen_runtime_id) {
            return &engine->sessions[i];
        }
    }
    return nullptr;
}

static struct screen_audio_session *acquire_session(
    struct screen_audio_engine *engine, uint64_t screen_runtime_id)
{
    struct screen_audio_session *existing =
        screen_audio_find(engine, screen_runtime_id);
    if (existing)
        return existing;
    for (int i = 0; i < SCREEN_REGISTRY_MAX; i++) {
        if (!engine->sessions[i].active)
            return &engine->sessions[i];
    }
    return nullptr;
}

static enum screen_audio_start_result parse_song(
    struct music_cache *cache, const struct music_catalog *catalog,
    const char *video_name, int *song_index, struct nbs_error_info *error)
{
    char filename[MUSIC_SONG_NAME_MAX + 5];
    snprintf(filename, sizeof(filename), "%s.nbs", video_name);

    char **names = nullptr;
    int name_count = music_catalog_list(catalog, &names);
    bool found = false;
    for (int i = 0; i < name_count; i++) {
        if (strcmp(names[i], filename) == 0) {
            found = true;
            break;
        }
    }
    music_catalog_free_list(names, name_count);
    if (!found)
        return SCREEN_AUDIO_NOT_FOUND;

    long long cached = music_cache_find(cache, video_name);
    if (cached >= 0) {
        *song_index = (int)cached;
        return SCREEN_AUDIO_OK;
    }

    char path[ENDSTONE_MEDIAPLAYER_PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s",
                          catalog->nbs_dir, filename);
    if (length < 0 || (size_t)length >= sizeof(path))
        return SCREEN_AUDIO_FILE_ERROR;

    FILE *file = fopen_utf8(path, "rb");
    if (!file)
        return SCREEN_AUDIO_FILE_ERROR;

    struct nbs_error_info parse_error = {0};
    cached = music_cache_parse(cache, file, video_name, &parse_error);
    fclose(file);
    if (cached < 0) {
        if (error)
            *error = parse_error;
        switch (parse_error.code) {
        case NBS_ERROR_UNSUPPORTED_VERSION:
            return SCREEN_AUDIO_VERSION_ERROR;
        case NBS_ERROR_LIMIT_EXCEEDED:
            return SCREEN_AUDIO_LIMIT_ERROR;
        default:
            return SCREEN_AUDIO_PARSE_ERROR;
        }
    }

    *song_index = (int)cached;
    return SCREEN_AUDIO_OK;
}

enum screen_audio_start_result screen_audio_start(
    struct screen_audio_engine *engine, struct music_cache *cache,
    const struct music_catalog *catalog, uint64_t screen_runtime_id,
    const char *video_name, struct nbs_error_info *error)
{
    if (!engine || !cache || !catalog || screen_runtime_id == 0 ||
        !video_name || !video_name[0]) {
        return SCREEN_AUDIO_FILE_ERROR;
    }

    int song_index = -1;
    enum screen_audio_start_result result =
        parse_song(cache, catalog, video_name, &song_index, error);
    if (result != SCREEN_AUDIO_OK)
        return result;

    struct screen_audio_session *session =
        acquire_session(engine, screen_runtime_id);
    if (!session)
        return SCREEN_AUDIO_NO_SLOT;

    memset(session, 0, sizeof(*session));
    session->active = true;
    session->screen_runtime_id = screen_runtime_id;
    session->song_index = song_index;
    session->loop_current = 1;
    return SCREEN_AUDIO_OK;
}

void screen_audio_stop(struct screen_audio_engine *engine,
                       uint64_t screen_runtime_id)
{
    struct screen_audio_session *session =
        screen_audio_find(engine, screen_runtime_id);
    if (session)
        memset(session, 0, sizeof(*session));
}

void screen_audio_tick(struct screen_audio_session *session,
                       struct music_cache *cache, int loop_current,
                       int64_t loop_elapsed_ms, bool loop_changed,
                       void **players, int player_count)
{
    if (!session || !session->active || !cache)
        return;
    struct music_cache_entry *song =
        music_cache_get(cache, session->song_index);
    if (!song)
        return;

    size_t note_count = arrlen(song->notes);
    if (loop_elapsed_ms < 0)
        loop_elapsed_ms = 0;
    if (loop_changed || loop_current != session->loop_current) {
        session->cursor = 0;
        session->loop_current = loop_current;
        int64_t cutoff = loop_elapsed_ms - SCREEN_AUDIO_LOOP_CATCHUP_MS;
        while (session->cursor < note_count &&
               song->notes[session->cursor].time_ms < cutoff) {
            session->cursor++;
        }
    }

    while (session->cursor < note_count &&
           song->notes[session->cursor].time_ms <= loop_elapsed_ms) {
        music_note_play(&song->notes[session->cursor], players, player_count);
        session->cursor++;
    }
}

const char *screen_audio_start_result_name(
    enum screen_audio_start_result result)
{
    switch (result) {
    case SCREEN_AUDIO_OK: return "ok";
    case SCREEN_AUDIO_NOT_FOUND: return "no matching NBS";
    case SCREEN_AUDIO_FILE_ERROR: return "NBS file error";
    case SCREEN_AUDIO_PARSE_ERROR: return "invalid NBS";
    case SCREEN_AUDIO_VERSION_ERROR: return "unsupported NBS version";
    case SCREEN_AUDIO_LIMIT_ERROR: return "NBS limit exceeded";
    case SCREEN_AUDIO_NO_SLOT: return "no free audio session";
    default: return "unknown audio error";
    }
}

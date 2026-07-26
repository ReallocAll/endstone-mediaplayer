#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_SESSION_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_SESSION_H

#include "mediaplayer/music/music_cache.h"
#include "mediaplayer/music/music_catalog.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum music_bar_type {
    MUSIC_BAR_NOT_DISPLAY = 0,
    MUSIC_BAR_POPUP,
    MUSIC_BAR_TIP,
    MUSIC_BAR_BOSSBAR,
};

enum music_enqueue_result {
    MUSIC_ENQUEUE_OK = 0,
    MUSIC_ENQUEUE_BAD_LOOP,
    MUSIC_ENQUEUE_BAD_BAR,
    MUSIC_ENQUEUE_FILE_ERROR,
    MUSIC_ENQUEUE_NBS_PARSE_ERROR,
    MUSIC_ENQUEUE_NBS_VERSION_ERROR,
    MUSIC_ENQUEUE_NBS_LIMIT_ERROR,
};

enum music_player_result {
    MUSIC_PLAYER_OK = 0,
    MUSIC_PLAYER_NO_PLAYLIST,
    MUSIC_PLAYER_INDEX_OUT_OF_RANGE,
    MUSIC_PLAYER_ALREADY_PAUSED,
    MUSIC_PLAYER_NOT_PAUSED,
};

struct music_queue_entry {
    int song_index;
    size_t cursor;
    int64_t start_ms;
    int64_t pause_elapsed;
    int loop;
    enum music_bar_type bar_type;
    void *boss_bar;
};

struct music_player {
    void *player;
    struct music_queue_entry *playlist;
    size_t current_track;
    bool paused;
};

struct music_engine {
    struct music_player *players;
    int64_t tick_start_ms;
};

void music_engine_init(struct music_engine *engine);
void music_engine_shutdown(struct music_engine *engine);
long long music_engine_find(const struct music_engine *engine, void *player);
enum music_enqueue_result music_engine_enqueue(
    struct music_engine *engine, struct music_cache *cache,
    const struct music_catalog *catalog, void *player,
    const char *nbs_file, int loop, enum music_bar_type bar,
    struct nbs_error_info *error);
enum music_player_result music_engine_dequeue(
    struct music_engine *engine, void *player, size_t index);
enum music_player_result music_engine_stop(
    struct music_engine *engine, void *player);
enum music_player_result music_engine_pause(
    struct music_engine *engine, void *player);
enum music_player_result music_engine_resume(
    struct music_engine *engine, void *player);
void music_engine_remove_player(struct music_engine *engine, void *player);
void music_engine_tick(struct music_engine *engine,
                       struct music_cache *cache);

#endif // ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_SESSION_H

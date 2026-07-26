#define _POSIX_C_SOURCE 200809L

#include "mediaplayer/music/music_session.h"
#include "mediaplayer/endstone_api.h"
#include "abi_helpers.h"
#include <stb_ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static const char *const g_instruments[MUSIC_INSTRUMENT_COUNT] = {
    "note.harp", "note.bassattack", "note.bd", "note.snare",
    "note.hat", "note.guitar", "note.flute", "note.bell",
    "note.chime", "note.xylobone", "note.iron_xylophone", "note.cow_bell",
    "note.didgeridoo", "note.bit", "note.banjo", "note.pling",
};

static int64_t monotonic_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    static bool initialized;
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000 / frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
}

static int64_t playback_ms(struct music_engine *engine)
{
    return monotonic_ms() - engine->tick_start_ms;
}

static void destroy_boss_bars(struct music_player *player)
{
    int count = (int)arrlen(player->playlist);
    for (int i = 0; i < count; i++) {
        if (player->playlist[i].boss_bar) {
            boss_bar_destroy(player->playlist[i].boss_bar);
            player->playlist[i].boss_bar = nullptr;
        }
    }
}

static void path_stem(const char *filename, char *output, size_t output_size)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        snprintf(output, output_size, "%s", filename);
        return;
    }
    size_t length = (size_t)(dot - filename);
    if (length >= output_size) length = output_size - 1;
    memcpy(output, filename, length);
    output[length] = '\0';
}

void music_engine_init(struct music_engine *engine)
{
    memset(engine, 0, sizeof(*engine));
}

void music_engine_shutdown(struct music_engine *engine)
{
    int count = (int)arrlen(engine->players);
    for (int i = 0; i < count; i++) {
        destroy_boss_bars(&engine->players[i]);
        arrfree(engine->players[i].playlist);
    }
    arrfree(engine->players);
    memset(engine, 0, sizeof(*engine));
}

long long music_engine_find(const struct music_engine *engine, void *player)
{
    int count = (int)arrlen(engine->players);
    for (int i = 0; i < count; i++) {
        if (engine->players[i].player == player)
            return i;
    }
    return -1;
}

enum music_enqueue_result music_engine_enqueue(
    struct music_engine *engine, struct music_cache *cache,
    const struct music_catalog *catalog, void *player,
    const char *nbs_file, int loop, enum music_bar_type bar,
    struct nbs_error_info *error)
{
    if (loop < -1) return MUSIC_ENQUEUE_BAD_LOOP;
    if (bar < MUSIC_BAR_NOT_DISPLAY || bar > MUSIC_BAR_BOSSBAR)
        return MUSIC_ENQUEUE_BAD_BAR;

    char stem[MUSIC_SONG_NAME_MAX];
    path_stem(nbs_file, stem, sizeof(stem));

    long long cache_index = music_cache_find(cache, stem);
    if (cache_index == -1) {
        char path[ENDSTONE_MEDIAPLAYER_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", catalog->nbs_dir, nbs_file);
        FILE *file = fopen_utf8(path, "rb");
        if (!file) return MUSIC_ENQUEUE_FILE_ERROR;

        struct nbs_error_info parse_error;
        cache_index = music_cache_parse(cache, file, stem, &parse_error);
        fclose(file);

        if (cache_index == -1) {
            if (error) *error = parse_error;
            switch (parse_error.code) {
            case NBS_ERROR_UNSUPPORTED_VERSION:
                return MUSIC_ENQUEUE_NBS_VERSION_ERROR;
            case NBS_ERROR_LIMIT_EXCEEDED:
                return MUSIC_ENQUEUE_NBS_LIMIT_ERROR;
            default:
                return MUSIC_ENQUEUE_NBS_PARSE_ERROR;
            }
        }
    }

    struct music_queue_entry entry = {
        .song_index = (int)cache_index,
        .loop = loop,
        .bar_type = bar,
    };

    long long position = music_engine_find(engine, player);
    if (position == -1) {
        struct music_player music_player = {
            .player = player,
        };
        arrput(music_player.playlist, entry);
        arrput(engine->players, music_player);
    } else {
        arrput(engine->players[position].playlist, entry);
    }
    return MUSIC_ENQUEUE_OK;
}

enum music_player_result music_engine_dequeue(
    struct music_engine *engine, void *player, size_t index)
{
    long long position = music_engine_find(engine, player);
    if (position < 0) return MUSIC_PLAYER_NO_PLAYLIST;

    struct music_player *music_player = &engine->players[position];
    if (index >= (size_t)arrlen(music_player->playlist))
        return MUSIC_PLAYER_INDEX_OUT_OF_RANGE;

    if (music_player->playlist[index].boss_bar) {
        boss_bar_destroy(music_player->playlist[index].boss_bar);
        music_player->playlist[index].boss_bar = nullptr;
    }

    if (index == music_player->current_track) {
        arrdel(music_player->playlist, (int)index);
        if (arrlen(music_player->playlist) == 0) {
            arrfree(music_player->playlist);
            arrdelswap(engine->players, (int)position);
        } else {
            music_player = &engine->players[position];
            if (music_player->current_track >=
                (size_t)arrlen(music_player->playlist)) {
                music_player->current_track =
                    (size_t)arrlen(music_player->playlist) - 1;
            }
            struct music_queue_entry *current =
                &music_player->playlist[music_player->current_track];
            current->start_ms = 0;
            current->cursor = 0;
        }
    } else {
        arrdel(music_player->playlist, (int)index);
        if (index < music_player->current_track)
            music_player->current_track--;
    }
    return MUSIC_PLAYER_OK;
}

enum music_player_result music_engine_stop(
    struct music_engine *engine, void *player)
{
    long long position = music_engine_find(engine, player);
    if (position < 0) return MUSIC_PLAYER_NO_PLAYLIST;
    destroy_boss_bars(&engine->players[position]);
    arrfree(engine->players[position].playlist);
    arrdelswap(engine->players, (int)position);
    return MUSIC_PLAYER_OK;
}

enum music_player_result music_engine_pause(
    struct music_engine *engine, void *player)
{
    long long position = music_engine_find(engine, player);
    if (position < 0) return MUSIC_PLAYER_NO_PLAYLIST;
    struct music_player *music_player = &engine->players[position];
    if (arrlen(music_player->playlist) == 0)
        return MUSIC_PLAYER_NO_PLAYLIST;
    if (music_player->paused)
        return MUSIC_PLAYER_ALREADY_PAUSED;
    music_player->paused = true;

    struct music_queue_entry *entry =
        &music_player->playlist[music_player->current_track];
    entry->pause_elapsed = playback_ms(engine) - entry->start_ms;

    if (entry->bar_type == MUSIC_BAR_BOSSBAR && entry->boss_bar) {
        boss_bar_set_title(entry->boss_bar, MC_GRAY "Paused");
    } else if (entry->bar_type == MUSIC_BAR_POPUP) {
        player_send_popup(music_player->player, MC_GRAY "Paused");
    } else if (entry->bar_type == MUSIC_BAR_TIP) {
        player_send_tip(music_player->player, MC_GRAY "Paused");
    }
    return MUSIC_PLAYER_OK;
}

enum music_player_result music_engine_resume(
    struct music_engine *engine, void *player)
{
    long long position = music_engine_find(engine, player);
    if (position < 0) return MUSIC_PLAYER_NO_PLAYLIST;
    struct music_player *music_player = &engine->players[position];
    if (arrlen(music_player->playlist) == 0)
        return MUSIC_PLAYER_NO_PLAYLIST;
    if (!music_player->paused)
        return MUSIC_PLAYER_NOT_PAUSED;
    music_player->paused = false;

    struct music_queue_entry *entry =
        &music_player->playlist[music_player->current_track];
    entry->start_ms = playback_ms(engine) - entry->pause_elapsed;
    entry->pause_elapsed = 0;
    return MUSIC_PLAYER_OK;
}

void music_engine_remove_player(struct music_engine *engine, void *player)
{
    long long position = music_engine_find(engine, player);
    if (position < 0) return;
    destroy_boss_bars(&engine->players[position]);
    arrfree(engine->players[position].playlist);
    arrdelswap(engine->players, (int)position);
}

static void update_progress(struct music_player *player,
                            struct music_queue_entry *entry,
                            const struct music_cache_entry *song,
                            int64_t elapsed)
{
    if (entry->bar_type == MUSIC_BAR_NOT_DISPLAY) return;

    int64_t total = song->duration_ms;
    if (total == 0) total = 1;
    float progress = (float)elapsed / (float)total;
    if (progress > 1.0f) progress = 1.0f;

    int total_minutes = (int)(total / 60000);
    int total_seconds = (int)((total / 1000) % 60);
    int played_minutes = (int)(elapsed / 60000);
    int played_seconds = (int)((elapsed / 1000) % 60);

    if (entry->bar_type == MUSIC_BAR_BOSSBAR) {
        if (!entry->boss_bar)
            entry->boss_bar = boss_bar_create(player->player, song->song_name);
        if (!entry->boss_bar) return;

        boss_bar_set_progress(entry->boss_bar, progress);
        char title[300];
        snprintf(title, sizeof(title),
                 MC_GREEN "%s" MC_GOLD " | " MC_AQUA "%d:%02d"
                 MC_GRAY "/" MC_AQUA "%d:%02d",
                 song->song_name, played_minutes, played_seconds,
                 total_minutes, total_seconds);
        boss_bar_set_title(entry->boss_bar, title);
        return;
    }

    char message[80];
    snprintf(message, sizeof(message),
             MC_AQUA "%d:%02d" MC_GRAY "/" MC_AQUA "%d:%02d",
             played_minutes, played_seconds,
             total_minutes, total_seconds);
    if (entry->bar_type == MUSIC_BAR_POPUP)
        player_send_popup(player->player, message);
    else if (entry->bar_type == MUSIC_BAR_TIP)
        player_send_tip(player->player, message);
}

void music_engine_tick(struct music_engine *engine,
                       struct music_cache *cache)
{
    if (engine->tick_start_ms == 0)
        engine->tick_start_ms = monotonic_ms();
    int64_t now_ms = playback_ms(engine);

    int player_count = (int)arrlen(engine->players);
    for (int i = 0; i < player_count; i++) {
        struct music_player *player = &engine->players[i];
        if (player->paused || arrlen(player->playlist) == 0)
            continue;

        struct music_queue_entry *entry =
            &player->playlist[player->current_track];
        struct music_cache_entry *song =
            music_cache_get(cache, entry->song_index);
        if (!song) continue;

        if (entry->start_ms == 0)
            entry->start_ms = now_ms;

        int64_t elapsed = now_ms - entry->start_ms;
        size_t note_count = arrlen(song->notes);
        while (entry->cursor < note_count &&
               song->notes[entry->cursor].time_ms <= elapsed) {
            struct music_note *note = &song->notes[entry->cursor];
            player_play_sound(player->player,
                              g_instruments[note->instrument],
                              note->volume, note->pitch);
            entry->cursor++;
        }

        update_progress(player, entry, song, elapsed);

        if (entry->cursor >= note_count) {
            if (entry->boss_bar) {
                boss_bar_destroy(entry->boss_bar);
                entry->boss_bar = nullptr;
            }

            if (entry->loop > 1) {
                entry->loop--;
                entry->cursor = 0;
                entry->start_ms = now_ms;
            } else if (entry->loop == -1) {
                entry->cursor = 0;
                entry->start_ms = now_ms;
            } else {
                music_engine_dequeue(engine, player->player,
                                     player->current_track);
                i--;
                player_count = (int)arrlen(engine->players);
            }
        }
    }
}

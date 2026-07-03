#include "music_player.h"
#include "nbsparser.h"
#include "abi_helpers.h"
#include <stb_ds.h>
#include <cppcompat/string.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

struct music_player_ctx g_music_ctx;

// --- Path management ---
static char s_data[MAX_PATH_LEN];
static char s_nbs[MAX_PATH_LEN];

void path_init(const char *data_folder)
{
    snprintf(s_data, MAX_PATH_LEN, "%s", data_folder);
    snprintf(s_nbs, MAX_PATH_LEN, "%s/nbs", s_data);
#ifdef _WIN32
    CreateDirectoryA(s_data, nullptr);
    CreateDirectoryA(s_nbs, nullptr);
#else
    mkdir(s_data, 0755);
    mkdir(s_nbs, 0755);
#endif
}

const char *path_data(void) { return s_data; }
const char *path_nbs(void)  { return s_nbs; }

// --- Directory listing (UTF-8 on Windows) ---
int list_nbs_files(const char *dir, char ***out_names)
{
    *out_names = nullptr;
    int count = 0;
#ifdef _WIN32
    wchar_t wdir[MAX_PATH_LEN];
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH_LEN);

    wchar_t wpattern[MAX_PATH_LEN];
    swprintf(wpattern, MAX_PATH_LEN, L"%s\\*.nbs", wdir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char utf8_name[512];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                utf8_name, sizeof(utf8_name), nullptr, nullptr);
            char *name = _strdup(utf8_name);
            arrput(*out_names, name);
            count++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcmp(ent->d_name + len - 4, ".nbs") == 0) {
            char *name = strdup(ent->d_name);
            arrput(*out_names, name);
            count++;
        }
    }
    closedir(d);
#endif
    return count;
}

void free_nbs_list(char **names, int count)
{
    for (int i = 0; i < count; i++) free(names[i]);
    arrfree(names);
}

// --- Init/Shutdown ---
void music_player_init(void)
{
    memset(&g_music_ctx, 0, sizeof(g_music_ctx));
}

static void cleanup_playlist_bossbars(struct player_music *pm)
{
    int len = (int)arrlen(pm->playlist);
    for (int i = 0; i < len; i++) {
        if (pm->playlist[i].boss_bar) {
            boss_bar_destroy(pm->playlist[i].boss_bar);
            pm->playlist[i].boss_bar = nullptr;
        }
    }
}

void music_player_shutdown(void)
{
    int len = (int)arrlen(g_music_ctx.online_players);
    for (int i = 0; i < len; i++) {
        cleanup_playlist_bossbars(&g_music_ctx.online_players[i]);
        arrfree(g_music_ctx.online_players[i].playlist);
    }
    arrfree(g_music_ctx.online_players);

    int cache_len = (int)arrlen(g_music_ctx.song_cache);
    for (int i = 0; i < cache_len; i++)
        arrfree(g_music_ctx.song_cache[i].notes);
    arrfree(g_music_ctx.song_cache);

    memset(&g_music_ctx, 0, sizeof(g_music_ctx));
}

// --- Song cache ---
long long song_cache_parse(FILE *fp, const char *song_name)
{
    struct nbs_song *nbs = nbs_parse(fp);
    if (!nbs) return -1;

    struct song_cache_entry entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.song_name, song_name, sizeof(entry.song_name) - 1);
    entry.notes = nullptr;

    float time_per_tick = 20.0f / nbs->tempo * 50.0f;
    int note_count = (int)arrlen(nbs->notes);
    int layer_count = (int)arrlen(nbs->layers);
    int instr_count = (int)arrlen(nbs->instruments);

    for (int i = 0; i < note_count; i++) {
        struct nbs_note *nn = &nbs->notes[i];
        struct nbs_layer *layer = (nn->layer < layer_count) ? &nbs->layers[nn->layer] : nullptr;

        int instrument_pitch = 45;
        if (nn->instrument < instr_count)
            instrument_pitch = (int)nbs->instruments[nn->instrument].pitch;

        int instrument = (nn->instrument <= 15) ? nn->instrument : 0;
        float volume = (float)nn->velocity / 100.0f;
        if (layer) volume *= (float)layer->volume / 100.0f;

        float final_key = (float)nn->key + (float)(instrument_pitch - 45) + (float)nn->pitch / 100.0f;
        float pitch = powf(2.0f, (final_key - 45.0f) / 12.0f);

        struct note nt;
        nt.time_ms = (int64_t)((float)nn->tick * time_per_tick);
        nt.instrument = instrument;
        nt.volume = volume;
        nt.pitch = pitch;
        arrput(entry.notes, nt);
    }

    nbs_free(nbs);

    if (arrlen(entry.notes) > 0)
        entry.duration_ms = entry.notes[arrlen(entry.notes) - 1].time_ms;
    else
        entry.duration_ms = 0;

    arrput(g_music_ctx.song_cache, entry);
    return arrlen(g_music_ctx.song_cache) - 1;
}

static long long song_cache_find(const char *song_name)
{
    int len = (int)arrlen(g_music_ctx.song_cache);
    for (int i = 0; i < len; i++) {
        if (strcmp(g_music_ctx.song_cache[i].song_name, song_name) == 0)
            return i;
    }
    return -1;
}

// --- Player lookup ---
long long player_music_find(void *player)
{
    int len = (int)arrlen(g_music_ctx.online_players);
    for (int i = 0; i < len; i++) {
        if (g_music_ctx.online_players[i].player == player)
            return i;
    }
    return -1;
}

// --- Path stem ---
static void do_path_stem(const char *filename, char *dst, size_t dst_size)
{
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        strncpy(dst, filename, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }
    size_t len = (size_t)(dot - filename);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, filename, len);
    dst[len] = '\0';
}

// --- Playlist operations ---
bool player_music_enqueue(void *player, const char *nbs_file, int loop, enum music_bar_type bar)
{
    char stem[256];
    do_path_stem(nbs_file, stem, sizeof(stem));

    long long cache_idx = song_cache_find(stem);
    if (cache_idx == -1) {
        char path[MAX_PATH_LEN];
        snprintf(path, MAX_PATH_LEN, "%s/%s", path_nbs(), nbs_file);
        FILE *fp = fopen_utf8(path, "rb");
        if (!fp) return false;
        cache_idx = song_cache_parse(fp, stem);
        fclose(fp);
        if (cache_idx == -1) return false;
    }

    struct music_queue_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.song_index = (int)cache_idx;
    entry.cursor = 0;
    entry.start_ms = 0;
    entry.loop = loop;
    entry.bar_type = bar;
    entry.boss_bar = nullptr;

    long long pos = player_music_find(player);
    if (pos == -1) {
        struct player_music pm;
        memset(&pm, 0, sizeof(pm));
        pm.player = player;
        pm.playlist = nullptr;
        pm.paused = false;
        pm.current_track = 0;
        arrput(pm.playlist, entry);
        arrput(g_music_ctx.online_players, pm);
    } else {
        arrput(g_music_ctx.online_players[pos].playlist, entry);
    }
    return true;
}

bool player_music_dequeue(void *player, size_t index)
{
    long long pos = player_music_find(player);
    if (pos < 0) return false;

    struct player_music *pm = &g_music_ctx.online_players[pos];
    if (index >= arrlen(pm->playlist)) return false;

    // Clean up boss bar for the entry being removed
    if (pm->playlist[index].boss_bar) {
        boss_bar_destroy(pm->playlist[index].boss_bar);
        pm->playlist[index].boss_bar = nullptr;
    }

    if (index == pm->current_track) {
        arrdel(pm->playlist, (int)index);
        if (arrlen(pm->playlist) == 0) {
            arrfree(pm->playlist);
            arrdelswap(g_music_ctx.online_players, (int)pos);
        } else {
            pm = &g_music_ctx.online_players[pos];
            if (pm->current_track >= arrlen(pm->playlist))
                pm->current_track = arrlen(pm->playlist) - 1;
            pm->playlist[pm->current_track].start_ms = 0;
            pm->playlist[pm->current_track].cursor = 0;
        }
    } else {
        arrdel(pm->playlist, (int)index);
        if (index < pm->current_track) pm->current_track--;
    }
    return true;
}

void player_music_stop(void *player)
{
    long long pos = player_music_find(player);
    if (pos < 0) return;
    cleanup_playlist_bossbars(&g_music_ctx.online_players[pos]);
    arrfree(g_music_ctx.online_players[pos].playlist);
    arrdelswap(g_music_ctx.online_players, (int)pos);
}

void player_music_pause(void *player)
{
    long long pos = player_music_find(player);
    if (pos < 0) return;
    struct player_music *pm = &g_music_ctx.online_players[pos];
    pm->paused = true;

    if (arrlen(pm->playlist) == 0) return;
    struct music_queue_entry *entry = &pm->playlist[pm->current_track];

    if (entry->bar_type == MUSIC_BAR_BOSSBAR && entry->boss_bar) {
        boss_bar_set_title(entry->boss_bar, MC_GRAY "Paused");
    } else if (entry->bar_type == MUSIC_BAR_POPUP || entry->bar_type == MUSIC_BAR_TIP) {
        player_send_popup(pm->player, MC_GRAY "Paused");
    }
}

void player_music_resume(void *player)
{
    long long pos = player_music_find(player);
    if (pos < 0) return;
    struct player_music *pm = &g_music_ctx.online_players[pos];
    pm->paused = false;
    if (arrlen(pm->playlist) > 0)
        pm->playlist[pm->current_track].start_ms = 0;
}

void music_player_query_playlist(void *player) { (void)player; }
void music_player_on_join(void *player) { (void)player; }

void music_player_on_quit(void *player)
{
    long long pos = player_music_find(player);
    if (pos < 0) return;
    cleanup_playlist_bossbars(&g_music_ctx.online_players[pos]);
    arrfree(g_music_ctx.online_players[pos].playlist);
    arrdelswap(g_music_ctx.online_players, (int)pos);
}

/* --- Tick (called from Scheduler on server thread) --- */

static int64_t get_tick_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static bool freq_init = false;
    if (!freq_init) { QueryPerformanceFrequency(&freq); freq_init = true; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int64_t g_tick_start_ms = 0;

void music_player_tick(void)
{
    if (g_tick_start_ms == 0) g_tick_start_ms = get_tick_ms();
    int64_t now_ms = get_tick_ms() - g_tick_start_ms;

    int len = (int)arrlen(g_music_ctx.online_players);
    for (int i = 0; i < len; i++) {
        struct player_music *pm = &g_music_ctx.online_players[i];
        if (pm->paused || arrlen(pm->playlist) == 0) continue;

        struct music_queue_entry *entry = &pm->playlist[pm->current_track];
        struct song_cache_entry *song = &g_music_ctx.song_cache[entry->song_index];

        if (entry->start_ms == 0)
            entry->start_ms = now_ms;

        int64_t elapsed = now_ms - entry->start_ms;
        size_t notes_len = arrlen(song->notes);

        while (entry->cursor < notes_len && song->notes[entry->cursor].time_ms <= elapsed) {
            struct note *nt = &song->notes[entry->cursor];
            player_play_sound(pm->player, BUILTIN_INSTRUMENT[nt->instrument], nt->volume, nt->pitch);
            entry->cursor++;
        }

        // Update progress bar every tick, even during silent sections
        if (entry->bar_type != MUSIC_BAR_NOT_DISPLAY) {
            int64_t total = song->duration_ms;
            if (total == 0) total = 1;
            float progress = (float)elapsed / (float)total;
            if (progress > 1.0f) progress = 1.0f;

            if (entry->bar_type == MUSIC_BAR_BOSSBAR) {
                if (!entry->boss_bar) {
                    entry->boss_bar = boss_bar_create(pm->player, song->song_name);
                }
                if (entry->boss_bar) {
                    boss_bar_set_progress(entry->boss_bar, progress);
                    int tmin = (int)(total / 60000), tsec = (int)((total / 1000) % 60);
                    int pmin = (int)(elapsed / 60000), psec = (int)((elapsed / 1000) % 60);
                    char title_buf[300];
                    snprintf(title_buf, sizeof(title_buf),
                             MC_GREEN "%s" MC_GOLD " | " MC_AQUA "%d:%02d" MC_GRAY "/" MC_AQUA "%d:%02d",
                             song->song_name, pmin, psec, tmin, tsec);
                    boss_bar_set_title(entry->boss_bar, title_buf);
                }
            } else {
                int tmin = (int)(total / 60000), tsec = (int)((total / 1000) % 60);
                int pmin = (int)(elapsed / 60000), psec = (int)((elapsed / 1000) % 60);
                char bar[80];
                snprintf(bar, sizeof(bar),
                         MC_AQUA "%d:%02d" MC_GRAY "/" MC_AQUA "%d:%02d",
                         pmin, psec, tmin, tsec);
                player_send_popup(pm->player, bar);
            }
        }

        if (entry->cursor >= notes_len) {
            // Clean up boss bar when track ends
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
                player_music_dequeue(pm->player, pm->current_track);
                i--;
                len = (int)arrlen(g_music_ctx.online_players);
            }
        }
    }
}

#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_PLAYER_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_PLAYER_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define NUM_INSTRUMENTS 16
#define MAX_PATH_LEN 4096

static const char *BUILTIN_INSTRUMENT[NUM_INSTRUMENTS] = {
    "note.harp", "note.bassattack", "note.bd", "note.snare",
    "note.hat", "note.guitar", "note.flute", "note.bell",
    "note.chime", "note.xylobone", "note.iron_xylophone", "note.cow_bell",
    "note.didgeridoo", "note.bit", "note.banjo", "note.pling"
};

enum music_bar_type {
    MUSIC_BAR_NOT_DISPLAY = 0,
    MUSIC_BAR_POPUP,
    MUSIC_BAR_TIP,
    MUSIC_BAR_BOSSBAR,
};

enum enqueue_result {
    ENQUEUE_OK = 0,
    ENQUEUE_NO_FILES,
    ENQUEUE_INDEX_OUT_OF_RANGE,
    ENQUEUE_BAD_LOOP,
    ENQUEUE_BAD_BAR,
    ENQUEUE_FILE_ERROR,
};

enum player_op_result {
    PLAYER_OK = 0,
    PLAYER_NO_PLAYLIST,
    PLAYER_INDEX_OUT_OF_RANGE,
    PLAYER_ALREADY_PAUSED,
    PLAYER_NOT_PAUSED,
};

struct note {
    int64_t time_ms;
    int     instrument;
    float   volume;
    float   pitch;
};

struct song_cache_entry {
    char           song_name[256];
    struct note   *notes;       // stb_ds dynamic array
    int64_t        duration_ms;
};

struct music_queue_entry {
    int              song_index;   // index into g_music_ctx.song_cache
    size_t           cursor;       // index into song->notes
    int64_t          start_ms;     // tick_timer_start + cursor offset
    int64_t          pause_elapsed; // elapsed ms when paused (0 = not paused)
    int              loop;
    enum music_bar_type bar_type;
    void            *boss_bar;     // EndstoneBossBar* (unique_ptr managed)
};

struct player_music {
    void                     *player;      // Endstone Player*
    struct music_queue_entry *playlist;    // stb_ds array
    size_t                    current_track;
    bool                      paused;
};

struct music_player_ctx {
    struct song_cache_entry *song_cache;      // stb_ds array
    struct player_music     *online_players;  // stb_ds array
};

extern struct music_player_ctx g_music_ctx;

// Initialize/shutdown
void music_player_init(void);
void music_player_shutdown(void);

// Song cache
long long song_cache_parse(FILE *fp, const char *song_name);

// Player management
long long player_music_find(void *player);
enum enqueue_result player_music_enqueue(void *player, const char *nbs_file, int loop, enum music_bar_type bar);
enum player_op_result player_music_dequeue(void *player, size_t index);
enum player_op_result player_music_stop(void *player);
enum player_op_result player_music_pause(void *player);
enum player_op_result player_music_resume(void *player);
void music_player_query_playlist(void *player);

// Called from event handlers
void music_player_on_join(void *player);
void music_player_on_quit(void *player);

// Tick: called from Scheduler on server thread
void music_player_tick(void);

// File listing
int list_nbs_files(const char *dir, char ***out_names);
void free_nbs_list(char **names, int count);

// Path helpers
void path_init(const char *data_folder);
const char *path_nbs(void);
const char *path_data(void);

// Endstone API wrappers (defined in src/endstone_api.c)
FILE *fopen_utf8(const char *path, const char *mode);
void sender_send_message(void *sender, const char *msg);
void player_play_sound(void *player, const char *sound, float volume, float pitch);
void player_send_popup(void *player, const char *msg);
void player_send_tip(void *player, const char *msg);
void *boss_bar_create(void *player, const char *title);
void boss_bar_destroy(void *bb);
void boss_bar_set_progress(void *bb, float progress);
void boss_bar_set_title(void *bb, const char *title);

#endif /* ENDSTONE_MEDIAPLAYER_MUSIC_PLAYER_H */

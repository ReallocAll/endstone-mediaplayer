#include "mediaplayer/music/music_commands.h"
#include "mediaplayer/endstone_api.h"
#include "abi_helpers.h"
#include <stb_ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const music_command_usages[MUSIC_COMMAND_USAGE_COUNT] = {
    "/mpm",
    "/mpm list [filter: string]",
    "/mpm add <index: int> [loop: int] [bar: int]",
    "/mpm del <index: int>",
    "/mpm pause",
    "/mpm resume",
    "/mpm stop",
    "/mpm playlist",
    "/mpm help",
};

void music_ctx_init(struct music_ctx *ctx, void *plugin,
                    const char *data_dir)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->plugin = plugin;
    music_catalog_init(&ctx->catalog, data_dir);
    music_cache_init(&ctx->cache);
    music_engine_init(&ctx->engine);
}

void music_ctx_shutdown(struct music_ctx *ctx)
{
    music_engine_shutdown(&ctx->engine);
    music_cache_shutdown(&ctx->cache);
    memset(ctx, 0, sizeof(*ctx));
}

void music_tick(struct music_ctx *ctx)
{
    music_engine_tick(&ctx->engine, &ctx->cache);
}

void music_on_player_join(struct music_ctx *ctx, void *player)
{
    (void)ctx;
    (void)player;
}

void music_on_player_quit(struct music_ctx *ctx, void *player)
{
    music_engine_remove_player(&ctx->engine, player);
}

static const char *bar_type_name(int bar)
{
    switch (bar) {
    case MUSIC_BAR_NOT_DISPLAY: return "off";
    case MUSIC_BAR_POPUP: return "popup";
    case MUSIC_BAR_TIP: return "tip";
    case MUSIC_BAR_BOSSBAR: return "bossbar";
    default: return "?";
    }
}

static const char *loop_description(int loop)
{
    if (loop == -1) return "infinite loop";
    if (loop == 1) return "once";
    static char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d times", loop);
    return buffer;
}

static void send_usage(void *sender)
{
    sender_send_message(
        sender, MC_GRAY "───── " MC_GREEN "MediaPlayer "
        MC_AQUA "Music" MC_GRAY " ─────");
    sender_send_message(
        sender, MC_GRAY " /mpm " MC_YELLOW "list " MC_GRAY "[filter]");
    sender_send_message(
        sender, MC_GRAY " /mpm " MC_YELLOW "add "
        MC_GRAY "<index> [loop] [bar]");
    sender_send_message(
        sender, MC_GRAY " /mpm " MC_YELLOW "del " MC_GRAY "<index>");
    sender_send_message(
        sender, MC_GRAY " /mpm " MC_YELLOW "pause" MC_GRAY " | "
        MC_YELLOW "resume" MC_GRAY " | " MC_YELLOW "stop");
    sender_send_message(
        sender, MC_GRAY " /mpm " MC_YELLOW "playlist");
    sender_send_message(
        sender, MC_GRAY "── " MC_AQUA "Bar" MC_GRAY
        " ──  0=off  1=popup  2=tip  3=bossbar");
    sender_send_message(
        sender, MC_GRAY "── " MC_AQUA "Loop" MC_GRAY
        " ──  -1=infinite  1=once  N=times");
}

static void command_list(struct music_ctx *ctx, void *sender,
                         int argc, const char **argv)
{
    char **files = nullptr;
    int count = music_catalog_list(&ctx->catalog, &files);
    if (count == 0) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY
            "No .nbs files in data folder");
        return;
    }

    const char *filter = argc >= 2 ? argv[1] : nullptr;
    int matched = 0;
    char message[512];
    for (int i = 0; i < count; i++) {
        if (filter && !strstr(files[i], filter)) continue;
        snprintf(message, sizeof(message),
                 MC_GREEN "[MediaPlayer] " MC_GRAY "[%d] "
                 MC_YELLOW "%s", i, files[i]);
        sender_send_message(sender, message);
        matched++;
    }
    if (matched == 0) {
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "No matches for " MC_YELLOW "'%s'", filter);
        sender_send_message(sender, message);
    } else if (filter) {
        snprintf(message, sizeof(message),
                 MC_GREEN "[MediaPlayer] " MC_GRAY
                 "%d of %d matched", matched, count);
        sender_send_message(sender, message);
    }
    music_catalog_free_list(files, count);
}

static void log_enqueue_error(struct music_ctx *ctx, const char *file,
                              enum music_enqueue_result result,
                              const struct nbs_error_info *error)
{
    if (!ctx->plugin) return;

    char message[512];
    if (result == MUSIC_ENQUEUE_NBS_VERSION_ERROR) {
        snprintf(message, sizeof(message),
                 "[MediaPlayer] Unsupported NBS version in '%s': version=%d",
                 file, (int)error->actual_version);
    } else if (result == MUSIC_ENQUEUE_NBS_LIMIT_ERROR) {
        snprintf(message, sizeof(message),
                 "[MediaPlayer] NBS limit exceeded in '%s': "
                 "error=%s section=%s offset=%lld",
                 file, nbs_error_string(error->code),
                 nbs_section_string(error->section),
                 (long long)error->file_offset);
    } else {
        snprintf(message, sizeof(message),
                 "[MediaPlayer] Failed to parse '%s': "
                 "error=%s section=%s offset=%lld tick=%u layer=%u",
                 file, nbs_error_string(error->code),
                 nbs_section_string(error->section),
                 (long long)error->file_offset,
                 error->tick, error->layer);
    }
    PLUGIN_LOG(ctx->plugin, ES_LOG_INFO, message);
}

static void send_enqueue_result(struct music_ctx *ctx, void *sender,
                                const char *file, int loop, int bar,
                                enum music_enqueue_result result,
                                const struct nbs_error_info *error)
{
    char message[512];
    switch (result) {
    case MUSIC_ENQUEUE_OK:
        snprintf(message, sizeof(message),
                 MC_GREEN "[MediaPlayer] " MC_GRAY "Added " MC_YELLOW "%s"
                 MC_GRAY "  [" MC_AQUA "%s" MC_GRAY "]  ["
                 MC_AQUA "%s" MC_GRAY "]",
                 file, bar_type_name(bar), loop_description(loop));
        sender_send_message(sender, message);
        break;
    case MUSIC_ENQUEUE_BAD_LOOP:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY
            "Loop must be >= -1  (-1=infinite, 1=once, N=times)");
        break;
    case MUSIC_ENQUEUE_BAD_BAR:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY
            "Bar must be 0-3  (0=off, 1=popup, 2=tip, 3=bossbar)");
        break;
    case MUSIC_ENQUEUE_FILE_ERROR:
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "Failed to load " MC_YELLOW "%s", file);
        sender_send_message(sender, message);
        break;
    case MUSIC_ENQUEUE_NBS_VERSION_ERROR:
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "Unsupported NBS version " MC_YELLOW "%u"
                 MC_GRAY " in " MC_YELLOW "%s",
                 (unsigned int)error->actual_version, file);
        sender_send_message(sender, message);
        log_enqueue_error(ctx, file, result, error);
        break;
    case MUSIC_ENQUEUE_NBS_LIMIT_ERROR:
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "NBS file exceeds limits " MC_YELLOW "%s", file);
        sender_send_message(sender, message);
        log_enqueue_error(ctx, file, result, error);
        break;
    case MUSIC_ENQUEUE_NBS_PARSE_ERROR:
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "Failed to parse " MC_YELLOW "%s: %s",
                 file, nbs_error_string(error->code));
        sender_send_message(sender, message);
        log_enqueue_error(ctx, file, result, error);
        break;
    }
}

static void command_add(struct music_ctx *ctx, void *sender,
                        int argc, const char **argv)
{
    int index = atoi(argv[1]);
    int loop = argc >= 3 ? atoi(argv[2]) : 1;
    int bar = argc >= 4 ? atoi(argv[3]) : MUSIC_BAR_BOSSBAR;
    if (index < 0) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Index must be >= 0");
        return;
    }

    char **files = nullptr;
    int count = music_catalog_list(&ctx->catalog, &files);
    if (count == 0) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY
            "No .nbs files in data folder");
        return;
    }
    if (index >= count) {
        char message[512];
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "Index out of range " MC_YELLOW "(max %d)", count - 1);
        sender_send_message(sender, message);
        music_catalog_free_list(files, count);
        return;
    }

    struct nbs_error_info error = {0};
    enum music_enqueue_result result = music_engine_enqueue(
        &ctx->engine, &ctx->cache, &ctx->catalog, sender, files[index],
        loop, (enum music_bar_type)bar, &error);
    send_enqueue_result(ctx, sender, files[index], loop, bar, result, &error);
    music_catalog_free_list(files, count);
}

static void command_delete(struct music_ctx *ctx, void *sender,
                           const char *index_text)
{
    int index = atoi(index_text);
    if (index < 0) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Index must be >= 0");
        return;
    }

    long long position = music_engine_find(&ctx->engine, sender);
    if (position < 0) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Playlist is empty");
        return;
    }
    struct music_player *player = &ctx->engine.players[position];
    if ((size_t)index >= (size_t)arrlen(player->playlist)) {
        char message[512];
        snprintf(message, sizeof(message),
                 MC_RED "[MediaPlayer] " MC_GRAY
                 "Index out of range " MC_YELLOW "(max %d)",
                 (int)arrlen(player->playlist) - 1);
        sender_send_message(sender, message);
        return;
    }

    struct music_cache_entry *song = music_cache_get(
        &ctx->cache, player->playlist[index].song_index);
    char name[MUSIC_SONG_NAME_MAX] = {0};
    if (song)
        snprintf(name, sizeof(name), "%s", song->song_name);

    enum music_player_result result =
        music_engine_dequeue(&ctx->engine, sender, (size_t)index);
    if (result == MUSIC_PLAYER_OK) {
        char message[512];
        snprintf(message, sizeof(message),
                 MC_GREEN "[MediaPlayer] " MC_GRAY
                 "Removed " MC_YELLOW "%s", name);
        sender_send_message(sender, message);
    } else if (result == MUSIC_PLAYER_NO_PLAYLIST) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Playlist is empty");
    } else if (result == MUSIC_PLAYER_INDEX_OUT_OF_RANGE) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Index out of range");
    }
}

static void command_pause(struct music_ctx *ctx, void *sender)
{
    switch (music_engine_pause(&ctx->engine, sender)) {
    case MUSIC_PLAYER_OK:
        sender_send_message(
            sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Paused");
        break;
    case MUSIC_PLAYER_NO_PLAYLIST:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "No music playing");
        break;
    case MUSIC_PLAYER_ALREADY_PAUSED:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Already paused");
        break;
    default:
        break;
    }
}

static void command_resume(struct music_ctx *ctx, void *sender)
{
    switch (music_engine_resume(&ctx->engine, sender)) {
    case MUSIC_PLAYER_OK:
        sender_send_message(
            sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Resumed");
        break;
    case MUSIC_PLAYER_NO_PLAYLIST:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "No music playing");
        break;
    case MUSIC_PLAYER_NOT_PAUSED:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Not paused");
        break;
    default:
        break;
    }
}

static void command_stop(struct music_ctx *ctx, void *sender)
{
    switch (music_engine_stop(&ctx->engine, sender)) {
    case MUSIC_PLAYER_OK:
        sender_send_message(
            sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Stopped");
        break;
    case MUSIC_PLAYER_NO_PLAYLIST:
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "No music playing");
        break;
    default:
        break;
    }
}

static void command_playlist(struct music_ctx *ctx, void *sender)
{
    long long position = music_engine_find(&ctx->engine, sender);
    if (position < 0) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Playlist is empty");
        return;
    }

    struct music_player *player = &ctx->engine.players[position];
    int count = (int)arrlen(player->playlist);
    char message[512];
    snprintf(message, sizeof(message),
             MC_GREEN "[MediaPlayer] " MC_GRAY
             "Playlist (%d tracks)", count);
    sender_send_message(sender, message);

    for (int i = 0; i < count; i++) {
        struct music_cache_entry *song = music_cache_get(
            &ctx->cache, player->playlist[i].song_index);
        const char *name = song ? song->song_name : "?";
        if (i == (int)player->current_track) {
            snprintf(message, sizeof(message),
                     MC_GREEN "[MediaPlayer] " MC_YELLOW
                     "> [%d] %s", i, name);
        } else {
            snprintf(message, sizeof(message),
                     MC_GREEN "[MediaPlayer] " MC_GRAY
                     "  [%d] %s", i, name);
        }
        sender_send_message(sender, message);
    }
}

void music_handle_command(struct music_ctx *ctx,
                          int argc, const char **argv,
                          void *sender, void *player)
{
    if (argc < 1) {
        send_usage(sender);
        return;
    }

    const char *action = argv[0];
    if (strcmp(action, "help") == 0) {
        send_usage(sender);
        return;
    }
    if (strcmp(action, "list") == 0) {
        command_list(ctx, sender, argc, argv);
        return;
    }
    if (!player) {
        sender_send_message(
            sender, MC_RED "[MediaPlayer] " MC_GRAY "Player-only command");
        return;
    }
    if (strcmp(action, "add") == 0 && argc >= 2) {
        command_add(ctx, sender, argc, argv);
        return;
    }
    if (strcmp(action, "del") == 0 && argc >= 2) {
        command_delete(ctx, sender, argv[1]);
        return;
    }
    if (strcmp(action, "pause") == 0) {
        command_pause(ctx, sender);
        return;
    }
    if (strcmp(action, "resume") == 0) {
        command_resume(ctx, sender);
        return;
    }
    if (strcmp(action, "stop") == 0) {
        command_stop(ctx, sender);
        return;
    }
    if (strcmp(action, "playlist") == 0) {
        command_playlist(ctx, sender);
        return;
    }

    sender_send_message(
        sender, MC_RED "[MediaPlayer] " MC_GRAY
        "Unknown subcommand, use /mpm help");
}

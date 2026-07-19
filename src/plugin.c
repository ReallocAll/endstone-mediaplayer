/**
 * plugin.c — Pure C Endstone MediaPlayer plugin.
 *
 * Plugin lifecycle, command handling, event/scheduler registration.
 * ABI details are in abi_helpers.h, sfunc.c, and endstone_api.c.
 */

#include "abi_helpers.h"
#include "music_player.h"
#include "version.h"
#include <cppcompat/string.h>
#include <cppcompat/vector.h>
#include <stb_ds.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =====================================================================
 *  Plugin vtable
 * ===================================================================== */

static void plugin_destructor(void *self, int flags);
static bool plugin_on_command(void *self, void *sender,
                              const void *command, const void *args);
static const void *plugin_get_description(const void *self);
static void plugin_on_load(void *self);
static void plugin_on_enable(void *self);
static void plugin_on_disable(void *self);

#if ES_PLATFORM_LINUX
static void plugin_complete_destructor(void *self) { plugin_destructor(self, 0); }
static void plugin_deleting_destructor(void *self) { plugin_destructor(self, 1); }
#endif

static void *g_vtable[ES_VTABLE_SLOT_COUNT] = {
#if ES_PLATFORM_LINUX
    (void *)plugin_complete_destructor,
    (void *)plugin_deleting_destructor,
#else
    (void *)plugin_destructor,
#endif
    (void *)plugin_on_command,
    (void *)plugin_get_description,
    (void *)plugin_on_load,
    (void *)plugin_on_enable,
    (void *)plugin_on_disable,
};

/* =====================================================================
 *  PluginDescription helpers
 * ===================================================================== */

#define DESC_STRING(desc, off, value) \
    cpp_string_construct((desc) + (off), (value))
#define DESC_VECTOR(desc, off, esize) \
    cpp_vector_construct((desc) + (off), (esize), NULL)

static void description_init(char *desc)
{
    memset(desc, 0, ES_DESCRIPTION_SIZE);
    DESC_STRING(desc, ES_DESC_OFF_NAME,        "mediaplayer");
    DESC_STRING(desc, ES_DESC_OFF_VERSION,     MP_VERSION);
    DESC_STRING(desc, ES_DESC_OFF_FULL_NAME,   "MediaPlayer v" MP_VERSION);
    DESC_STRING(desc, ES_DESC_OFF_API_VERSION, ES_API_VERSION);
    DESC_STRING(desc, ES_DESC_OFF_DESCRIPTION, "NBS music player for Endstone");
    DESC_STRING(desc, ES_DESC_OFF_WEBSITE,     "");
    DESC_STRING(desc, ES_DESC_OFF_PREFIX,      "MediaPlayer");
    *(int32_t *)(desc + ES_DESC_OFF_LOAD) = ES_LOAD_POST_WORLD;
    *(int32_t *)(desc + ES_DESC_OFF_DEFAULT_PERM) = ES_PERM_OPERATOR;
    DESC_VECTOR(desc, ES_DESC_OFF_AUTHORS,      ES_STRING_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_CONTRIBUTORS, ES_STRING_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_PROVIDES,      ES_STRING_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_DEPEND,        ES_STRING_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_SOFT_DEPEND,   ES_STRING_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_LOAD_BEFORE,   ES_STRING_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_COMMANDS,      ES_COMMAND_SIZE);
    DESC_VECTOR(desc, ES_DESC_OFF_PERMISSIONS,   ES_PERMISSION_SIZE);
}

#undef DESC_STRING
#undef DESC_VECTOR

static void description_destroy(char *desc)
{
    cpp_string_destroy(desc + ES_DESC_OFF_NAME);
    cpp_string_destroy(desc + ES_DESC_OFF_VERSION);
    cpp_string_destroy(desc + ES_DESC_OFF_FULL_NAME);
    cpp_string_destroy(desc + ES_DESC_OFF_API_VERSION);
    cpp_string_destroy(desc + ES_DESC_OFF_DESCRIPTION);
    cpp_string_destroy(desc + ES_DESC_OFF_WEBSITE);
    cpp_string_destroy(desc + ES_DESC_OFF_PREFIX);
    cpp_vector_destroy(desc + ES_DESC_OFF_AUTHORS);
    cpp_vector_destroy(desc + ES_DESC_OFF_CONTRIBUTORS);
    cpp_vector_destroy(desc + ES_DESC_OFF_PROVIDES);
    cpp_vector_destroy(desc + ES_DESC_OFF_DEPEND);
    cpp_vector_destroy(desc + ES_DESC_OFF_SOFT_DEPEND);
    cpp_vector_destroy(desc + ES_DESC_OFF_LOAD_BEFORE);
    cpp_vector_destroy(desc + ES_DESC_OFF_COMMANDS);
    cpp_vector_destroy(desc + ES_DESC_OFF_PERMISSIONS);
}

/* =====================================================================
 *  Global plugin pointer (used by endstone_api.c for BossBar)
 * ===================================================================== */

void *g_plugin = NULL;

/* =====================================================================
 *  Command class
 * ===================================================================== */

static void  cmd_dtor(void *self, int f)        { (void)self; (void)f; }
static bool  cmd_exec(void *s, void *nd, const void *a) { (void)s;(void)nd;(void)a; return false; }
static void *cmd_as_pcmd(void *self)             { (void)self; return NULL; }

#if ES_PLATFORM_LINUX
static void cmd_complete_dtor(void *self) { cmd_dtor(self, 0); }
static void cmd_deleting_dtor(void *self) { cmd_dtor(self, 1); }
#endif

static void *g_cmd_vtable[ES_CMD_VTABLE_SLOT_COUNT] = {
#if ES_PLATFORM_LINUX
    (void *)cmd_complete_dtor, (void *)cmd_deleting_dtor,
#else
    (void *)cmd_dtor,
#endif
    (void *)cmd_exec, (void *)cmd_as_pcmd,
};

static void command_init(char *cmd, const char *name, const char *desc)
{
    memset(cmd, 0, ES_COMMAND_SIZE);
    *(void **)cmd = g_cmd_vtable;
    cpp_string_construct(cmd + ES_COMMAND_OFF_NAME, name);
    cpp_string_construct(cmd + ES_COMMAND_OFF_DESC, desc);
}

/* =====================================================================
 *  String vector helpers
 * ===================================================================== */

static void setup_string_vector(char *vec_storage, char (*strs)[ES_STRING_SIZE],
                                const char **values, int count)
{
    for (int i = 0; i < count; i++)
        cpp_string_construct(strs[i], values[i]);
    VEC_INIT(vec_storage, strs[0], count, ES_STRING_SIZE);
}

static int read_string_vector(const void *vec, const char **out, int max)
{
    char *begin = (char *)*(void *const *)vec;
    char *end   = (char *)*(void *const *)((const char *)vec + 8);
    if (!begin || begin == end) return 0;
    int count = (int)((end - begin) / ES_STRING_SIZE);
    if (count > max) count = max;
    for (int i = 0; i < count; i++)
        out[i] = cpp_string_str(begin + (i * ES_STRING_SIZE));
    return count;
}

/* =====================================================================
 *  Command handler: /mpm
 * ===================================================================== */

static const char *bar_type_name(int bar)
{
    switch (bar) {
    case MUSIC_BAR_NOT_DISPLAY: return "off";
    case MUSIC_BAR_POPUP:       return "popup";
    case MUSIC_BAR_TIP:         return "tip";
    case MUSIC_BAR_BOSSBAR:     return "bossbar";
    default:                    return "?";
    }
}

static const char *loop_desc(int loop)
{
    if (loop == -1) return "infinite loop";
    if (loop == 1)  return "once";
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d times", loop);
    return buf;
}

static void print_usage(void *sender)
{
    sender_send_message(sender, MC_GRAY "───── " MC_GREEN "MediaPlayer" MC_GRAY " ─────");
    sender_send_message(sender, MC_GRAY " /mpm " MC_YELLOW "list " MC_GRAY "[filter]");
    sender_send_message(sender, MC_GRAY " /mpm " MC_YELLOW "add " MC_GRAY "<index> [loop] [bar]");
    sender_send_message(sender, MC_GRAY " /mpm " MC_YELLOW "del " MC_GRAY "<index>");
    sender_send_message(sender, MC_GRAY " /mpm " MC_YELLOW "pause" MC_GRAY " | " MC_YELLOW "resume" MC_GRAY " | " MC_YELLOW "stop");
    sender_send_message(sender, MC_GRAY " /mpm " MC_YELLOW "playlist");
    sender_send_message(sender, MC_GRAY "── " MC_AQUA "Bar" MC_GRAY " ──  " MC_GRAY "0=off  1=popup  2=tip  3=bossbar");
    sender_send_message(sender, MC_GRAY "── " MC_AQUA "Loop" MC_GRAY " ──  " MC_GRAY "-1=infinite  1=once  N=times");
}

static void handle_mpm(void *self, void *sender, int argc, const char *argv[])
{
    if (argc < 1) { print_usage(sender); return; }
    const char *action = argv[0];
    char msg[512];

    if (strcmp(action, "help") == 0) { print_usage(sender); return; }

    /* ── list (works from console) ── */
    if (strcmp(action, "list") == 0) {
        char **files = NULL;
        int count = list_nbs_files(path_nbs(), &files);
        if (count == 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "No .nbs files in data folder");
            return;
        }
        const char *filter = (argc >= 2) ? argv[1] : NULL;
        int matched = 0;
        for (int i = 0; i < count; i++) {
            if (filter && !strstr(files[i], filter)) continue;
            snprintf(msg, sizeof(msg),
                     MC_GREEN "[MediaPlayer] " MC_GRAY "[%d] " MC_YELLOW "%s",
                     i, files[i]);
            sender_send_message(sender, msg);
            matched++;
        }
        if (matched == 0) {
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "No matches for " MC_YELLOW "'%s'",
                     filter);
            sender_send_message(sender, msg);
        } else if (filter) {
            snprintf(msg, sizeof(msg),
                     MC_GREEN "[MediaPlayer] " MC_GRAY "%d of %d matched",
                     matched, count);
            sender_send_message(sender, msg);
        }
        free_nbs_list(files, count);
        return;
    }

    /* ── player-only commands ── */
    if (!VCALL0(sender, ES_SENDER_SLOT_AS_PLAYER, void *)) {
        sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Player-only command");
        return;
    }

    /* ── add ── */
    if (strcmp(action, "add") == 0 && argc >= 2) {
        int idx  = atoi(argv[1]);
        int loop = (argc >= 3) ? atoi(argv[2]) : 1;
        int bar  = (argc >= 4) ? atoi(argv[3]) : MUSIC_BAR_BOSSBAR;

        if (idx < 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Index must be >= 0");
            return;
        }

        char **files = NULL;
        int count = list_nbs_files(path_nbs(), &files);
        if (count == 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "No .nbs files in data folder");
            return;
        }
        if (idx >= count) {
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "Index out of range " MC_YELLOW "(max %d)",
                     count - 1);
            sender_send_message(sender, msg);
            free_nbs_list(files, count);
            return;
        }

        struct nbs_error_info err;
        enum enqueue_result r = player_music_enqueue(sender, files[idx], loop,
                                                      (enum music_bar_type)bar, &err);
        switch (r) {
        case ENQUEUE_OK:
            snprintf(msg, sizeof(msg),
                     MC_GREEN "[MediaPlayer] " MC_GRAY "Added " MC_YELLOW "%s"
                     MC_GRAY "  [" MC_AQUA "%s" MC_GRAY "]  [" MC_AQUA "%s" MC_GRAY "]",
                     files[idx], bar_type_name(bar), loop_desc(loop));
            sender_send_message(sender, msg);
            break;
        case ENQUEUE_BAD_LOOP:
            sender_send_message(sender,
                                MC_RED "[MediaPlayer] " MC_GRAY "Loop must be >= -1  "
                                MC_GRAY "(-1=infinite, 1=once, N=times)");
            break;
        case ENQUEUE_BAD_BAR:
            sender_send_message(sender,
                                MC_RED "[MediaPlayer] " MC_GRAY "Bar must be 0-3  "
                                MC_GRAY "(0=off, 1=popup, 2=tip, 3=bossbar)");
            break;
        case ENQUEUE_FILE_ERROR:
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "Failed to load " MC_YELLOW "%s",
                     files[idx]);
            sender_send_message(sender, msg);
            break;
        case ENQUEUE_NBS_VERSION_ERROR:
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "Unsupported NBS version " MC_YELLOW "%s",
                     files[idx]);
            sender_send_message(sender, msg);
            if (self) {
                char logmsg[512];
                snprintf(logmsg, sizeof(logmsg),
                    "[MediaPlayer] Unsupported NBS version in '%s': version=%d",
                    files[idx], (int)err.tick);
                PLUGIN_LOG(self, ES_LOG_INFO, logmsg);
            }
            break;
        case ENQUEUE_NBS_LIMIT_ERROR:
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "NBS file exceeds limits " MC_YELLOW "%s",
                     files[idx]);
            sender_send_message(sender, msg);
            if (self) {
                char logmsg[512];
                snprintf(logmsg, sizeof(logmsg),
                    "[MediaPlayer] NBS limit exceeded in '%s': error=%s section=%s offset=%lld",
                    files[idx], nbs_error_string(err.code), nbs_section_string(err.section),
                    (long long)err.file_offset);
                PLUGIN_LOG(self, ES_LOG_INFO, logmsg);
            }
            break;
        case ENQUEUE_NBS_PARSE_ERROR:
        default:
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "Failed to parse " MC_YELLOW "%s: %s",
                     files[idx], nbs_error_string(err.code));
            sender_send_message(sender, msg);
            if (self) {
                char logmsg[512];
                snprintf(logmsg, sizeof(logmsg),
                    "[MediaPlayer] Failed to parse '%s': error=%s section=%s offset=%lld tick=%u layer=%u",
                    files[idx], nbs_error_string(err.code), nbs_section_string(err.section),
                    (long long)err.file_offset, err.tick, err.layer);
                PLUGIN_LOG(self, ES_LOG_INFO, logmsg);
            }
            break;
        }
        free_nbs_list(files, count);
        return;
    }

    /* ── del ── */
    if (strcmp(action, "del") == 0 && argc >= 2) {
        int idx = atoi(argv[1]);
        if (idx < 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Index must be >= 0");
            return;
        }

        long long pos = player_music_find(sender);
        if (pos < 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Playlist is empty");
            return;
        }
        struct player_music *pm = &g_music_ctx.online_players[pos];
        if ((size_t)idx >= (size_t)arrlen(pm->playlist)) {
            snprintf(msg, sizeof(msg),
                     MC_RED "[MediaPlayer] " MC_GRAY "Index out of range " MC_YELLOW "(max %d)",
                     (int)arrlen(pm->playlist) - 1);
            sender_send_message(sender, msg);
            return;
        }
        const char *name = g_music_ctx.song_cache[pm->playlist[idx].song_index].song_name;
        char name_buf[256];
        strncpy(name_buf, name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';

        enum player_op_result r = player_music_dequeue(sender, (size_t)idx);
        switch (r) {
        case PLAYER_OK:
            snprintf(msg, sizeof(msg),
                     MC_GREEN "[MediaPlayer] " MC_GRAY "Removed " MC_YELLOW "%s", name_buf);
            sender_send_message(sender, msg);
            break;
        case PLAYER_NO_PLAYLIST:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Playlist is empty");
            break;
        case PLAYER_INDEX_OUT_OF_RANGE:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Index out of range");
            break;
        default:
            break;
        }
        return;
    }

    /* ── pause ── */
    if (strcmp(action, "pause") == 0) {
        switch (player_music_pause(sender)) {
        case PLAYER_OK:
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Paused");
            break;
        case PLAYER_NO_PLAYLIST:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "No music playing");
            break;
        case PLAYER_ALREADY_PAUSED:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Already paused");
            break;
        default:
            break;
        }
        return;
    }

    /* ── resume ── */
    if (strcmp(action, "resume") == 0) {
        switch (player_music_resume(sender)) {
        case PLAYER_OK:
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Resumed");
            break;
        case PLAYER_NO_PLAYLIST:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "No music playing");
            break;
        case PLAYER_NOT_PAUSED:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Not paused");
            break;
        default:
            break;
        }
        return;
    }

    /* ── stop ── */
    if (strcmp(action, "stop") == 0) {
        switch (player_music_stop(sender)) {
        case PLAYER_OK:
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Stopped");
            break;
        case PLAYER_NO_PLAYLIST:
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "No music playing");
            break;
        default:
            break;
        }
        return;
    }

    /* ── playlist ── */
    if (strcmp(action, "playlist") == 0) {
        long long pos = player_music_find(sender);
        if (pos < 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Playlist is empty");
            return;
        }
        struct player_music *pm = &g_music_ctx.online_players[pos];
        int len = (int)arrlen(pm->playlist);
        snprintf(msg, sizeof(msg),
                 MC_GREEN "[MediaPlayer] " MC_GRAY "Playlist (%d tracks)", len);
        sender_send_message(sender, msg);
        for (int i = 0; i < len; i++) {
            const char *name = g_music_ctx.song_cache[pm->playlist[i].song_index].song_name;
            if (i == (int)pm->current_track)
                snprintf(msg, sizeof(msg),
                         MC_GREEN "[MediaPlayer] " MC_YELLOW "> [%d] %s", i, name);
            else
                snprintf(msg, sizeof(msg),
                         MC_GREEN "[MediaPlayer] " MC_GRAY "  [%d] %s", i, name);
            sender_send_message(sender, msg);
        }
        return;
    }

    sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Unknown subcommand, use /mpm help");
}

/* =====================================================================
 *  Event handlers
 * ===================================================================== */

static void on_player_join(void *event)
{
    void *player = *(void **)((char *)event + ES_PLAYER_EVENT_OFF_PLAYER);
    if (player) music_player_on_join(player);
}
static void on_player_quit(void *event)
{
    void *player = *(void **)((char *)event + ES_PLAYER_EVENT_OFF_PLAYER);
    if (player) music_player_on_quit(player);
}

/* =====================================================================
 *  Event & scheduler registration
 * ===================================================================== */

static void plugin_register_event(void *self, const char *event_name,
                                  void *handler, int priority)
{
    void *server = PLUGIN_SERVER(self);
    if (!server) return;
    void *pm = VCALL0(server, ES_SERVER_SLOT_GET_PLUGIN_MANAGER, void *);
    if (!pm) return;

    func_impl_t *impl = sfunc_alloc(handler, false);
    if (!impl) return;

    _Alignas(8) unsigned char event_str[ES_STRING_SIZE];
    cpp_string_construct(event_str, event_name);
    _Alignas(8) unsigned char std_fn[ES_STD_FUNCTION_SIZE];
    SFUNC_BUILD(std_fn, impl);
    VCALL5(pm, ES_PM_SLOT_REGISTER_EVENT, void,
           void *, event_str, void *, std_fn, int, priority, void *, self, int, 0);
    cpp_string_destroy(event_str);
}

static void scheduler_register_tick(void *self)
{
    void *server = PLUGIN_SERVER(self);
    if (!server) return;
    void *scheduler = VCALL0(server, ES_SERVER_SLOT_GET_SCHEDULER, void *);
    if (!scheduler) return;

    func_impl_t *impl = sfunc_alloc((void *)music_player_tick, true);
    if (!impl) return;

    _Alignas(8) unsigned char std_fn[ES_STD_FUNCTION_SIZE];
    SFUNC_BUILD(std_fn, impl);

    _Alignas(8) unsigned char result[16] = {0};
    VCALL5(scheduler, ES_SCHEDULER_SLOT_RUN_TIMER, void *,
           void *, result, void *, self, void *, std_fn, uint64_t, 0ULL, uint64_t, 1ULL);
}

/* =====================================================================
 *  Plugin vtable implementations
 * ===================================================================== */

static void plugin_destructor(void *self, int flags)
{
    description_destroy((char *)self + ES_PLUGIN_OFF_DESCRIPTION);
    if (flags & 1) free(self);
}

static bool plugin_on_command(void *self, void *sender,
                              const void *command, const void *args)
{
    const char *cmd_name = cpp_string_str((char *)command + ES_COMMAND_OFF_NAME);
    if (strcmp(cmd_name, "mpm") == 0) {
        const char *arg_strs[16];
        int argc = read_string_vector(args, arg_strs, 16);
        handle_mpm(self, sender, argc, arg_strs);
        return true;
    }
    return false;
}

static const void *plugin_get_description(const void *self)
{
    return (const char *)self + ES_PLUGIN_OFF_DESCRIPTION;
}

static void plugin_on_load(void *self)    { (void)self; }

static void plugin_on_enable(void *self)
{
    g_plugin = self;
    music_player_init();

    char data_path[4096];
    snprintf(data_path, sizeof(data_path), "plugins/endstone_mediaplayer");
    path_init(data_path);

    PLUGIN_LOG(self, ES_LOG_INFO, "MediaPlayer v" MP_VERSION " enabled!");
    PLUGIN_LOG(self, ES_LOG_INFO, "Use /mpm help for commands");

    plugin_register_event(self, "PlayerJoinEvent",
                          (void *)on_player_join, ES_PRIORITY_NORMAL);
    plugin_register_event(self, "PlayerQuitEvent",
                          (void *)on_player_quit, ES_PRIORITY_NORMAL);
    scheduler_register_tick(self);
}

static void plugin_on_disable(void *self)
{
    PLUGIN_LOG(self, ES_LOG_INFO, "MediaPlayer disabled!");
    music_player_shutdown();
    char *desc = (char *)self + ES_PLUGIN_OFF_DESCRIPTION;
    memset(desc + ES_DESC_OFF_COMMANDS, 0, ES_VECTOR_SIZE);
    memset(desc + ES_DESC_OFF_PERMISSIONS, 0, ES_VECTOR_SIZE);
}

/* =====================================================================
 *  Entry point
 * ===================================================================== */

_Alignas(8) static char g_commands[1][ES_COMMAND_SIZE];
_Alignas(8) static char g_mpm_usages[9][ES_STRING_SIZE];

ES_EXPORT void *init_endstone_plugin(void)
{
    char *obj = (char *)calloc(1, ES_PLUGIN_IMPL_SIZE);
    if (!obj) return NULL;

    *(void **)obj = g_vtable;
    description_init((char *)obj + ES_PLUGIN_OFF_DESCRIPTION);

    command_init(g_commands[0], "mpm", "NBS music player");
    {
        const char *usages[] = {
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
        setup_string_vector(g_commands[0] + ES_COMMAND_OFF_USAGES,
                            g_mpm_usages, usages, 9);
    }

    VEC_INIT((char *)obj + ES_PLUGIN_OFF_DESCRIPTION + ES_DESC_OFF_COMMANDS,
             g_commands[0], 1, ES_COMMAND_SIZE);

    return obj;
}

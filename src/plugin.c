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

static void *g_vtable[ES_VTABLE_SLOT_COUNT] = {
    (void *)plugin_destructor,
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
    cpp_vector_construct((desc) + (off), (esize), nullptr)

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
    DESC_VECTOR(desc, ES_DESC_OFF_AUTHORS,      32);
    DESC_VECTOR(desc, ES_DESC_OFF_CONTRIBUTORS,  32);
    DESC_VECTOR(desc, ES_DESC_OFF_PROVIDES,      32);
    DESC_VECTOR(desc, ES_DESC_OFF_DEPEND,        32);
    DESC_VECTOR(desc, ES_DESC_OFF_SOFT_DEPEND,   32);
    DESC_VECTOR(desc, ES_DESC_OFF_LOAD_BEFORE,   32);
    DESC_VECTOR(desc, ES_DESC_OFF_COMMANDS,      152);
    DESC_VECTOR(desc, ES_DESC_OFF_PERMISSIONS,   144);
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

void *g_plugin = nullptr;

/* =====================================================================
 *  Command class
 * ===================================================================== */

static void  cmd_dtor(void *self, int f)        { (void)self; (void)f; }
static bool  cmd_exec(void *s, void *nd, const void *a) { (void)s;(void)nd;(void)a; return false; }
static void *cmd_as_pcmd(void *self)             { (void)self; return nullptr; }

static void *g_cmd_vtable[ES_CMD_VTABLE_SLOT_COUNT] = {
    (void *)cmd_dtor, (void *)cmd_exec, (void *)cmd_as_pcmd,
};

static void command_init(char *cmd, const char *name, const char *desc)
{
    memset(cmd, 0, ES_COMMAND_SIZE);
    *(void **)cmd = g_cmd_vtable;
    cpp_string_construct(cmd + ES_COMMAND_OFF_NAME, name);
    cpp_string_construct(cmd + ES_COMMAND_OFF_DESC, desc);
}

static void command_destroy(char *cmd)
{
    cpp_string_destroy(cmd + ES_COMMAND_OFF_NAME);
    cpp_string_destroy(cmd + ES_COMMAND_OFF_DESC);
    cpp_vector_destroy(cmd + ES_COMMAND_OFF_ALIASES);
    cpp_vector_destroy(cmd + ES_COMMAND_OFF_USAGES);
    cpp_vector_destroy(cmd + ES_COMMAND_OFF_PERMS);
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
    sender_send_message(sender, MC_GOLD   "[MediaPlayer] " MC_GREEN "─── Commands ───");
    sender_send_message(sender, MC_YELLOW "/mpm list [filter]");
    sender_send_message(sender, MC_YELLOW "/mpm add <index> [loop] [bar]");
    sender_send_message(sender, MC_YELLOW "/mpm del <index>");
    sender_send_message(sender, MC_YELLOW "/mpm pause | resume | stop | playlist");
    sender_send_message(sender, MC_YELLOW "/mpm help");
    sender_send_message(sender, MC_GOLD   "[MediaPlayer] " MC_GREEN "─── Bar types ───");
    sender_send_message(sender, MC_AQUA   "  0=off  1=popup  2=tip  3=bossbar (default)");
    sender_send_message(sender, MC_GOLD   "[MediaPlayer] " MC_GREEN "─── Loop ───");
    sender_send_message(sender, MC_AQUA   "  -1=infinite  1=once  N=N times");
}

static void handle_mpm(void *sender, int argc, const char *argv[])
{
    if (argc < 1) { print_usage(sender); return; }
    const char *action = argv[0];
    char msg[512];

    if (strcmp(action, "help") == 0) { print_usage(sender); return; }

    /* ── list (works from console) ── */
    if (strcmp(action, "list") == 0) {
        char **files = nullptr;
        int count = list_nbs_files(path_nbs(), &files);
        if (count == 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] No .nbs files found!");
            return;
        }
        sender_send_message(sender, MP_TAG MC_GREEN "[Index] " MC_AQUA "Music List");
        for (int i = 0; i < count; i++) {
            if (argc >= 2 && !strstr(files[i], argv[1])) continue;
            snprintf(msg, sizeof(msg), MP_TAG MC_GREEN "[%d] " MC_AQUA "%s", i, files[i]);
            sender_send_message(sender, msg);
        }
        free_nbs_list(files, count);
        return;
    }

    /* ── player-only commands ── */
    if (!VCALL0(sender, ES_SENDER_SLOT_AS_PLAYER, void *)) {
        sender_send_message(sender, MC_RED "[MediaPlayer] This command requires a player");
        return;
    }

    /* ── add ── */
    if (strcmp(action, "add") == 0 && argc >= 2) {
        int idx  = atoi(argv[1]);
        int loop = (argc >= 3) ? atoi(argv[2]) : 1;
        int bar  = (argc >= 4) ? atoi(argv[3]) : MUSIC_BAR_BOSSBAR;

        if (idx < 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] Index must be >= 0");
            return;
        }
        if (loop < -1) {
            sender_send_message(sender, MC_RED "[MediaPlayer] Loop must be >= -1 (-1=infinite, 1=once)");
            return;
        }
        if (bar < 0 || bar > 3) {
            sender_send_message(sender, MC_RED "[MediaPlayer] Bar must be 0-3 (0=off, 1=popup, 2=tip, 3=bossbar)");
            return;
        }

        char **files = nullptr;
        int count = list_nbs_files(path_nbs(), &files);
        if (count == 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] No .nbs files!");
            return;
        }
        if (idx >= count) {
            snprintf(msg, sizeof(msg), MC_RED "[MediaPlayer] Index out of range (max %d)", count - 1);
            sender_send_message(sender, msg);
            free_nbs_list(files, count);
            return;
        }
        if (player_music_enqueue(sender, files[idx], loop, (enum music_bar_type)bar)) {
            snprintf(msg, sizeof(msg), MP_TAG MC_AQUA "Added " MC_GREEN "%s" MC_AQUA
                     "  [" MC_GREEN "%s" MC_AQUA "]  [" MC_GREEN "%s" MC_AQUA "]",
                     files[idx], bar_type_name(bar), loop_desc(loop));
            sender_send_message(sender, msg);
        } else {
            sender_send_message(sender, MC_RED "[MediaPlayer] Failed to add music");
        }
        free_nbs_list(files, count);
        return;
    }

    /* ── del ── */
    if (strcmp(action, "del") == 0 && argc >= 2) {
        int idx = atoi(argv[1]);
        if (idx < 0) {
            sender_send_message(sender, MC_RED "[MediaPlayer] Index must be >= 0");
            return;
        }
        if (player_music_dequeue(sender, (size_t)idx))
            sender_send_message(sender, MP_TAG "Deleted");
        else
            sender_send_message(sender, MC_RED "[MediaPlayer] Delete failed");
        return;
    }

    /* ── pause / resume / stop ── */
    if (strcmp(action, "pause") == 0) {
        player_music_pause(sender);
        sender_send_message(sender, MP_TAG "Paused");
        return;
    }
    if (strcmp(action, "resume") == 0) {
        player_music_resume(sender);
        sender_send_message(sender, MP_TAG "Resumed");
        return;
    }
    if (strcmp(action, "stop") == 0) {
        player_music_stop(sender);
        sender_send_message(sender, MP_TAG "Stopped");
        return;
    }

    /* ── playlist ── */
    if (strcmp(action, "playlist") == 0) {
        long long pos = player_music_find(sender);
        if (pos < 0) {
            sender_send_message(sender, MP_TAG "Playlist empty!");
            return;
        }
        struct player_music *pm = &g_music_ctx.online_players[pos];
        int len = (int)arrlen(pm->playlist);
        sender_send_message(sender, MP_TAG "─── Playlist ───");
        for (int i = 0; i < len; i++) {
            const char *marker = (i == (int)pm->current_track) ? " " MC_GREEN "(Current)" : "";
            const char *name = g_music_ctx.song_cache[pm->playlist[i].song_index].song_name;
            snprintf(msg, sizeof(msg), MP_TAG MC_GREEN "[%d] " MC_AQUA "%s%s", i, name, marker);
            sender_send_message(sender, msg);
        }
        return;
    }

    sender_send_message(sender, MC_RED "[MediaPlayer] Unknown subcommand. Use /mpm help");
}

/* =====================================================================
 *  Event handlers
 * ===================================================================== */

static void on_player_join(void *event)
{
    void *player = *(void **)((char *)event + 16);
    if (player) music_player_on_join(player);
}
static void on_player_quit(void *event)
{
    void *player = *(void **)((char *)event + 16);
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

    char event_str[ES_STRING_SIZE];
    cpp_string_construct(event_str, event_name);
    char std_fn[ES_STD_FUNCTION_SIZE];
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

    char std_fn[ES_STD_FUNCTION_SIZE];
    SFUNC_BUILD(std_fn, impl);

    char result[16] = {0};
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
    (void)self;
    if (strcmp(cmd_name, "mpm") == 0) {
        const char *arg_strs[16];
        int argc = read_string_vector(args, arg_strs, 16);
        handle_mpm(sender, argc, arg_strs);
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

static char g_commands[1][ES_COMMAND_SIZE];
static char g_mpm_usages[9][ES_STRING_SIZE];

__declspec(dllexport) void *init_endstone_plugin(void)
{
    char *obj = (char *)calloc(1, ES_PLUGIN_IMPL_SIZE);
    if (!obj) return nullptr;

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
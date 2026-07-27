/**
 * plugin.c — Pure C Endstone MediaPlayer plugin.
 *
 * Plugin lifecycle, command handling, event/scheduler registration.
 * ABI details are in abi_helpers.h, sfunc.c, and endstone_api.c.
 */

#include "abi_helpers.h"
#include "mediaplayer/endstone_api.h"
#include "mediaplayer/music/music_commands.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/video/video_commands.h"
#include "version.h"
#include <cppcompat/string.h>
#include <cppcompat/vector.h>
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
    cpp_vector_construct((desc) + (off), (esize), nullptr)

static void description_init(char *desc)
{
    memset(desc, 0, ES_DESCRIPTION_SIZE);
    DESC_STRING(desc, ES_DESC_OFF_NAME,        "mediaplayer");
    DESC_STRING(desc, ES_DESC_OFF_VERSION,     MP_VERSION);
    DESC_STRING(desc, ES_DESC_OFF_FULL_NAME,   "MediaPlayer v" MP_VERSION);
    DESC_STRING(desc, ES_DESC_OFF_API_VERSION, ES_API_VERSION);
    DESC_STRING(desc, ES_DESC_OFF_DESCRIPTION, "music & video player for Endstone");
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

void *g_plugin = nullptr;
static struct music_ctx g_music_ctx;
static struct video_ctx g_video_ctx;

/* =====================================================================
 *  Command class
 * ===================================================================== */

static void  cmd_dtor(void *self, int f)        { (void)self; (void)f; }
static bool  cmd_exec(void *s, void *nd, const void *a) { (void)s;(void)nd;(void)a; return false; }
static void *cmd_as_pcmd(void *self)             { (void)self; return nullptr; }

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
                                const char *const *values, int count)
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
 *  Event handlers
 * ===================================================================== */

static void on_player_join(void *event)
{
    void *player = *(void **)((char *)event + ES_PLAYER_EVENT_OFF_PLAYER);
    if (player) {
        music_on_player_join(&g_music_ctx, player);
        char uid[SCREEN_UUID_LEN + 1];
        if (es_player_uuid_string(player, uid))
            video_on_player_join(&g_video_ctx, player, uid);
    }
}
static void on_player_quit(void *event)
{
    void *player = *(void **)((char *)event + ES_PLAYER_EVENT_OFF_PLAYER);
    if (player) {
        music_on_player_quit(&g_music_ctx, player);
        char uid[SCREEN_UUID_LEN + 1];
        if (es_player_uuid_string(player, uid))
            video_on_player_quit(&g_video_ctx, uid);
    }
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

static void video_tick_wrapper(void)
{
    video_tick(&g_video_ctx);
}

static void music_tick_wrapper(void)
{
    music_tick(&g_music_ctx);
}

static void scheduler_register_tick(void *self)
{
    void *server = PLUGIN_SERVER(self);
    if (!server) return;
    void *scheduler = VCALL0(server, ES_SERVER_SLOT_GET_SCHEDULER, void *);
    if (!scheduler) return;

    func_impl_t *impl = sfunc_alloc((void *)music_tick_wrapper, true);
    if (!impl) return;

    _Alignas(8) unsigned char std_fn[ES_STD_FUNCTION_SIZE];
    SFUNC_BUILD(std_fn, impl);

    _Alignas(8) unsigned char result[16] = {0};
    VCALL5(scheduler, ES_SCHEDULER_SLOT_RUN_TIMER, void *,
           void *, result, void *, self, void *, std_fn, uint64_t, 0ULL, uint64_t, 1ULL);

    func_impl_t *vimpl = sfunc_alloc((void *)video_tick_wrapper, true);
    if (!vimpl) return;

    _Alignas(8) unsigned char std_fn2[ES_STD_FUNCTION_SIZE];
    SFUNC_BUILD(std_fn2, vimpl);

    _Alignas(8) unsigned char result2[16] = {0};
    VCALL5(scheduler, ES_SCHEDULER_SLOT_RUN_TIMER, void *,
           void *, result2, void *, self, void *, std_fn2, uint64_t, 0ULL, uint64_t, 1ULL);
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
        void *player = VCALL0(sender, ES_SENDER_SLOT_AS_PLAYER, void *);
        music_handle_command(&g_music_ctx, argc, arg_strs, sender, player);
        return true;
    }
    if (strcmp(cmd_name, "mpv") == 0) {
        const char *arg_strs[16];
        int argc = read_string_vector(args, arg_strs, 16);
        void *player = VCALL0(sender, ES_SENDER_SLOT_AS_PLAYER, void *);
        char uuid_buf[SCREEN_UUID_LEN + 1] = {0};
        if (player && !es_player_uuid_string(player, uuid_buf)) {
            // Refuse players without a stable identity.
            player = nullptr;
        }
        if (player) {
            // Register players after /reload.
            int found = 0;
            for (int i = 0; i < g_video_ctx.online_count; i++) {
                if (strcmp(g_video_ctx.online_players[i].uuid, uuid_buf) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found)
                video_on_player_join(&g_video_ctx, player, uuid_buf);
        }
        video_handle_command(&g_video_ctx, argc, arg_strs, sender, player,
                             player ? uuid_buf : nullptr);
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

    char data_path[4096];
    snprintf(data_path, sizeof(data_path), "plugins/endstone_mediaplayer");
    music_ctx_init(&g_music_ctx, self, data_path);

    void *server = PLUGIN_SERVER(self);
    video_ctx_init(&g_video_ctx, server, self, data_path);

    PLUGIN_LOG(self, ES_LOG_INFO, "MediaPlayer v" MP_VERSION " enabled!");
    PLUGIN_LOG(self, ES_LOG_INFO, "Use /mpm help for music, /mpv help for video");

    plugin_register_event(self, "PlayerJoinEvent",
                          (void *)on_player_join, ES_PRIORITY_NORMAL);
    plugin_register_event(self, "PlayerQuitEvent",
                          (void *)on_player_quit, ES_PRIORITY_NORMAL);
    scheduler_register_tick(self);
}

static void plugin_on_disable(void *self)
{
    PLUGIN_LOG(self, ES_LOG_INFO, "MediaPlayer disabled!");
    video_ctx_shutdown(&g_video_ctx);
    music_ctx_shutdown(&g_music_ctx);
    char *desc = (char *)self + ES_PLUGIN_OFF_DESCRIPTION;
    memset(desc + ES_DESC_OFF_COMMANDS, 0, ES_VECTOR_SIZE);
    memset(desc + ES_DESC_OFF_PERMISSIONS, 0, ES_VECTOR_SIZE);
}

/* =====================================================================
 *  Entry point
 * ===================================================================== */

_Alignas(8) static char g_commands[2][ES_COMMAND_SIZE];
_Alignas(8) static char
    g_mpm_usages[MUSIC_COMMAND_USAGE_COUNT][ES_STRING_SIZE];
_Alignas(8) static char g_mpv_usages[MPV_COMMAND_USAGE_COUNT][ES_STRING_SIZE];

ES_EXPORT void *init_endstone_plugin(void)
{
    char *obj = (char *)calloc(1, ES_PLUGIN_IMPL_SIZE);
    if (!obj) return nullptr;

    *(void **)obj = g_vtable;
    description_init((char *)obj + ES_PLUGIN_OFF_DESCRIPTION);

    command_init(g_commands[0], "mpm", "NBS music player");
    {
        setup_string_vector(g_commands[0] + ES_COMMAND_OFF_USAGES,
                            g_mpm_usages, music_command_usages,
                            MUSIC_COMMAND_USAGE_COUNT);
    }

    command_init(g_commands[1], "mpv", "Video screen player");
    {
        setup_string_vector(g_commands[1] + ES_COMMAND_OFF_USAGES,
                            g_mpv_usages, mpv_command_usages,
                            MPV_COMMAND_USAGE_COUNT);
    }

    VEC_INIT((char *)obj + ES_PLUGIN_OFF_DESCRIPTION + ES_DESC_OFF_COMMANDS,
             g_commands[0], 2, ES_COMMAND_SIZE);

    return obj;
}

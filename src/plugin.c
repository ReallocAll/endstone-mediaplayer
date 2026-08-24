// plugin.c — Pure C Endstone MediaPlayer plugin.
//
// Plugin lifecycle, command handling, event/scheduler registration.
// ABI details are in abi_helpers.h, sfunc.c, and endstone_api.c.

#include "abi_helpers.h"
#include "mediaplayer/endstone_api.h"
#include "mediaplayer/music/music_commands.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/video/video_commands.h"
#include "mediaplayer/api_provider.h"
#include "version.h"
#include <cppcompat/string.h>
#include <cppcompat/vector.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static_assert(ES_PLUGIN_OFF_DESCRIPTION + ES_DESCRIPTION_SIZE <=
                  ES_PLUGIN_IMPL_SIZE,
              "PluginDescription exceeds synthetic plugin allocation");
static_assert(ES_PLUGIN_OFF_DESCRIPTION % ES_DESCRIPTION_ALIGN == 0,
              "PluginDescription is misaligned in synthetic plugin");

// =====================================================================
//  Plugin vtable
// =====================================================================

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

static void *g_vtable_storage[
    ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_VTABLE_SLOT_COUNT] = {
#if ES_PLATFORM_LINUX
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_DTOR_COMPLETE] =
        (void *)plugin_complete_destructor,
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_DTOR_DELETING] =
        (void *)plugin_deleting_destructor,
#else
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_DTOR] =
        (void *)plugin_destructor,
#endif
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_ON_COMMAND] =
        (void *)plugin_on_command,
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_GET_DESCRIPTION] =
        (void *)plugin_get_description,
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_ON_LOAD] =
        (void *)plugin_on_load,
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_ON_ENABLE] =
        (void *)plugin_on_enable,
    [ES_PLUGIN_VTABLE_PREFIX_SLOTS + ES_PLUGIN_SLOT_ON_DISABLE] =
        (void *)plugin_on_disable,
};

#define PLUGIN_VTABLE_ADDRESS \
    (g_vtable_storage + ES_PLUGIN_VTABLE_PREFIX_SLOTS)

// =====================================================================
//  PluginDescription helpers
// =====================================================================

#define DESC_STRING(desc, off, value) \
    cpp_string_construct((desc) + (off), (value))
#define DESC_VECTOR(desc, off, esize) \
    cpp_vector_construct((desc) + (off), (esize), nullptr)
#define DESC_EMPTY_VECTOR(desc, off) \
    cpp_vector_construct((desc) + (off), 0, nullptr)

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
    // The permissions vector is always empty. Its constructor does not
    // materialize an element, so no Permission stride is part of this
    // consumer contract.
    DESC_EMPTY_VECTOR(desc, ES_DESC_OFF_PERMISSIONS);
}

#undef DESC_STRING
#undef DESC_VECTOR
#undef DESC_EMPTY_VECTOR

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

// =====================================================================
//  Global plugin pointer (used by endstone_api.c for BossBar)
// =====================================================================

void *g_plugin = nullptr;
static struct music_ctx g_music_ctx;
static struct video_ctx g_video_ctx;

// =====================================================================
//  Command class
// =====================================================================

_Alignas(void *) static unsigned char g_command_type_info_storage[128];
static const void *g_command_type_info;

static const char *type_info_raw_name(const void *type_info)
{
    if (!type_info) return nullptr;
    if (ES_TYPE_INFO_NAME_INDIRECT) {
        return es_pointer_at(type_info, ES_TYPE_INFO_OFF_NAME);
    }
    return (const char *)type_info + ES_TYPE_INFO_OFF_NAME;
}

static void cmd_dtor(void *self, int flags)
{
    (void)self;
    (void)flags;
}

#if ES_PLATFORM_WINDOWS
static void cmd_get_class_info(const void *self, void *result)
{
    (void)self;
    es_store_pointer(result, 0, (void *)g_command_type_info);
}
#else
static const void *cmd_get_class_info(const void *self)
{
    (void)self;
    return g_command_type_info;
}
#endif

static bool cmd_is_instance_of(const void *self, const void *target)
{
    (void)self;
    const char *target_name = type_info_raw_name(target);
    return g_command_type_info && target_name &&
           strcmp(target_name, ES_COMMAND_TYPEINFO_NAME) == 0;
}

static bool cmd_exec(void *self, const void *sender, const void *args)
{
    (void)self;
    (void)sender;
    (void)args;
    return false;
}

#if ES_PLATFORM_LINUX
static void cmd_complete_dtor(void *self) { cmd_dtor(self, 0); }
static void cmd_deleting_dtor(void *self) { cmd_dtor(self, 1); }
#endif

static void *g_cmd_vtable_storage[
    ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_CMD_VTABLE_SLOT_COUNT] = {
#if ES_PLATFORM_LINUX
    [ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_COMMAND_SLOT_DTOR_COMPLETE] =
        (void *)cmd_complete_dtor,
    [ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_COMMAND_SLOT_DTOR_DELETING] =
        (void *)cmd_deleting_dtor,
#else
    [ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_COMMAND_SLOT_DTOR] =
        (void *)cmd_dtor,
#endif
    [ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_COMMAND_SLOT_GET_CLASS_TYPE_ID] =
        (void *)cmd_get_class_info,
    [ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_COMMAND_SLOT_IS_INSTANCE_OF] =
        (void *)cmd_is_instance_of,
    [ES_COMMAND_VTABLE_PREFIX_SLOTS + ES_COMMAND_SLOT_EXECUTE] =
        (void *)cmd_exec,
};

#define COMMAND_VTABLE_ADDRESS \
    (g_cmd_vtable_storage + ES_COMMAND_VTABLE_PREFIX_SLOTS)

static void command_init(char *cmd, const char *name, const char *desc)
{
    memset(cmd, 0, ES_COMMAND_SIZE);
    *(void **)cmd = COMMAND_VTABLE_ADDRESS;
    cpp_string_construct(cmd + ES_COMMAND_OFF_NAME, name);
    cpp_string_construct(cmd + ES_COMMAND_OFF_DESC, desc);
}

// =====================================================================
//  String vector helpers
// =====================================================================

static void setup_string_vector(char *vec_storage, char (*strs)[ES_STRING_SIZE],
                                const char *const *values, int count)
{
    for (int i = 0; i < count; i++)
        cpp_string_construct(strs[i], values[i]);
    VEC_INIT(vec_storage, strs[0], count, ES_STRING_SIZE);
}

static int read_string_vector(const void *vec, const char **out, int max)
{
    char *begin = es_pointer_at(vec, ES_VECTOR_OFF_BEGIN);
    char *end = es_pointer_at(vec, ES_VECTOR_OFF_END);
    if (!begin || begin == end) return 0;
    int count = (int)((end - begin) / ES_STRING_SIZE);
    if (count > max) count = max;
    for (int i = 0; i < count; i++)
        out[i] = cpp_string_str(begin + (i * ES_STRING_SIZE));
    return count;
}

// =====================================================================
//  Event handlers
// =====================================================================

static void register_player(void *player)
{
    if (!player) return;
    music_on_player_join(&g_music_ctx, player);
    char uid[SCREEN_UUID_LEN + 1];
    if (es_player_uuid_string(player, uid))
        video_on_player_join(&g_video_ctx, player, uid);
}

static void on_player_join(void *event)
{
    void *player = es_shared_object(
        (char *)event + ES_PLAYER_EVENT_OFF_PLAYER +
        ES_NOTNULL_PLAYER_OFF_SHARED_PTR);
    register_player(player);
}
static void on_player_quit(void *event)
{
    void *player = es_shared_object(
        (char *)event + ES_PLAYER_EVENT_OFF_PLAYER +
        ES_NOTNULL_PLAYER_OFF_SHARED_PTR);
    if (player) {
        music_on_player_quit(&g_music_ctx, player);
        char uid[SCREEN_UUID_LEN + 1];
        if (es_player_uuid_string(player, uid))
            video_on_player_quit(&g_video_ctx, uid);
    }
}

// =====================================================================
//  Event & scheduler registration
// =====================================================================

static void plugin_register_event(void *self, const char *event_name,
                                  void *handler, int priority)
{
    void *server = PLUGIN_SERVER(self);
    if (!server) return;
    void *pm = VCALL0(server, ES_SERVER_SLOT_GET_PLUGIN_MANAGER, void *);
    if (!pm) return;

    struct func_impl *impl = sfunc_alloc(handler, false);
    if (!impl) return;

    _Alignas(8) unsigned char event_str[ES_STRING_SIZE];
    cpp_string_construct(event_str, event_name);
    _Alignas(8) unsigned char std_fn[ES_STD_FUNCTION_SIZE];
    SFUNC_BUILD(std_fn, impl);
    STR_GUARD(event_str,
        VCALL5(pm, ES_PM_SLOT_REGISTER_EVENT, void,
               void *, event_str, void *, std_fn, int, priority,
               void *, self, bool, false));
    sfunc_after_by_value(std_fn);
}

static void video_tick_wrapper(void)
{
    video_tick(&g_video_ctx);
}

static void music_tick_wrapper(void)
{
    music_tick(&g_music_ctx);
}

static void scheduler_schedule(void *scheduler, void *plugin,
                               struct func_impl *impl)
{
    _Alignas(8) unsigned char function[ES_STD_FUNCTION_SIZE];
    struct es_shared_handle task = {0};
    SFUNC_BUILD(function, impl);
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *, void *, void *, uint64_t, uint64_t))
        VTABLE(scheduler)[ES_SCHEDULER_SLOT_RUN_TIMER])(
            scheduler, &task, plugin, function, 0ULL, 1ULL);
#else
    ((void (*)(void *, void *, void *, void *, uint64_t, uint64_t))
        VTABLE(scheduler)[ES_SCHEDULER_SLOT_RUN_TIMER])(
            &task, scheduler, plugin, function, 0ULL, 1ULL);
#endif
    sfunc_after_by_value(function);
    es_shared_release(&task);
}

static void scheduler_register_tick(void *self)
{
    void *server = PLUGIN_SERVER(self);
    if (!server) return;
    void *scheduler = VCALL0(server, ES_SERVER_SLOT_GET_SCHEDULER, void *);
    if (!scheduler) return;

    struct func_impl *impl = sfunc_alloc((void *)music_tick_wrapper, true);
    if (!impl) return;

    scheduler_schedule(scheduler, self, impl);

    struct func_impl *vimpl = sfunc_alloc((void *)video_tick_wrapper, true);
    if (!vimpl) return;

    scheduler_schedule(scheduler, self, vimpl);
}

// =====================================================================
//  Plugin vtable implementations
// =====================================================================

static void plugin_destructor(void *self, int flags)
{
    description_destroy((char *)self + ES_PLUGIN_OFF_DESCRIPTION);
    if (flags & 1) free(self);
}

static bool plugin_on_command(void *self, void *sender,
                              const void *command, const void *args)
{
    const struct es_shared_handle *sender_handle = sender;
    void *sender_object = es_shared_object(sender_handle);
    if (!sender_object) return false;
    void *server = PLUGIN_SERVER(self);
    void *player = server_player_from_sender(server, sender_handle);
    const char *cmd_name = cpp_string_str((char *)command + ES_COMMAND_OFF_NAME);
    if (strcmp(cmd_name, "mpm") == 0) {
        const char *arg_strs[16];
        int argc = read_string_vector(args, arg_strs, 16);
        music_handle_command(&g_music_ctx, argc, arg_strs,
                             sender_object, player);
        return true;
    }
    if (strcmp(cmd_name, "mpv") == 0) {
        const char *arg_strs[16];
        int argc = read_string_vector(args, arg_strs, 16);
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
        video_handle_command(&g_video_ctx, argc, arg_strs,
                             sender_object, player,
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
    video_ctx_init(&g_video_ctx, server, self, data_path,
                   &g_music_ctx.catalog, &g_music_ctx.cache);
    mp_api_provider_activate(&g_video_ctx);

    PLUGIN_LOG(self, ES_LOG_INFO, "MediaPlayer v" MP_VERSION " enabled!");
    PLUGIN_LOG(self, ES_LOG_INFO, "Use /mpm help for music, /mpv help for video");

    plugin_register_event(self, "PlayerJoinEvent",
                          (void *)on_player_join, ES_PRIORITY_NORMAL);
    plugin_register_event(self, "PlayerQuitEvent",
                          (void *)on_player_quit, ES_PRIORITY_NORMAL);

    // PlayerJoinEvent is not replayed for players who remain connected across
    // /reload, so seed both media contexts from the server's live roster.
    void *online_players[64];
    int online_count = server_get_online_players(
        server, online_players,
        (int)(sizeof(online_players) / sizeof(online_players[0])));
    for (int i = 0; i < online_count; i++)
        register_player(online_players[i]);

    scheduler_register_tick(self);
}

static void plugin_on_disable(void *self)
{
    PLUGIN_LOG(self, ES_LOG_INFO, "MediaPlayer disabled!");
    mp_api_provider_deactivate(&g_video_ctx);
    video_ctx_shutdown(&g_video_ctx);
    music_ctx_shutdown(&g_music_ctx);
    char *desc = (char *)self + ES_PLUGIN_OFF_DESCRIPTION;
    memset(desc + ES_DESC_OFF_COMMANDS, 0, ES_VECTOR_SIZE);
    memset(desc + ES_DESC_OFF_PERMISSIONS, 0, ES_VECTOR_SIZE);
}

// =====================================================================
//  Entry point
// =====================================================================

_Alignas(8) static char g_commands[2][ES_COMMAND_SIZE];
_Alignas(8) static char
    g_mpm_usages[MUSIC_COMMAND_USAGE_COUNT][ES_STRING_SIZE];
_Alignas(8) static char g_mpv_usages[MPV_COMMAND_USAGE_COUNT][ES_STRING_SIZE];

ES_EXPORT void *init_endstone_plugin(void)
{
    char *obj = (char *)calloc(1, ES_PLUGIN_IMPL_SIZE);
    if (!obj) return nullptr;

    void *image_base = endstone_expected_image_base();
    if (!image_base) {
        free(obj);
        return nullptr;
    }
    g_command_type_info =
        g_command_type_info_storage;
    const void *item_type_info =
        (const char *)image_base + ES_ITEM_TYPE_TYPEINFO_RVA;
    memset(g_command_type_info_storage, 0,
           sizeof(g_command_type_info_storage));
    es_store_pointer(g_command_type_info_storage, 0,
                     es_pointer_at(item_type_info, 0));
    if (ES_TYPE_INFO_NAME_INDIRECT) {
        es_store_pointer(g_command_type_info_storage,
                         ES_TYPE_INFO_OFF_NAME,
                         (void *)ES_COMMAND_TYPEINFO_NAME);
    }
    else {
        size_t name_size = sizeof(ES_COMMAND_TYPEINFO_NAME);
        if (ES_TYPE_INFO_OFF_NAME + name_size >
            sizeof(g_command_type_info_storage)) {
            free(obj);
            return nullptr;
        }
        memcpy(g_command_type_info_storage + ES_TYPE_INFO_OFF_NAME,
               ES_COMMAND_TYPEINFO_NAME, name_size);
    }
#if ES_PLATFORM_LINUX
    if (ES_COMMAND_VTABLE_PREFIX_SLOTS >= 2) {
        g_cmd_vtable_storage[ES_COMMAND_VTABLE_PREFIX_SLOTS - 2] = nullptr;
        g_cmd_vtable_storage[ES_COMMAND_VTABLE_PREFIX_SLOTS - 1] =
            (void *)g_command_type_info;
    }
#endif
    *(void **)obj = PLUGIN_VTABLE_ADDRESS;
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

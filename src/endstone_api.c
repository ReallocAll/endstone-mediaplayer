#include "abi_helpers.h"
#include "music_player.h"
#include <cppcompat/string.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ES_PLATFORM_WINDOWS
#include <windows.h>
#endif

extern void *g_plugin;

FILE *fopen_utf8(const char *path, const char *mode)
{
#if ES_PLATFORM_WINDOWS
    wchar_t wide_path[MAX_PATH_LEN];
    wchar_t wide_mode[8];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, MAX_PATH_LEN);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8);
    return _wfopen(wide_path, wide_mode);
#else
    return fopen(path, mode);
#endif
}

void sender_send_message(void *sender, const char *text)
{
    _Alignas(8) unsigned char message[ES_MESSAGE_SIZE];
    if (!sender) return;
    memset(message, 0, sizeof(message));
    cpp_string_construct(message + ES_MESSAGE_OFF_STRING, text);
    *(size_t *)(message + ES_MESSAGE_OFF_INDEX) = ES_MESSAGE_STRING_INDEX;
    VCALL1(sender, ES_SENDER_SLOT_SEND_MESSAGE, void, const void *, message);
    cpp_string_destroy(message + ES_MESSAGE_OFF_STRING);
}

void player_get_location(void *player, struct es_location *out)
{
    if (!player || !out) return;
#if ES_PLATFORM_LINUX
    *out = ((struct es_location (*)(void *))
        VTABLE(player)[ES_PLAYER_SLOT_GET_LOCATION])(player);
#else
    ((void (*)(void *, struct es_location *))
        VTABLE(player)[ES_PLAYER_SLOT_GET_LOCATION])(player, out);
#endif
}

void player_play_sound(void *player, const char *sound, float volume, float pitch)
{
    struct es_location location;
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    if (!player) return;
    memset(&location, 0, sizeof(location));
    player_get_location(player, &location);
    cpp_string_construct(string, sound);
#if ES_PLATFORM_LINUX
    ((void (*)(void *, struct es_location, void *, float, float))
        VTABLE(player)[ES_PLAYER_SLOT_PLAY_SOUND])(
            player, location, string, volume, pitch);
    memset(string, 0, sizeof(string));
#else
    STR_GUARD(string,
        VCALL4(player, ES_PLAYER_SLOT_PLAY_SOUND, void,
               void *, &location, void *, string, float, volume, float, pitch));
#endif
}

void player_send_popup(void *player, const char *text)
{
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    if (!player) return;
    cpp_string_construct(string, text);
    STR_GUARD(string,
        VCALL1(player, ES_PLAYER_SLOT_SEND_POPUP, void, void *, string));
}

void player_send_tip(void *player, const char *text)
{
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    if (!player) return;
    cpp_string_construct(string, text);
    STR_GUARD(string,
        VCALL1(player, ES_PLAYER_SLOT_SEND_TIP, void, void *, string));
}

void *boss_bar_create(void *player, const char *title)
{
    void *server;
    void *boss;
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    _Alignas(8) void *result = NULL;
    if (!g_plugin || !player) return NULL;
    server = PLUGIN_SERVER(g_plugin);
    if (!server) return NULL;
    cpp_string_construct(string, title);
#if ES_PLATFORM_LINUX
    ((void (*)(void *, void *, void *, int, int))
        VTABLE(server)[ES_SERVER_SLOT_CREATE_BOSS_BAR])(
            &result, server, string, ES_BAR_COLOR_GREEN, ES_BAR_STYLE_SOLID);
    memset(string, 0, sizeof(string));
#else
    VCALL4(server, ES_SERVER_SLOT_CREATE_BOSS_BAR, void,
           void *, &result, void *, string,
           int, ES_BAR_COLOR_GREEN, int, ES_BAR_STYLE_SOLID);
    cpp_string_destroy(string);
#endif
    boss = result;
    if (boss) VCALL1(boss, ES_BOSSBAR_SLOT_ADD_PLAYER, void, void *, player);
    return boss;
}

void boss_bar_destroy(void *boss)
{
    if (!boss) return;
    VCALL1(boss, ES_BOSSBAR_SLOT_SET_VISIBLE, void, bool, false);
#if ES_PLATFORM_LINUX
    ((void (*)(void *))VTABLE(boss)[ES_BOSSBAR_SLOT_DTOR])(boss);
#else
    VCALL1(boss, ES_BOSSBAR_SLOT_DTOR, void, int, 1);
#endif
}

void boss_bar_set_progress(void *boss, float progress)
{
    if (boss)
        VCALL1(boss, ES_BOSSBAR_SLOT_SET_PROGRESS, void, float, progress);
}

void boss_bar_set_title(void *boss, const char *title)
{
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    if (!boss) return;
    cpp_string_construct(string, title);
    STR_GUARD(string,
        VCALL1(boss, ES_BOSSBAR_SLOT_SET_TITLE, void, void *, string));
}

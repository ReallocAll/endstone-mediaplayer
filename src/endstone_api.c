/**
 * endstone_api.c — Endstone API wrappers for pure C.
 *
 * Player methods (playSound, sendPopup, sendTip, getLocation), BossBar helpers,
 * sendMessage, and UTF-8 file open.  All vtable dispatch goes through
 * the VCALL/STR_GUARD macros from abi_helpers.h.
 */

#include "abi_helpers.h"
#include "music_player.h"
#include <cppcompat/string.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* =====================================================================
 *  Global plugin pointer (set by plugin.c)
 * ===================================================================== */

extern void *g_plugin;

/* =====================================================================
 *  UTF-8 file open helper
 * ===================================================================== */

FILE *fopen_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wpath[MAX_PATH_LEN], wmode[8];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH_LEN);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

/* =====================================================================
 *  sendMessage
 * ===================================================================== */

void sender_send_message(void *sender, const char *msg)
{
    if (!sender) return;
    char message[ES_MESSAGE_SIZE];
    memset(message, 0, ES_MESSAGE_SIZE);
    cpp_string_construct(message + ES_MESSAGE_OFF_STRING, msg);
    *(int32_t *)(message + ES_MESSAGE_OFF_INDEX) = 1;
    VCALL1(sender, ES_SENDER_SLOT_SEND_MESSAGE, void, void *, message);
    cpp_string_destroy(message + ES_MESSAGE_OFF_STRING);
}

/* =====================================================================
 *  Player API wrappers
 * ===================================================================== */

void player_get_location(void *player, struct es_location *out)
{
    if (!player || !out) return;
    VCALL1(player, ES_PLAYER_SLOT_GET_LOCATION, void *, void *, out);
}

/* playSound takes Location + std::string BY VALUE — callee destroys the string. */
void player_play_sound(void *player, const char *sound, float volume, float pitch)
{
    if (!player) return;
    struct es_location loc;
    memset(&loc, 0, sizeof(loc));
    player_get_location(player, &loc);

    char str[ES_STRING_SIZE];
    cpp_string_construct(str, sound);
    STR_GUARD(str, VCALL4(player, ES_PLAYER_SLOT_PLAY_SOUND, void,
                          void *, &loc, void *, str,
                          float, volume, float, pitch));
}

/* sendPopup takes std::string BY VALUE. */
void player_send_popup(void *player, const char *msg)
{
    if (!player) return;
    char str[ES_STRING_SIZE];
    cpp_string_construct(str, msg);
    STR_GUARD(str, VCALL1(player, ES_PLAYER_SLOT_SEND_POPUP, void, void *, str));
}

/* sendTip takes std::string BY VALUE. */
void player_send_tip(void *player, const char *msg)
{
    if (!player) return;
    char str[ES_STRING_SIZE];
    cpp_string_construct(str, msg);
    STR_GUARD(str, VCALL1(player, ES_PLAYER_SLOT_SEND_TIP, void, void *, str));
}

/* =====================================================================
 *  BossBar helpers
 * ===================================================================== */

/* createBossBar does std::move(title) — the string is moved, not destroyed. */
void *boss_bar_create(void *player, const char *title)
{
    if (!g_plugin || !player) return nullptr;
    void *server = PLUGIN_SERVER(g_plugin);
    if (!server) return nullptr;

    char str[ES_STRING_SIZE];
    cpp_string_construct(str, title);
    char result[8] = {0};
    VCALL4(server, ES_SERVER_SLOT_CREATE_BOSS_BAR, void,
           void *, result, void *, str, int, ES_BAR_COLOR_GREEN, int, ES_BAR_STYLE_SOLID);
    void *bb = *(void **)result;
    cpp_string_destroy(str);  // string was moved — safe

    if (!bb) return nullptr;
    VCALL1(bb, ES_BOSSBAR_SLOT_ADD_PLAYER, void, void *, player);
    return bb;
}

void boss_bar_destroy(void *bb)
{
    if (!bb) return;
    VCALL1(bb, ES_BOSSBAR_SLOT_SET_VISIBLE, void, bool, false);
    VCALL1(bb, ES_BOSSBAR_SLOT_DTOR, void, int, 1);
}

void boss_bar_set_progress(void *bb, float progress)
{
    if (!bb) return;
    VCALL1(bb, ES_BOSSBAR_SLOT_SET_PROGRESS, void, float, progress);
}

void boss_bar_set_title(void *bb, const char *title)
{
    if (!bb) return;
    char str[ES_STRING_SIZE];
    cpp_string_construct(str, title);
    STR_GUARD(str, VCALL1(bb, ES_BOSSBAR_SLOT_SET_TITLE, void, void *, str));
}
#ifndef ENDSTONE_ABI_H
#define ENDSTONE_ABI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* --- ABI size constants --- */
#define ES_STRING_SIZE      32
#define ES_VECTOR_SIZE      24
#define ES_PATH_SIZE        32

/* --- Plugin object (72 bytes) --- */
#define ES_PLUGIN_SIZE            72
#define ES_PLUGIN_OFF_VPTR         0
#define ES_PLUGIN_OFF_ENABLED      8
#define ES_PLUGIN_OFF_LOADER      16
#define ES_PLUGIN_OFF_SERVER      24
#define ES_PLUGIN_OFF_LOGGER      32
#define ES_PLUGIN_OFF_DATA_FOLDER 40
#define ES_PLUGIN_OFF_DESCRIPTION 72

/* --- PluginDescription (432 bytes) --- */
#define ES_DESCRIPTION_SIZE 432

/* --- Enums --- */
#define ES_LOAD_STARTUP    0
#define ES_LOAD_POST_WORLD 1

#define ES_PERM_TRUE        0
#define ES_PERM_FALSE       1
#define ES_PERM_OPERATOR    2
#define ES_PERM_NOT_OPERATOR 3
#define ES_PERM_CONSOLE     4

/* --- EndstonePluginImpl --- */
#define ES_PLUGIN_IMPL_SIZE  (ES_PLUGIN_SIZE + ES_DESCRIPTION_SIZE)

#define ES_DESC_OFF_NAME               0
#define ES_DESC_OFF_VERSION           32
#define ES_DESC_OFF_FULL_NAME         64
#define ES_DESC_OFF_API_VERSION       96
#define ES_DESC_OFF_DESCRIPTION      128
#define ES_DESC_OFF_LOAD             160
#define ES_DESC_OFF_AUTHORS          168
#define ES_DESC_OFF_CONTRIBUTORS     192
#define ES_DESC_OFF_WEBSITE          216
#define ES_DESC_OFF_PREFIX           248
#define ES_DESC_OFF_PROVIDES         280
#define ES_DESC_OFF_DEPEND           304
#define ES_DESC_OFF_SOFT_DEPEND      328
#define ES_DESC_OFF_LOAD_BEFORE      352
#define ES_DESC_OFF_DEFAULT_PERM     376
#define ES_DESC_OFF_COMMANDS         384
#define ES_DESC_OFF_PERMISSIONS      408

/* --- Plugin vtable --- */
#define ES_VTABLE_DESTRUCTOR      0
#define ES_VTABLE_ON_COMMAND      1
#define ES_VTABLE_GET_DESCRIPTION 2
#define ES_VTABLE_ON_LOAD         3
#define ES_VTABLE_ON_ENABLE       4
#define ES_VTABLE_ON_DISABLE      5
#define ES_VTABLE_SLOT_COUNT      6

/* --- Logger vtable --- */
#define ES_LOGGER_SLOT_LOG 4

#define ES_LOG_TRACE    0
#define ES_LOG_DEBUG    1
#define ES_LOG_INFO     2
#define ES_LOG_WARNING  3
#define ES_LOG_ERROR    4
#define ES_LOG_CRITICAL 5

/* --- Server vtable --- */
#define ES_SERVER_SLOT_GET_PLUGIN_MANAGER  7
#define ES_SERVER_SLOT_DISPATCH_COMMAND   10
#define ES_SERVER_SLOT_GET_SCHEDULER       11
#define ES_SERVER_SLOT_CREATE_BOSS_BAR    38   /* verified: vtable+304 in cpp-example-plugin */

/* --- PluginManager vtable --- */
#define ES_PM_SLOT_CALL_EVENT    14
#define ES_PM_SLOT_REGISTER_EVENT 15

/* --- std::function ABI (MSVC x64, sizeof=64) --- */
#define ES_STD_FUNCTION_SIZE 64

/* --- EventPriority --- */
#define ES_PRIORITY_LOWEST  0
#define ES_PRIORITY_LOW     1
#define ES_PRIORITY_NORMAL  2
#define ES_PRIORITY_HIGH    3
#define ES_PRIORITY_HIGHEST 4
#define ES_PRIORITY_MONITOR 5

/* --- Command class (152 bytes) --- */
#define ES_COMMAND_SIZE          152
#define ES_COMMAND_OFF_VPTR        0
#define ES_COMMAND_OFF_NAME        8
#define ES_COMMAND_OFF_DESC       40
#define ES_COMMAND_OFF_ALIASES    72
#define ES_COMMAND_OFF_USAGES     96
#define ES_COMMAND_OFF_PERMS     120
#define ES_COMMAND_OFF_CMDMAP    144

#define ES_CMD_VTABLE_DESTRUCTOR     0
#define ES_CMD_VTABLE_EXECUTE        1
#define ES_CMD_VTABLE_AS_PLUGIN_CMD  2
#define ES_CMD_VTABLE_SLOT_COUNT     3

/* --- Message = std::variant<string, Translatable> (64 bytes) --- */
#define ES_MESSAGE_SIZE          64
#define ES_MESSAGE_OFF_STRING     0
#define ES_MESSAGE_OFF_INDEX     32

/* --- CommandSender vtable --- */
#define ES_SENDER_SLOT_SEND_MESSAGE    16
#define ES_SENDER_SLOT_SEND_ERROR      17
#define ES_SENDER_SLOT_GET_SERVER      18
#define ES_SENDER_SLOT_GET_NAME        19
#define ES_SENDER_SLOT_AS_PLAYER       15   /* asPlayer() → Player*, nullptr if console */

/* --- Player vtable (primary, full chain Permissible->CommandSender->Actor->Mob->Player) --- */
#define ES_PLAYER_SLOT_SEND_MESSAGE   16
#define ES_PLAYER_SLOT_GET_NAME       19   /* CRASHES in PlayerJoinEvent */
#define ES_PLAYER_SLOT_GET_LOCATION   24   /* () -> Location (28B, hidden ptr) */
#define ES_PLAYER_SLOT_GET_XUID       57   /* () -> string (hidden ptr) */
#define ES_PLAYER_SLOT_PLAY_SOUND     65   /* (Location, string, float, float) — IDA: vtable+520 */
#define ES_PLAYER_SLOT_STOP_SOUND     66   /* (string) */
#define ES_PLAYER_SLOT_STOP_ALL       67   /* () */
#define ES_PLAYER_SLOT_SEND_POPUP     85   /* (string) */
#define ES_PLAYER_SLOT_SEND_TIP       86   /* (string) */
#define ES_PLAYER_SLOT_SEND_TOAST     87   /* (string, string) */
#define ES_PLAYER_SLOT_SEND_PACKET   109   /* (int, string_view) */

/* --- Scheduler vtable --- */
#define ES_SCHEDULER_SLOT_DTOR          0
#define ES_SCHEDULER_SLOT_RUN_TASK      1
#define ES_SCHEDULER_SLOT_RUN_LATER     2
#define ES_SCHEDULER_SLOT_RUN_TIMER     3

/* --- BossBar vtable --- */
#define ES_BOSSBAR_SLOT_DTOR            0
#define ES_BOSSBAR_SLOT_SET_TITLE       2
#define ES_BOSSBAR_SLOT_SET_PROGRESS   11
#define ES_BOSSBAR_SLOT_SET_VISIBLE    13
#define ES_BOSSBAR_SLOT_ADD_PLAYER     14   /* verified: vtable+112 in cpp-example-plugin */
#define ES_BOSSBAR_SLOT_REMOVE_PLAYER  16

/* --- BarColor enum --- */
#define ES_BAR_COLOR_PINK    0
#define ES_BAR_COLOR_BLUE    1
#define ES_BAR_COLOR_RED     2
#define ES_BAR_COLOR_GREEN   3
#define ES_BAR_COLOR_YELLOW  4
#define ES_BAR_COLOR_PURPLE  5

/* --- BarStyle enum --- */
#define ES_BAR_STYLE_SOLID        0
#define ES_BAR_STYLE_SEGMENTED6  1
#define ES_BAR_STYLE_SEGMENTED10 2
#define ES_BAR_STYLE_SEGMENTED12 3
#define ES_BAR_STYLE_SEGMENTED20 4

/* --- Location struct (32 bytes: 28 data + 4 padding, alignment=8) --- */
#define ES_LOCATION_SIZE 32

struct es_location {
    void    *dimension;
    float    x, y, z;
    float    pitch, yaw;
    uint32_t _pad;          /* trailing padding for 8-byte alignment */
};

/* --- API version --- */
#define ES_API_VERSION "0.11"

#endif /* ENDSTONE_ABI_H */

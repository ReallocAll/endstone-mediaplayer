// Focused tests for the production UTF-8 file-opening helper.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mediaplayer/endstone_api.h"
#include "abi_helpers.h"

void *g_plugin = nullptr;

static void *g_online_player_values[] = {
    (void *)(uintptr_t)0x1111,
    (void *)(uintptr_t)0x2222,
    (void *)(uintptr_t)0x3333,
};

#if ES_PLATFORM_LINUX
static void fake_get_online_players(void *out, void *server)
#else
static void *fake_get_online_players(void *server, void *out)
#endif
{
    (void)server;
    size_t count = sizeof(g_online_player_values) /
                   sizeof(g_online_player_values[0]);
    size_t bytes = count * ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE;
    unsigned char *allocation = calloc(1, bytes);
    if (allocation) {
        for (size_t index = 0; index < count; index++) {
            es_shared_init(
                allocation + index * ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE +
                    ES_NOTNULL_PLAYER_OFF_SHARED_PTR,
                g_online_player_values[index], nullptr);
        }
    }
    es_store_pointer(out, ES_VECTOR_OFF_BEGIN, allocation);
    es_store_pointer(out, ES_VECTOR_OFF_END,
                     allocation ? allocation + bytes : nullptr);
    es_store_pointer(out, ES_VECTOR_OFF_CAPACITY,
                     allocation ? allocation + bytes : nullptr);
#if !ES_PLATFORM_LINUX
    return out;
#endif
}

static int test_online_player_enumeration(void)
{
    void *vtable[ES_SERVER_SLOT_GET_ONLINE_PLAYERS + 1] = {0};
    vtable[ES_SERVER_SLOT_GET_ONLINE_PLAYERS] =
        (void *)fake_get_online_players;
    void **server = vtable;
    void *players[2] = {0};
    int count = server_get_online_players(&server, players, 2);
    return count == 2 && players[0] == g_online_player_values[0] &&
           players[1] == g_online_player_values[1] &&
           server_get_online_players(nullptr, players, 2) == -1;
}

static int test_valid_utf8_path(void)
{
    const char *path = "test_fopen_utf8_\xC3\xA9.tmp";
    FILE *file = fopen_utf8(path, "wb");
    if (file == nullptr) return 0;
    fclose(file);
#if defined(_WIN32)
    if (_wremove(L"test_fopen_utf8_\u00e9.tmp") != 0) return 0;
#else
    remove(path);
#endif
    return 1;
}

static int test_null_arguments(void)
{
    return fopen_utf8(nullptr, "rb") == nullptr &&
           fopen_utf8("test_fopen_utf8.tmp", nullptr) == nullptr;
}

#if defined(_WIN32)
static int test_invalid_utf8_is_rejected(void)
{
    const char invalid_utf8[] = "test_fopen_utf8_\xC3\x28.tmp";
    const unsigned char invalid_mode[] = {'r', 0xFF, 'b', '\0'};
    return fopen_utf8(invalid_utf8, "rb") == nullptr &&
           fopen_utf8("test_fopen_utf8.tmp", (const char *)invalid_mode) == nullptr;
}

static int test_fixed_buffers_are_not_truncated(void)
{
    char long_path[ENDSTONE_MEDIAPLAYER_PATH_MAX + 2];
    char long_mode[10];

    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    memset(long_mode, 'b', sizeof(long_mode) - 1);
    long_mode[sizeof(long_mode) - 1] = '\0';
    return fopen_utf8(long_path, "rb") == nullptr &&
           fopen_utf8("test_fopen_utf8.tmp", long_mode) == nullptr;
}
#endif

int main(void)
{
    if (!test_null_arguments() || !test_valid_utf8_path() ||
        !test_online_player_enumeration())
        return 1;
#if defined(_WIN32)
    if (!test_invalid_utf8_is_rejected() ||
        !test_fixed_buffers_are_not_truncated()) return 1;
#endif
    return 0;
}

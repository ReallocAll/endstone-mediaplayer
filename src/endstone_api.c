#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "abi_helpers.h"
#include "mediaplayer/endstone_api.h"

#include <cppcompat/string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ES_PLATFORM_WINDOWS
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <elf.h>
#include <link.h>
#endif

extern void *g_plugin;
extern void cppcompat_free(void *ptr);

#if ES_PLATFORM_WINDOWS

bool endstone_expected_image_matches(void *image)
{
    if (!image) return false;
    const unsigned char *base = image;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const IMAGE_NT_HEADERS *nt =
        (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return nt->FileHeader.TimeDateStamp ==
               (DWORD)ES_ITEM_TYPE_TYPEINFO_PE_TIMESTAMP &&
           nt->OptionalHeader.SizeOfImage ==
               (DWORD)ES_ITEM_TYPE_TYPEINFO_IMAGE_SIZE;
}

void *endstone_expected_image_base(void)
{
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules,
                               sizeof(modules), &needed)) {
        return nullptr;
    }
    size_t count = needed / sizeof(modules[0]);
    if (count > sizeof(modules) / sizeof(modules[0])) {
        count = sizeof(modules) / sizeof(modules[0]);
    }
    for (size_t index = 0; index < count; index++) {
        if (endstone_expected_image_matches(modules[index])) {
            return modules[index];
        }
    }
    return nullptr;
}

#else

struct expected_image_search {
    void *base;
    void *candidate;
};

static bool linux_image_identity(const struct dl_phdr_info *info,
                                 char *identity, size_t identity_size,
                                 uint64_t *image_size)
{
    if (!info || !identity || identity_size < 5 || !image_size) return false;
    *image_size = 0;
    identity[0] = '\0';
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; index++) {
        const ElfW(Phdr) *program = &info->dlpi_phdr[index];
        if (program->p_type == PT_LOAD) {
            uint64_t end = (uint64_t)program->p_vaddr + program->p_memsz;
            if (end > *image_size) *image_size = end;
        }
        if (program->p_type != PT_NOTE || program->p_memsz == 0) continue;
        const unsigned char *cursor =
            (const unsigned char *)(info->dlpi_addr + program->p_vaddr);
        const unsigned char *end = cursor + program->p_memsz;
        while ((size_t)(end - cursor) >= sizeof(ElfW(Nhdr))) {
            const ElfW(Nhdr) *note = (const ElfW(Nhdr) *)cursor;
            cursor += sizeof(*note);
            size_t name_size = ((size_t)note->n_namesz + 3U) & ~3U;
            size_t desc_size = ((size_t)note->n_descsz + 3U) & ~3U;
            if ((size_t)(end - cursor) < name_size + desc_size) break;
            const char *name = (const char *)cursor;
            const unsigned char *description = cursor + name_size;
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz >= 3 &&
                memcmp(name, "GNU", 3) == 0) {
                static const char digits[] = "0123456789abcdef";
                size_t required = 4 + (size_t)note->n_descsz * 2 + 1;
                if (required > identity_size) return false;
                memcpy(identity, "elf:", 4);
                for (size_t byte = 0; byte < note->n_descsz; byte++) {
                    identity[4 + byte * 2] = digits[description[byte] >> 4];
                    identity[5 + byte * 2] = digits[description[byte] & 0x0f];
                }
                identity[required - 1] = '\0';
            }
            cursor += name_size + desc_size;
        }
    }
    return identity[0] && *image_size != 0;
}

static int find_expected_image(struct dl_phdr_info *info, size_t size,
                               void *data)
{
    (void)size;
    struct expected_image_search *search = data;
    void *base = (void *)(uintptr_t)info->dlpi_addr;
    if (search->candidate && base != search->candidate) return 0;
    char identity[160];
    uint64_t image_size = 0;
    if (linux_image_identity(info, identity, sizeof(identity), &image_size) &&
        image_size == (uint64_t)ES_ITEM_TYPE_TYPEINFO_IMAGE_SIZE &&
        strcmp(identity, ES_ITEM_TYPE_TYPEINFO_IMAGE_ID) == 0) {
        search->base = base;
        return 1;
    }
    return 0;
}

bool endstone_expected_image_matches(void *base)
{
    if (!base) return false;
    struct expected_image_search search = {.candidate = base};
    dl_iterate_phdr(find_expected_image, &search);
    return search.base == base;
}

void *endstone_expected_image_base(void)
{
    struct expected_image_search search = {0};
    dl_iterate_phdr(find_expected_image, &search);
    return search.base;
}

#endif

struct online_players_result {
    _Alignas(void *) unsigned char vector[ES_VECTOR_SIZE];
    unsigned char *begin;
    unsigned char *end;
};

static bool online_players_get(void *server,
                               struct online_players_result *result)
{
    if (!server || !result ||
        !VTABLE(server)[ES_SERVER_SLOT_GET_ONLINE_PLAYERS]) {
        return false;
    }
    memset(result, 0, sizeof(*result));
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))
        VTABLE(server)[ES_SERVER_SLOT_GET_ONLINE_PLAYERS])(
            server, result->vector);
#else
    ((void (*)(void *, void *))
        VTABLE(server)[ES_SERVER_SLOT_GET_ONLINE_PLAYERS])(
            result->vector, server);
#endif
    result->begin = es_pointer_at(result->vector, ES_VECTOR_OFF_BEGIN);
    result->end = es_pointer_at(result->vector, ES_VECTOR_OFF_END);
    if (!result->begin && !result->end) return true;
    if (!result->begin || !result->end || result->end < result->begin ||
        (size_t)(result->end - result->begin) %
            ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE != 0) {
        if (result->begin) cppcompat_free(result->begin);
        memset(result, 0, sizeof(*result));
        return false;
    }
    return true;
}

static void online_players_destroy(struct online_players_result *result)
{
    if (!result) return;
    for (unsigned char *element = result->begin;
         element && element < result->end;
         element += ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE) {
        es_shared_release(element + ES_NOTNULL_PLAYER_OFF_SHARED_PTR);
    }
    cppcompat_free(result->begin);
    memset(result, 0, sizeof(*result));
}

int server_get_online_players(void *server, void **players, int capacity)
{
    if (!players || capacity <= 0) return -1;
    struct online_players_result result;
    if (!online_players_get(server, &result)) return -1;
    size_t count = result.begin
        ? (size_t)(result.end - result.begin) /
            ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE
        : 0;
    size_t copied = count < (size_t)capacity ? count : (size_t)capacity;
    for (size_t index = 0; index < copied; index++) {
        const unsigned char *element = result.begin +
            index * ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE;
        players[index] = es_shared_object(
            element + ES_NOTNULL_PLAYER_OFF_SHARED_PTR);
    }
    online_players_destroy(&result);
    return (int)copied;
}

bool server_find_player_handle(void *server, void *player,
                               struct es_shared_handle *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    struct online_players_result result;
    if (!player || !online_players_get(server, &result)) return false;
    bool found = false;
    for (unsigned char *element = result.begin;
         element && element < result.end;
         element += ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE) {
        void *handle = element + ES_NOTNULL_PLAYER_OFF_SHARED_PTR;
        if (es_shared_object(handle) == player) {
            es_shared_copy(out, handle);
            found = true;
            break;
        }
    }
    online_players_destroy(&result);
    return found;
}

void *server_player_from_sender(void *server,
                                const struct es_shared_handle *sender)
{
    if (!sender || !es_shared_control(sender)) return nullptr;
    struct online_players_result result;
    if (!online_players_get(server, &result)) return nullptr;
    void *player = nullptr;
    for (unsigned char *element = result.begin;
         element && element < result.end;
         element += ES_VECTOR_NOTNULL_PLAYER_ELEMENT_SIZE) {
        void *handle = element + ES_NOTNULL_PLAYER_OFF_SHARED_PTR;
        if (es_shared_control(handle) == es_shared_control(sender)) {
            player = es_shared_object(handle);
            break;
        }
    }
    online_players_destroy(&result);
    return player;
}

FILE *fopen_utf8(const char *path, const char *mode)
{
    if (path == nullptr || mode == nullptr) return nullptr;
#if ES_PLATFORM_WINDOWS
    wchar_t wide_path[ENDSTONE_MEDIAPLAYER_PATH_MAX];
    wchar_t wide_mode[8];
    int path_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          path, -1, nullptr, 0);
    int mode_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          mode, -1, nullptr, 0);
    if (path_length <= 0 || mode_length <= 0 ||
        path_length > (int)(sizeof(wide_path) / sizeof(wide_path[0])) ||
        mode_length > (int)(sizeof(wide_mode) / sizeof(wide_mode[0]))) {
        return nullptr;
    }
    int converted_path = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             path, -1, wide_path,
                                             path_length);
    int converted_mode = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             mode, -1, wide_mode,
                                             mode_length);
    if (converted_path != path_length || converted_mode != mode_length) {
        return nullptr;
    }
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
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!player || !VTABLE(player)[ES_PLAYER_SLOT_GET_LOCATION]) return;
#if ES_PLATFORM_WINDOWS
    ((void (*)(void *, void *))
        VTABLE(player)[ES_PLAYER_SLOT_GET_LOCATION])(player, out);
#else
    ((void (*)(void *, void *))
        VTABLE(player)[ES_PLAYER_SLOT_GET_LOCATION])(out, player);
#endif
}

void player_play_sound(void *player, const char *sound, float volume,
                       float pitch)
{
    if (!player) return;
    struct es_location location;
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    player_get_location(player, &location);
    cpp_string_construct(string, sound);
    STR_GUARD(string,
        VCALL4(player, ES_PLAYER_SLOT_PLAY_SOUND, void,
               void *, &location, void *, string,
               float, volume, float, pitch));
    if (ES_C_ABI_LOCATION_CALLEE_DESTROYS) {
        memset(&location, 0, sizeof(location));
    }
    else {
        es_location_release(&location);
    }
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

struct boss_bar_handle {
    struct es_shared_handle handle;
};

static void *boss_bar_object(void *boss)
{
    return boss ? es_shared_object(&((struct boss_bar_handle *)boss)->handle)
                : nullptr;
}

void *boss_bar_create(void *player, const char *title)
{
    if (!g_plugin || !player) return nullptr;
    void *server = PLUGIN_SERVER(g_plugin);
    if (!server || !VTABLE(server)[ES_SERVER_SLOT_CREATE_BOSS_BAR]) {
        return nullptr;
    }
    struct boss_bar_handle *boss = calloc(1, sizeof(*boss));
    if (!boss) return nullptr;
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    cpp_string_construct(string, title);
#if ES_PLATFORM_WINDOWS
    STR_GUARD(string,
        ((void (*)(void *, void *, void *, int, int))
            VTABLE(server)[ES_SERVER_SLOT_CREATE_BOSS_BAR])(
                server, &boss->handle, string,
                ES_BAR_COLOR_GREEN, ES_BAR_STYLE_SOLID));
#else
    STR_GUARD(string,
        ((void (*)(void *, void *, void *, int, int))
            VTABLE(server)[ES_SERVER_SLOT_CREATE_BOSS_BAR])(
                &boss->handle, server, string,
                ES_BAR_COLOR_GREEN, ES_BAR_STYLE_SOLID));
#endif
    void *object = boss_bar_object(boss);
    struct es_shared_handle player_handle = {0};
    if (!object || !server_find_player_handle(server, player,
                                               &player_handle)) {
        es_shared_release(&boss->handle);
        free(boss);
        return nullptr;
    }
    VCALL1(object, ES_BOSSBAR_SLOT_ADD_PLAYER, void,
           const void *, &player_handle);
    es_shared_release(&player_handle);
    return boss;
}

void boss_bar_destroy(void *boss)
{
    if (!boss) return;
    struct boss_bar_handle *handle = boss;
    void *object = boss_bar_object(boss);
    if (object) {
        VCALL1(object, ES_BOSSBAR_SLOT_SET_VISIBLE, void, bool, false);
    }
    es_shared_release(&handle->handle);
    free(handle);
}

void boss_bar_set_progress(void *boss, float progress)
{
    void *object = boss_bar_object(boss);
    if (object) {
        VCALL1(object, ES_BOSSBAR_SLOT_SET_PROGRESS, void, float, progress);
    }
}

void boss_bar_set_title(void *boss, const char *title)
{
    void *object = boss_bar_object(boss);
    if (!object) return;
    _Alignas(8) unsigned char string[ES_STRING_SIZE];
    cpp_string_construct(string, title);
    STR_GUARD(string,
        VCALL1(object, ES_BOSSBAR_SLOT_SET_TITLE, void, void *, string));
}

#ifndef ENDSTONE_MEDIAPLAYER_API_H
#define ENDSTONE_MEDIAPLAYER_API_H

// Stable MediaPlayer provider ABI.  Every call is made on Endstone's main
// thread.  Names, pixel buffers and strings are borrowed for the duration of
// the call and are never retained by the provider.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#else
#include <dlfcn.h>
#endif

#if defined(_WIN32)
#define MP_CALL __cdecl
#if defined(MP_PROVIDER_BUILD)
#define MP_EXPORT __declspec(dllexport)
#else
#define MP_EXPORT
#endif
#else
#define MP_CALL
#if defined(MP_PROVIDER_BUILD)
#define MP_EXPORT __attribute__((visibility("default")))
#else
#define MP_EXPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MP_API_V1 1u
#define MP_ABI_VERSION_V1 MP_API_V1
#define MP_SCREEN_NAME_SIZE 64u
#define MP_INVALID_SCREEN_HANDLE UINT64_C(0)
#define MP_INVALID_FRAME_HANDLE UINT64_C(0)
#define MP_SCREEN_HANDLE_INVALID MP_INVALID_SCREEN_HANDLE
#define MP_FRAME_HANDLE_INVALID MP_INVALID_FRAME_HANDLE

typedef uint64_t mp_screen_handle;
typedef uint64_t mp_frame_handle;

typedef int32_t mp_result;
enum {
    MP_OK = 0,
    MP_ERR_UNAVAILABLE = -1,
    MP_ERR_INVALID_ARGUMENT = -2,
    MP_ERR_INVALID_HANDLE = -3,
    MP_ERR_NOT_FOUND = -4,
    MP_ERR_ALREADY_EXISTS = -5,
    MP_ERR_CAPACITY = -6,
    MP_ERR_NO_MEMORY = -7,
    MP_ERR_UNSUPPORTED = -8,
    MP_ERR_BUFFER_TOO_SMALL = -9,
    MP_ERR_BUSY = -10,
    MP_ERR_IO = -11,
    MP_ERR_INTERNAL = -12,
};

// Compatibility spellings kept in the public namespace intentionally.
#define MP_ERR_INVALID_ARG MP_ERR_INVALID_ARGUMENT
#define MP_ERR_EXISTS MP_ERR_ALREADY_EXISTS

typedef uint32_t mp_pixel_format;
#define MP_PIXEL_FORMAT_ABGR UINT32_C(0)
#define MP_PIXEL_FORMAT_BGRA UINT32_C(1)
#define MP_PIXEL_FORMAT_RGBA UINT32_C(2)
#define MP_FORMAT_ABGR MP_PIXEL_FORMAT_ABGR
#define MP_FORMAT_BGRA MP_PIXEL_FORMAT_BGRA
#define MP_FORMAT_RGBA MP_PIXEL_FORMAT_RGBA

typedef uint32_t mp_backend;
#define MP_BACKEND_LOGICAL UINT32_C(0)
#define MP_BACKEND_PLUGIN_MANAGED UINT32_C(1)
#define MP_BACKEND_WORLD UINT32_C(2)
#define MP_BACKEND_PLUGIN MP_BACKEND_PLUGIN_MANAGED

typedef struct mp_screen_create_info {
    uint32_t struct_size;
    uint32_t flags;
    const char *name;
    uint32_t width_tiles;
    uint32_t height_tiles;
    mp_backend backend;
} mp_screen_create_info;

typedef struct mp_screen_info {
    uint32_t struct_size;
    uint32_t flags;
    char name[MP_SCREEN_NAME_SIZE];
    mp_screen_handle handle;
    uint64_t instance_id;
    uint32_t width_tiles;
    uint32_t height_tiles;
    uint32_t pixel_width;
    uint32_t pixel_height;
    mp_backend backend;
    uint32_t playing;
    uint64_t surface_generation;
} mp_screen_info;

typedef struct mp_stats {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t width_tiles;
    uint32_t height_tiles;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint64_t generation;
    uint64_t resident_tiles;
    uint64_t pending_tiles;
    uint64_t max_resident_tiles;
    uint64_t max_pending_tiles;
    uint64_t dropped_tiles;
    uint64_t allocated_pixel_bytes;
    uint64_t staged_tiles;
    uint64_t allocated_staging_pixel_bytes;
} mp_stats;
typedef mp_stats mp_surface_stats;
typedef mp_stats mp_screen_stats;

typedef struct mp_capabilities {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t abi_version;
    uint32_t max_width_tiles;
    uint32_t max_height_tiles;
    uint32_t tile_size;
    uint32_t max_screens;
    uint32_t max_resident_tiles;
    uint32_t max_pending_tiles;
    uint32_t pixel_format_bits;
    uint32_t backend_bits;
} mp_capabilities;

#define MP_PIXEL_FORMAT_BIT(format) (UINT32_C(1) << (format))
#define MP_BACKEND_BIT(backend) (UINT32_C(1) << (backend))

typedef const char *(MP_CALL *mp_result_string_fn)(mp_result result);
typedef mp_result (MP_CALL *mp_capabilities_get_fn)(mp_capabilities *out);
typedef mp_result (MP_CALL *mp_screen_create_fn)(
    const mp_screen_create_info *info, mp_screen_handle *out_handle);
typedef mp_result (MP_CALL *mp_screen_delete_fn)(mp_screen_handle handle);
typedef mp_result (MP_CALL *mp_screen_find_fn)(
    const char *name, mp_screen_handle *out_handle);
typedef mp_result (MP_CALL *mp_screen_list_fn)(
    mp_screen_handle *out_handles, uint32_t capacity, uint32_t *out_count);
typedef mp_result (MP_CALL *mp_screen_get_info_fn)(
    mp_screen_handle handle, mp_screen_info *out);
typedef mp_result (MP_CALL *mp_screen_rename_fn)(
    mp_screen_handle handle, const char *name);
typedef mp_result (MP_CALL *mp_screen_clear_fn)(mp_screen_handle handle);
typedef mp_result (MP_CALL *mp_screen_get_stats_fn)(
    mp_screen_handle handle, mp_stats *out);
typedef mp_result (MP_CALL *mp_screen_update_tile_fn)(
    mp_screen_handle handle, uint32_t tile_x, uint32_t tile_y,
    const void *pixels, uint64_t buffer_bytes, uint64_t stride,
    mp_pixel_format format);
typedef mp_result (MP_CALL *mp_screen_update_region_fn)(
    mp_screen_handle handle, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const void *pixels, uint64_t buffer_bytes,
    uint64_t stride, mp_pixel_format format);
typedef mp_result (MP_CALL *mp_frame_begin_fn)(
    mp_screen_handle screen, mp_frame_handle *out_frame);
typedef mp_result (MP_CALL *mp_frame_update_tile_fn)(
    mp_frame_handle frame, uint32_t tile_x, uint32_t tile_y,
    const void *pixels, uint64_t buffer_bytes, uint64_t stride,
    mp_pixel_format format);
typedef mp_result (MP_CALL *mp_frame_update_region_fn)(
    mp_frame_handle frame, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const void *pixels, uint64_t buffer_bytes,
    uint64_t stride, mp_pixel_format format);
typedef mp_result (MP_CALL *mp_frame_commit_fn)(mp_frame_handle frame);
typedef mp_result (MP_CALL *mp_frame_abort_fn)(mp_frame_handle frame);

typedef struct mp_api_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t instance_id;
    mp_result_string_fn result_string;
    mp_capabilities_get_fn capabilities_get;
    mp_screen_create_fn screen_create;
    mp_screen_delete_fn screen_delete;
    mp_screen_find_fn screen_find;
    mp_screen_list_fn screen_list;
    mp_screen_get_info_fn screen_get_info;
    mp_screen_rename_fn screen_rename;
    mp_screen_clear_fn screen_clear;
    mp_screen_get_stats_fn screen_get_stats;
    mp_screen_update_tile_fn screen_update_tile;
    mp_screen_update_region_fn screen_update_region;
    mp_frame_begin_fn frame_begin;
    mp_frame_update_tile_fn frame_update_tile;
    mp_frame_update_region_fn frame_update_region;
    mp_frame_commit_fn frame_commit;
    mp_frame_abort_fn frame_abort;
} mp_api_v1;

// The provider returns a pointer to its immutable table.  It remains valid
// until the provider module unloads; callers must stop using it before then.
MP_EXPORT const mp_api_v1 *MP_CALL endstone_mediaplayer_get_api(uint32_t version);

static_assert(sizeof(mp_screen_handle) == 8, "screen handles are uint64");
static_assert(sizeof(mp_frame_handle) == 8, "frame handles are uint64");
static_assert(sizeof(mp_result) == 4, "results are int32");
static_assert(sizeof(mp_pixel_format) == 4, "pixel format is uint32");
static_assert(sizeof(mp_backend) == 4, "backend is uint32");
static_assert(offsetof(mp_screen_create_info, struct_size) == 0, "ABI");
static_assert(offsetof(mp_screen_create_info, flags) == 4, "ABI");
static_assert(offsetof(mp_screen_create_info, name) == 8, "ABI");
static_assert(offsetof(mp_screen_info, handle) == 72, "ABI");
static_assert(offsetof(mp_stats, generation) == 24, "ABI");
static_assert(offsetof(mp_capabilities, backend_bits) == 40, "ABI");
static_assert(offsetof(mp_api_v1, result_string) == 16, "ABI");
static_assert(offsetof(mp_api_v1, screen_list) == 56, "ABI");
static_assert(sizeof(mp_screen_create_info) == 32, "ABI");
static_assert(sizeof(mp_screen_info) == 120, "ABI");
static_assert(sizeof(mp_stats) == 96, "ABI");
static_assert(sizeof(mp_capabilities) == 44, "ABI");
static_assert(sizeof(mp_api_v1) == 152, "ABI");

#ifdef __cplusplus
} // extern "C"
#endif

// Header-only loader -------------------------------------------------------

typedef const mp_api_v1 *(MP_CALL *mp_get_api_symbol_fn)(uint32_t version);

static inline const char *MP_CALL mp_sdk_result_string(mp_result result)
{
    switch (result) {
    case MP_OK: return "ok";
    case MP_ERR_UNAVAILABLE: return "unavailable";
    case MP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MP_ERR_INVALID_HANDLE: return "invalid handle";
    case MP_ERR_NOT_FOUND: return "not found";
    case MP_ERR_ALREADY_EXISTS: return "already exists";
    case MP_ERR_CAPACITY: return "capacity exceeded";
    case MP_ERR_NO_MEMORY: return "out of memory";
    case MP_ERR_UNSUPPORTED: return "unsupported";
    case MP_ERR_BUFFER_TOO_SMALL: return "buffer too small";
    case MP_ERR_BUSY: return "busy";
    case MP_ERR_IO: return "I/O error";
    case MP_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

static inline void MP_CALL mp_sdk_invalidate_screen(mp_screen_handle *out)
{
    if (out) *out = MP_INVALID_SCREEN_HANDLE;
}
static inline void MP_CALL mp_sdk_invalidate_frame(mp_frame_handle *out)
{
    if (out) *out = MP_INVALID_FRAME_HANDLE;
}
static inline mp_result MP_CALL mp_sdk_unavailable_caps(mp_capabilities *out)
{
    if (out && out->struct_size >= sizeof(uint32_t) * 2) {
        mp_capabilities value;
        memset(&value, 0, sizeof(value));
        value.struct_size = out->struct_size;
        value.flags = 0;
        size_t n = out->struct_size < sizeof(value) ? out->struct_size : sizeof(value);
        memcpy(out, &value, n);
    }
    return MP_ERR_UNAVAILABLE;
}
static inline mp_result MP_CALL mp_sdk_unavailable_create(
    const mp_screen_create_info *info, mp_screen_handle *out)
{ (void)info; mp_sdk_invalidate_screen(out); return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_delete(mp_screen_handle h)
{ (void)h; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_find(const char *n, mp_screen_handle *out)
{ (void)n; mp_sdk_invalidate_screen(out); return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_list(
    mp_screen_handle *h, uint32_t capacity, uint32_t *n)
{ (void)h; (void)capacity; if (n) *n = 0; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_info(mp_screen_handle h, mp_screen_info *o)
{ (void)h; (void)o; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_rename(mp_screen_handle h, const char *n)
{ (void)h; (void)n; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_clear(mp_screen_handle h)
{ (void)h; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_stats(mp_screen_handle h, mp_stats *o)
{ (void)h; (void)o; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_update(
    mp_screen_handle h, uint32_t x, uint32_t y, uint32_t w, uint32_t z,
    const void *p, uint64_t b, uint64_t s, mp_pixel_format f)
{ (void)h; (void)x; (void)y; (void)w; (void)z; (void)p; (void)b; (void)s; (void)f; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_tile(
    mp_screen_handle h, uint32_t x, uint32_t y, const void *p,
    uint64_t b, uint64_t s, mp_pixel_format f)
{ (void)h; (void)x; (void)y; (void)p; (void)b; (void)s; (void)f; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_frame_tile(
    mp_frame_handle h, uint32_t x, uint32_t y, const void *p,
    uint64_t b, uint64_t s, mp_pixel_format f)
{ (void)h; (void)x; (void)y; (void)p; (void)b; (void)s; (void)f; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_begin(mp_screen_handle h, mp_frame_handle *out)
{ (void)h; mp_sdk_invalidate_frame(out); return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_frame_update(
    mp_frame_handle h, uint32_t x, uint32_t y, uint32_t w, uint32_t z,
    const void *p, uint64_t b, uint64_t s, mp_pixel_format f)
{ (void)h; (void)x; (void)y; (void)w; (void)z; (void)p; (void)b; (void)s; (void)f; return MP_ERR_UNAVAILABLE; }
static inline mp_result MP_CALL mp_sdk_unavailable_frame(mp_frame_handle h)
{ (void)h; return MP_ERR_UNAVAILABLE; }

static inline void mp_sdk_fallback(mp_api_v1 *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->abi_version = MP_API_V1;
    out->struct_size = (uint32_t)sizeof(*out);
    out->result_string = mp_sdk_result_string;
    out->capabilities_get = mp_sdk_unavailable_caps;
    out->screen_create = mp_sdk_unavailable_create;
    out->screen_delete = mp_sdk_unavailable_delete;
    out->screen_find = mp_sdk_unavailable_find;
    out->screen_list = mp_sdk_unavailable_list;
    out->screen_get_info = mp_sdk_unavailable_info;
    out->screen_rename = mp_sdk_unavailable_rename;
    out->screen_clear = mp_sdk_unavailable_clear;
    out->screen_get_stats = mp_sdk_unavailable_stats;
    out->screen_update_tile = mp_sdk_unavailable_tile;
    out->screen_update_region = mp_sdk_unavailable_update;
    out->frame_begin = mp_sdk_unavailable_begin;
    out->frame_update_tile = mp_sdk_unavailable_frame_tile;
    out->frame_update_region = mp_sdk_unavailable_frame_update;
    out->frame_commit = mp_sdk_unavailable_frame;
    out->frame_abort = mp_sdk_unavailable_frame;
}

static inline bool mp_sdk_api_valid(const mp_api_v1 *api)
{
    if (!api || api->abi_version != MP_API_V1 ||
        api->struct_size < offsetof(mp_api_v1, frame_abort) + sizeof(api->frame_abort))
        return false;
    return api->result_string && api->capabilities_get && api->screen_create &&
        api->screen_delete && api->screen_find && api->screen_list &&
        api->screen_get_info && api->screen_rename && api->screen_clear &&
        api->screen_get_stats && api->screen_update_tile &&
        api->screen_update_region && api->frame_begin &&
        api->frame_update_tile && api->frame_update_region &&
        api->frame_commit && api->frame_abort;
}

#if defined(_WIN32)
static inline bool mp_sdk_windows_module_name(const wchar_t *name)
{
    static const wchar_t exact[] = L"endstone_mediaplayer.dll";
    static const wchar_t prefix[] = L"endstone_mediaplayer-";
    size_t length = wcslen(name);
    size_t prefix_length = (sizeof(prefix) / sizeof(prefix[0])) - 1u;
    if (_wcsicmp(name, exact) == 0) return true;
    return length > prefix_length + 4u &&
        _wcsnicmp(name, prefix, prefix_length) == 0 &&
        _wcsicmp(name + length - 4u, L".dll") == 0;
}

static inline const mp_api_v1 *mp_sdk_windows_api(void)
{
    const mp_api_v1 *inactive = (const mp_api_v1 *)0;
    HMODULE exact = GetModuleHandleW(L"endstone_mediaplayer.dll");
    if (exact) {
        mp_get_api_symbol_fn symbol = (mp_get_api_symbol_fn)(void *)
            GetProcAddress(exact, "endstone_mediaplayer_get_api");
        if (symbol) {
            const mp_api_v1 *candidate = symbol(MP_API_V1);
            if (mp_sdk_api_valid(candidate)) {
                if (candidate->instance_id != 0) return candidate;
                inactive = candidate;
            }
        }
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return inactive;
    MODULEENTRY32W entry = {0};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!mp_sdk_windows_module_name(entry.szModule) ||
                entry.hModule == exact)
                continue;
            mp_get_api_symbol_fn symbol = (mp_get_api_symbol_fn)(void *)
                GetProcAddress(entry.hModule, "endstone_mediaplayer_get_api");
            if (!symbol) continue;
            const mp_api_v1 *candidate = symbol(MP_API_V1);
            if (!mp_sdk_api_valid(candidate)) continue;
            if (candidate->instance_id != 0) {
                CloseHandle(snapshot);
                return candidate;
            }
            if (!inactive) inactive = candidate;
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return inactive;
}
#endif

static inline bool mp_try_get_api_v1(mp_api_v1 *out)
{
    if (!out) return false;
    mp_sdk_fallback(out);
    const mp_api_v1 *api = (const mp_api_v1 *)0;
#if defined(_WIN32)
    // Only an already-loaded provider is considered.  The SDK never loads or
    // unloads the module and therefore cannot impose a lifetime on Endstone.
    // Endstone may shadow-copy native plugins to
    // endstone_mediaplayer-<content-hash>.dll. Enumerate only already-loaded
    // modules and prefer a currently active validated provider.
    api = mp_sdk_windows_api();
#else
    mp_get_api_symbol_fn symbol = (mp_get_api_symbol_fn)dlsym(
        RTLD_DEFAULT, "endstone_mediaplayer_get_api");
    void *module = (void *)0;
    if (!symbol) {
#if defined(RTLD_NOLOAD)
        module = dlopen("endstone_mediaplayer.so", RTLD_NOW | RTLD_NOLOAD);
        if (module)
            symbol = (mp_get_api_symbol_fn)dlsym(
                module, "endstone_mediaplayer_get_api");
#endif
    }
    if (symbol) api = symbol(MP_API_V1);
#endif
    if (!mp_sdk_api_valid(api)) return false;
    size_t n = api->struct_size < sizeof(*out) ? api->struct_size : sizeof(*out);
    memcpy(out, api, n);
    return true;
}

static inline mp_api_v1 mp_get_api_v1(void)
{
    mp_api_v1 api;
    (void)mp_try_get_api_v1(&api);
    return api;
}

#endif // ENDSTONE_MEDIAPLAYER_API_H

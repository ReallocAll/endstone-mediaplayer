#include "endstone_mediaplayer_api.h"

#include <string.h>

static uint64_t g_mock_instance = UINT64_C(0x4242);
static bool g_mock_exists = true;
static char g_mock_name[MP_SCREEN_NAME_SIZE] = "mock-screen";
static mp_frame_handle g_mock_frame = MP_INVALID_FRAME_HANDLE;
static uint64_t g_last_buffer_bytes;
static uint64_t g_last_stride;
static mp_pixel_format g_last_format;
static uint32_t g_last_first_byte;
static bool g_mock_unavailable;
static bool g_mock_fail_commit;
static uint32_t g_mock_abort_count;

static const char *MP_CALL mock_result_string(mp_result result)
{
    switch (result) {
    case MP_OK: return "ok";
    case MP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MP_ERR_INVALID_HANDLE: return "invalid handle";
    case MP_ERR_NOT_FOUND: return "not found";
    case MP_ERR_ALREADY_EXISTS: return "already exists";
    default: return "mock result";
    }
}

static mp_result MP_CALL mock_caps(mp_capabilities *out)
{
    if (!out || out->struct_size < sizeof(uint32_t) * 2 || out->flags != 0)
        return MP_ERR_INVALID_ARGUMENT;
    mp_capabilities value = {
        .struct_size = out->struct_size,
        .abi_version = MP_API_V1,
        .max_width_tiles = 1024,
        .max_height_tiles = 1024,
        .tile_size = 1,
        .max_screens = 64,
        .max_resident_tiles = 4096,
        .max_pending_tiles = 256,
        .pixel_format_bits = MP_PIXEL_FORMAT_BIT(MP_PIXEL_FORMAT_ABGR) |
            MP_PIXEL_FORMAT_BIT(MP_PIXEL_FORMAT_BGRA) |
            MP_PIXEL_FORMAT_BIT(MP_PIXEL_FORMAT_RGBA),
        .backend_bits = MP_BACKEND_BIT(MP_BACKEND_LOGICAL),
    };
    size_t bytes = out->struct_size < sizeof(value) ? out->struct_size : sizeof(value);
    memcpy(out, &value, bytes);
    return MP_OK;
}

static mp_result MP_CALL mock_find(const char *name, mp_screen_handle *out)
{
    if (out) *out = MP_INVALID_SCREEN_HANDLE;
    if (g_mock_unavailable) return MP_ERR_UNAVAILABLE;
    if (!name || !out) return MP_ERR_INVALID_ARGUMENT;
    if (!g_mock_exists || strcmp(name, g_mock_name) != 0)
        return MP_ERR_NOT_FOUND;
    *out = UINT64_C(0xfeed1234);
    return MP_OK;
}

static mp_result MP_CALL mock_create(const mp_screen_create_info *i, mp_screen_handle *o)
{
    if (o) *o = MP_INVALID_SCREEN_HANDLE;
    if (!i || !o || i->struct_size < sizeof(*i) || i->flags != 0 ||
        !i->name || !i->width_tiles || !i->height_tiles ||
        i->backend != MP_BACKEND_LOGICAL)
        return MP_ERR_INVALID_ARGUMENT;
    if (g_mock_exists) return MP_ERR_ALREADY_EXISTS;
    size_t length = strlen(i->name);
    if (!length || length >= sizeof(g_mock_name)) return MP_ERR_INVALID_ARGUMENT;
    memcpy(g_mock_name, i->name, length + 1);
    g_mock_exists = true;
    *o = UINT64_C(0xfeed1234);
    return MP_OK;
}
static mp_result MP_CALL mock_delete(mp_screen_handle h)
{
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists)
        return MP_ERR_INVALID_HANDLE;
    g_mock_exists = false;
    g_mock_frame = MP_INVALID_FRAME_HANDLE;
    return MP_OK;
}
static mp_result MP_CALL mock_list(mp_screen_handle *h, uint32_t capacity,
                                   uint32_t *n)
{
    if (n) *n = g_mock_exists ? 1 : 0;
    if (!g_mock_exists) return MP_OK;
    if (!h || !capacity) return MP_OK;
    h[0] = UINT64_C(0xfeed1234);
    return MP_OK;
}
static mp_result MP_CALL mock_info(mp_screen_handle h, mp_screen_info *o)
{
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists)
        return MP_ERR_INVALID_HANDLE;
    if (!o || o->struct_size < sizeof(uint32_t) * 2 || o->flags != 0)
        return MP_ERR_INVALID_ARGUMENT;
    mp_screen_info value = {0};
    value.struct_size = o->struct_size;
    memcpy(value.name, g_mock_name, sizeof(value.name));
    value.handle = h;
    value.instance_id = g_mock_instance;
    value.width_tiles = 1;
    value.height_tiles = 1;
    value.pixel_width = 1;
    value.pixel_height = 1;
    value.backend = MP_BACKEND_LOGICAL;
    size_t bytes = o->struct_size < sizeof(value) ? o->struct_size : sizeof(value);
    memcpy(o, &value, bytes);
    return MP_OK;
}
static mp_result MP_CALL mock_rename(mp_screen_handle h, const char *n)
{
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists)
        return MP_ERR_INVALID_HANDLE;
    if (!n || !*n)
        return MP_ERR_INVALID_ARGUMENT;
    size_t length = strlen(n);
    if (length >= sizeof(g_mock_name))
        return MP_ERR_INVALID_ARGUMENT;
    memcpy(g_mock_name, n, length + 1);
    return MP_OK;
}
static mp_result MP_CALL mock_clear(mp_screen_handle h)
{ return h == UINT64_C(0xfeed1234) && g_mock_exists ? MP_OK : MP_ERR_INVALID_HANDLE; }
static mp_result MP_CALL mock_stats(mp_screen_handle h, mp_stats *o)
{
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists)
        return MP_ERR_INVALID_HANDLE;
    if (!o || o->struct_size < sizeof(uint32_t) * 2 || o->flags != 0)
        return MP_ERR_INVALID_ARGUMENT;
    mp_stats value = {0};
    value.struct_size = o->struct_size;
    value.width_tiles = 1;
    value.height_tiles = 1;
    value.pixel_width = 1;
    value.pixel_height = 1;
    size_t bytes = o->struct_size < sizeof(value) ? o->struct_size : sizeof(value);
    memcpy(o, &value, bytes);
    return MP_OK;
}
static mp_result MP_CALL mock_update_tile(mp_screen_handle h, uint32_t x, uint32_t y,
                                          const void *p, uint64_t b, uint64_t s,
                                          mp_pixel_format f)
{
    (void)x; (void)y;
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists || !p || !b || !s)
        return MP_ERR_INVALID_ARGUMENT;
    g_last_buffer_bytes = b;
    g_last_stride = s;
    g_last_format = f;
    g_last_first_byte = ((const unsigned char *)p)[0];
    return MP_OK;
}
static mp_result MP_CALL mock_update_region(mp_screen_handle h, uint32_t x, uint32_t y,
                                            uint32_t w, uint32_t z, const void *p,
                                            uint64_t b, uint64_t s, mp_pixel_format f)
{
    (void)x; (void)y;
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists || !w || !z || !p || !b || !s)
        return MP_ERR_INVALID_ARGUMENT;
    g_last_buffer_bytes = b;
    g_last_stride = s;
    g_last_format = f;
    g_last_first_byte = ((const unsigned char *)p)[0];
    return MP_OK;
}
static mp_result MP_CALL mock_begin(mp_screen_handle h, mp_frame_handle *o)
{
    if (o) *o = MP_INVALID_FRAME_HANDLE;
    if (h != UINT64_C(0xfeed1234) || !g_mock_exists || !o)
        return MP_ERR_INVALID_HANDLE;
    g_mock_frame = UINT64_C(0xbeef);
    *o = g_mock_frame;
    return MP_OK;
}
static mp_result MP_CALL mock_frame_tile(mp_frame_handle h, uint32_t x, uint32_t y,
                                         const void *p, uint64_t b, uint64_t s,
                                         mp_pixel_format f)
{
    (void)x; (void)y;
    if (h != g_mock_frame || !p || !b || !s)
        return MP_ERR_INVALID_HANDLE;
    g_last_buffer_bytes = b;
    g_last_stride = s;
    g_last_format = f;
    g_last_first_byte = ((const unsigned char *)p)[0];
    return MP_OK;
}
static mp_result MP_CALL mock_frame_region(mp_frame_handle h, uint32_t x, uint32_t y,
                                           uint32_t w, uint32_t z, const void *p,
                                           uint64_t b, uint64_t s, mp_pixel_format f)
{
    (void)x; (void)y;
    if (h != g_mock_frame || !w || !z || !p || !b || !s)
        return MP_ERR_INVALID_HANDLE;
    g_last_buffer_bytes = b;
    g_last_stride = s;
    g_last_format = f;
    g_last_first_byte = ((const unsigned char *)p)[0];
    return MP_OK;
}
static mp_result MP_CALL mock_frame_commit(mp_frame_handle h)
{
    if (h != g_mock_frame) return MP_ERR_INVALID_HANDLE;
    if (g_mock_fail_commit) return MP_ERR_BUSY;
    g_mock_frame = MP_INVALID_FRAME_HANDLE;
    return MP_OK;
}
static mp_result MP_CALL mock_frame_abort(mp_frame_handle h)
{
    if (h != g_mock_frame) return MP_ERR_INVALID_HANDLE;
    g_mock_frame = MP_INVALID_FRAME_HANDLE;
    g_mock_abort_count++;
    return MP_OK;
}

static mp_api_v1 g_mock_api = {
    .abi_version = MP_API_V1,
    .struct_size = sizeof(mp_api_v1),
    .instance_id = UINT64_C(0x4242),
    .result_string = mock_result_string,
    .capabilities_get = mock_caps,
    .screen_create = mock_create,
    .screen_delete = mock_delete,
    .screen_find = mock_find,
    .screen_list = mock_list,
    .screen_get_info = mock_info,
    .screen_rename = mock_rename,
    .screen_clear = mock_clear,
    .screen_get_stats = mock_stats,
    .screen_update_tile = mock_update_tile,
    .screen_update_region = mock_update_region,
    .frame_begin = mock_begin,
    .frame_update_tile = mock_frame_tile,
    .frame_update_region = mock_frame_region,
    .frame_commit = mock_frame_commit,
    .frame_abort = mock_frame_abort,
};

MP_EXPORT const mp_api_v1 *MP_CALL endstone_mediaplayer_get_api(uint32_t version)
{
    return version == MP_API_V1 ? &g_mock_api : nullptr;
}

MP_EXPORT void MP_CALL mock_mediaplayer_set_instance(uint64_t instance_id)
{
    g_mock_instance = instance_id;
    g_mock_api.instance_id = instance_id;
}

MP_EXPORT void MP_CALL mock_mediaplayer_reset(void)
{
    g_mock_instance = UINT64_C(0x4242);
    g_mock_api.instance_id = g_mock_instance;
    g_mock_exists = true;
    memcpy(g_mock_name, "mock-screen", sizeof("mock-screen"));
    g_mock_frame = MP_INVALID_FRAME_HANDLE;
    g_last_buffer_bytes = 0;
    g_last_stride = 0;
    g_last_format = MP_PIXEL_FORMAT_ABGR;
    g_last_first_byte = 0;
    g_mock_unavailable = false;
    g_mock_fail_commit = false;
    g_mock_abort_count = 0;
}

MP_EXPORT uint64_t MP_CALL mock_mediaplayer_last_buffer_bytes(void)
{ return g_last_buffer_bytes; }

MP_EXPORT uint64_t MP_CALL mock_mediaplayer_last_stride(void)
{ return g_last_stride; }

MP_EXPORT uint32_t MP_CALL mock_mediaplayer_last_format(void)
{ return g_last_format; }

MP_EXPORT uint32_t MP_CALL mock_mediaplayer_last_first_byte(void)
{ return g_last_first_byte; }

MP_EXPORT void MP_CALL mock_mediaplayer_set_unavailable(uint32_t unavailable)
{ g_mock_unavailable = unavailable != 0; }

MP_EXPORT void MP_CALL mock_mediaplayer_invalidate_screen(void)
{ g_mock_exists = false; }

MP_EXPORT void MP_CALL mock_mediaplayer_set_fail_commit(uint32_t fail)
{ g_mock_fail_commit = fail != 0; }

MP_EXPORT uint32_t MP_CALL mock_mediaplayer_abort_count(void)
{ return g_mock_abort_count; }

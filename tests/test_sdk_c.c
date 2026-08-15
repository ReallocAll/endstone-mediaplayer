#include "endstone_mediaplayer_api.h"

#include <stdio.h>

static_assert(sizeof(mp_pixel_format) == 4, "pixel format width");
static_assert(sizeof(mp_backend) == 4, "backend width");
static_assert(offsetof(mp_api_v1, screen_list) == 56, "screen list ABI");

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int fail(const char *message)
{
    fprintf(stderr, "SDK C failure: %s\n", message);
    return 1;
}

int main(void)
{
    mp_api_v1 fallback = mp_get_api_v1();
    mp_screen_handle handle = UINT64_C(42);
    if (mp_try_get_api_v1(&fallback) ||
        fallback.screen_find("missing", &handle) != MP_ERR_UNAVAILABLE ||
        handle != MP_INVALID_SCREEN_HANDLE ||
        fallback.screen_list(nullptr, 0, nullptr) != MP_ERR_UNAVAILABLE)
        return fail("missing-provider fallback");

#if defined(_WIN32)
    char temporary[MAX_PATH];
    char shadow[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, temporary) ||
        snprintf(shadow, sizeof(shadow), "%sendstone_mediaplayer-sdk-shadow-%lu.dll",
            temporary, (unsigned long)GetCurrentProcessId()) <= 0 ||
        !CopyFileA("endstone_mediaplayer.dll", shadow, FALSE))
        return fail("shadow provider copy");
    HMODULE module = LoadLibraryA(shadow);
    if (!module) return fail("actual provider load");
#else
    void *module = dlopen("./endstone_mediaplayer.so", RTLD_NOW | RTLD_GLOBAL);
    if (!module) return fail("actual provider load");
#endif
    mp_api_v1 actual;
    if (!mp_try_get_api_v1(&actual)) return fail("actual provider export resolution");
    if (actual.screen_find("missing", &handle) != MP_ERR_UNAVAILABLE ||
        handle != MP_INVALID_SCREEN_HANDLE)
        return fail("inactive actual provider behavior");
    if (actual.result_string(MP_ERR_UNAVAILABLE) == nullptr)
        return fail("actual result string");
#if defined(_WIN32)
    FreeLibrary(module);
    DeleteFileA(shadow);
#endif
    return 0;
}

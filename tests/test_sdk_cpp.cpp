#include "endstone_mediaplayer_api.h"

#include <cstdint>

static_assert(sizeof(mp_pixel_format) == 4, "pixel format width");
static_assert(sizeof(mp_backend) == 4, "backend width");
static_assert(offsetof(mp_api_v1, screen_list) == 56, "screen list ABI");

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int main()
{
#if defined(_WIN32)
    HMODULE module = LoadLibraryA("mock_provider/endstone_mediaplayer.dll");
    if (!module) return 1;
#else
    void *module = dlopen("./mock_provider/endstone_mediaplayer.so",
                          RTLD_NOW | RTLD_GLOBAL);
    if (!module) return 1;
#endif
    mp_api_v1 mp = mp_get_api_v1();
    mp_screen_handle handle = 0;
    if (mp.instance_id != UINT64_C(0x4242) ||
        mp.screen_find("mock-screen", &handle) != MP_OK ||
        handle != UINT64_C(0xfeed1234) ||
        mp.screen_list(nullptr, 0, nullptr) != MP_OK)
        return 2;
    return mp.result_string(MP_OK) == nullptr ? 3 : 0;
}

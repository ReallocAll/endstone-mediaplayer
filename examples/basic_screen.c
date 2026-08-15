// Minimal C23 consumer of the MediaPlayer C SDK.

#include "endstone_mediaplayer_api.h"

#include <stdio.h>

static int report_result(const mp_api_v1 *mp, const char *operation,
    mp_result result)
{
    if (result == MP_OK) return 0;
    fprintf(stderr, "%s failed: %s (%d)\n", operation,
        mp->result_string(result), (int)result);
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "live";
    mp_api_v1 mp = mp_get_api_v1();
    mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
    mp_result result = mp.screen_find(name, &screen);
    if (report_result(&mp, "screen_find", result) != 0) return 1;

    mp_screen_info info = {0};
    info.struct_size = sizeof(info);
    result = mp.screen_get_info(screen, &info);
    if (report_result(&mp, "screen_get_info", result) != 0) return 1;
    printf("%s: %ux%u pixels\n", info.name, info.pixel_width, info.pixel_height);

    result = mp.screen_clear(screen);
    return report_result(&mp, "screen_clear", result);
}

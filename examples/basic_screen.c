// C23 MediaPlayer SDK example.
// Call basic_screen_example() from an Endstone consumer plugin's main-thread
// enable callback after declaring a hard dependency on "mediaplayer".  This
// is intentionally not a standalone executable: the provider lives inside BDS.

#include "endstone_mediaplayer_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int report_result(const mp_api_v1 *mp, const char *operation,
    mp_result result)
{
    if (result == MP_OK) return 0;
    fprintf(stderr, "%s failed: %s (%d)\n", operation,
        mp->result_string(result), (int)result);
    return 1;
}

static mp_result find_or_create_screen(const mp_api_v1 *mp, const char *name,
                                       mp_screen_handle *out)
{
    mp_result result = mp->screen_find(name, out);
    if (result != MP_ERR_NOT_FOUND) return result;

    mp_screen_create_info create = {
        .struct_size = sizeof(create),
        .flags = 0,
        .name = name,
        .width_tiles = 1,
        .height_tiles = 1,
        .backend = MP_BACKEND_LOGICAL,
    };
    result = mp->screen_create(&create, out);
    // Another main-thread consumer may have created the name first.
    return result == MP_ERR_ALREADY_EXISTS
               ? mp->screen_find(name, out)
               : result;
}

int basic_screen_example(void)
{
    mp_api_v1 mp;
    if (!mp_try_get_api_v1(&mp)) {
        fprintf(stderr, "MediaPlayer provider is not loaded or enabled\n");
        return 1;
    }

    mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
    mp_result result = find_or_create_screen(&mp, "sdk-demo", &screen);
    if (report_result(&mp, "find_or_create_screen", result) != 0) return 1;

    mp_screen_info info = {
        .struct_size = sizeof(info),
        .flags = 0,
    };
    result = mp.screen_get_info(screen, &info);
    if (report_result(&mp, "screen_get_info", result) != 0) return 1;
    printf("%s: %ux%u pixels (backend %u)\n", info.name,
           info.pixel_width, info.pixel_height, info.backend);

    const uint32_t width = 16;
    const uint32_t height = 8;
    const uint64_t stride = (uint64_t)width * 4;
    const uint64_t bytes = (uint64_t)(height - 1) * stride + width * 4;
    uint8_t *rgba = malloc((size_t)bytes);
    if (!rgba) {
        fprintf(stderr, "pixel allocation failed\n");
        return 1;
    }
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t offset = (size_t)y * (size_t)stride + (size_t)x * 4;
            rgba[offset + 0] = (uint8_t)(x * 16); // R
            rgba[offset + 1] = (uint8_t)(y * 32); // G
            rgba[offset + 2] = 160;               // B
            rgba[offset + 3] = 255;               // A
        }
    }

    result = mp.screen_update_region(screen, 0, 0, width, height, rgba,
                                     bytes, stride, MP_PIXEL_FORMAT_RGBA);
    if (report_result(&mp, "screen_update_region", result) != 0) {
        free(rgba);
        return 1;
    }

    // Stage two regions and expose them atomically.
    mp_frame_handle frame = MP_INVALID_FRAME_HANDLE;
    result = mp.frame_begin(screen, &frame);
    if (result == MP_OK)
        result = mp.frame_update_region(frame, 0, 8, 8, height, rgba, bytes,
                                        stride, MP_PIXEL_FORMAT_RGBA);
    if (result == MP_OK)
        result = mp.frame_update_region(frame, 8, 8, 8, height, rgba + 8 * 4,
                                        bytes - 8 * 4, stride,
                                        MP_PIXEL_FORMAT_RGBA);
    if (result == MP_OK)
        result = mp.frame_commit(frame);
    if (result != MP_OK) {
        mp_result primary = result;
        if (frame != MP_INVALID_FRAME_HANDLE)
            (void)mp.frame_abort(frame);
        free(rgba);
        return report_result(&mp, "atomic frame", primary);
    }

    mp_stats stats = {
        .struct_size = sizeof(stats),
        .flags = 0,
    };
    result = mp.screen_get_stats(screen, &stats);
    free(rgba);
    if (report_result(&mp, "screen_get_stats", result) != 0) return 1;
    printf("generation=%llu resident=%llu pending=%llu\n",
           (unsigned long long)stats.generation,
           (unsigned long long)stats.resident_tiles,
           (unsigned long long)stats.pending_tiles);
    // Keep the screen and its pixels. screen_clear would erase the content;
    // screen_delete would invalidate the handle.
    return 0;
}

#include "mediaplayer/video/video_args.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int mpv_parse_int(const char *text, int *out_value)
{
    if (!text || !out_value || !text[0])
        return 0;
    // Reject leading whitespace and '+' so command arguments stay
    // strictly decimal; strtol would otherwise accept " 7" and "+7".
    if (text[0] != '-' && (text[0] < '0' || text[0] > '9'))
        return 0;

    errno = 0;
    char *end = nullptr;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return 0;
    if (value < INT_MIN || value > INT_MAX)
        return 0;

    *out_value = (int)value;
    return 1;
}

int mpv_parse_loop(const char *text, int *out_loop)
{
    if (!out_loop)
        return 0;
    *out_loop = 1;
    if (!text)
        return 0;

    int value = 0;
    if (!mpv_parse_int(text, &value))
        return 0;
    if (value != -1 && value < 1)
        return 0;

    *out_loop = value;
    return 1;
}

int mpv_parse_index(const char *text, int count, int *out_index)
{
    if (!out_index)
        return 0;

    int value = 0;
    if (!mpv_parse_int(text, &value))
        return 0;
    if (value < 0 || value >= count)
        return 0;

    *out_index = value;
    return 1;
}

int mpv_video_fits_screen(int video_width, int video_height,
                          int screen_width, int screen_height)
{
    return video_width == screen_width && video_height == screen_height;
}

#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_ARGS_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_ARGS_H

// Parses a strict decimal integer into int range.
int mpv_parse_int(const char *text, int *out_value);

// Parses -1 for infinite playback or a positive loop count.
int mpv_parse_loop(const char *text, int *out_loop);

// Parses a catalog index in [0, count).
int mpv_parse_index(const char *text, int count, int *out_index);

// A video may only play on a screen with identical tile dimensions.
int mpv_video_fits_screen(int video_width, int video_height,
                          int screen_width, int screen_height);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_ARGS_H

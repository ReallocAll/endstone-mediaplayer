#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_SESSION_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_SESSION_H

#include "mediaplayer/video/video_format.h"
#include "mediaplayer/screen/screen_registry.h"
#include <stdbool.h>
#include <stdint.h>

enum play_state {
    PLAY_STOPPED = 0,
    PLAY_PLAYING,
    PLAY_PAUSED,
    PLAY_FINISHED,
};

struct video_session {
    int active;
    enum play_state state;

    struct mcv_file mcv;
    char video_path[512];

    // Monotonic playback timing.
    int64_t start_ms;
    int64_t pause_start_ms;
    int64_t accumulated_pause;
    double frame_duration_ms;

    uint32_t current_frame;
    uint32_t frame_count;
    uint32_t skipped_frames;

    // -1 repeats forever; positive values are total play counts.
    int loop_total;
    int loop_current;

    // ABGR frame buffer.
    uint8_t *frame_buf;
    size_t frame_buf_size;

    // Owning screen runtime identity.
    uint64_t screen_runtime_id;
};

#define VIDEO_SESSION_MAX SCREEN_REGISTRY_MAX

struct video_engine {
    struct video_session sessions[VIDEO_SESSION_MAX];
};

struct video_tick_result {
    bool frame_changed;
    bool loop_changed;
    bool finished;
    uint32_t frame;
    int loop_current;
    int64_t loop_elapsed_ms;
};

void video_engine_init(struct video_engine *eng);
void video_engine_shutdown(struct video_engine *eng);

// Returns the screen's live session or nullptr.
struct video_session *video_engine_find(struct video_engine *eng,
                                        uint64_t screen_runtime_id);

// Reserves a session slot or returns nullptr.
struct video_session *video_engine_acquire(struct video_engine *eng,
                                           uint64_t screen_runtime_id);

// Stops and releases a screen's session.
void video_engine_release(struct video_engine *eng,
                          uint64_t screen_runtime_id);

// Starts playback on a screen.
int video_session_start(struct video_session *s, const char *video_path,
                        int loop, uint64_t screen_runtime_id, int64_t now_ms);

// Starts playback at a persisted frame and loop position.  The checkpoint
// values are validated against the opened video before the session becomes
// active.
int video_session_restore(struct video_session *s, const char *video_path,
                          int loop_total, int loop_current,
                          uint32_t frame, uint64_t screen_runtime_id,
                          int64_t now_ms);

// Stops playback and releases resources.
void video_session_stop(struct video_session *s);

void video_session_pause(struct video_session *s, int64_t now_ms);
void video_session_resume(struct video_session *s, int64_t now_ms);

// Advances playback and returns whether a new frame is ready.
int video_session_tick(struct video_session *s, int64_t now_ms, uint32_t *out_frame);

// Advances playback and reports frame and loop transitions.
void video_session_tick_detailed(struct video_session *s, int64_t now_ms,
                                 struct video_tick_result *result);

// Loads a frame into the session buffer.
int video_session_load_frame(struct video_session *s, uint32_t frame_idx);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_SESSION_H

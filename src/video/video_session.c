#include "mediaplayer/video/video_session.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void video_engine_init(struct video_engine *eng)
{
    memset(eng, 0, sizeof(*eng));
}

void video_engine_shutdown(struct video_engine *eng)
{
    for (int i = 0; i < VIDEO_SESSION_MAX; i++) {
        if (eng->sessions[i].active)
            video_session_stop(&eng->sessions[i]);
    }
}

struct video_session *video_engine_find(struct video_engine *eng,
                                        uint64_t screen_runtime_id)
{
    if (!eng || screen_runtime_id == 0)
        return nullptr;
    for (int i = 0; i < VIDEO_SESSION_MAX; i++) {
        if (eng->sessions[i].active &&
            eng->sessions[i].screen_runtime_id == screen_runtime_id)
            return &eng->sessions[i];
    }
    return nullptr;
}

struct video_session *video_engine_acquire(struct video_engine *eng,
                                           uint64_t screen_runtime_id)
{
    if (!eng || screen_runtime_id == 0)
        return nullptr;

    struct video_session *existing = video_engine_find(eng, screen_runtime_id);
    if (existing) {
        video_session_stop(existing);
        return existing;
    }
    for (int i = 0; i < VIDEO_SESSION_MAX; i++) {
        if (!eng->sessions[i].active)
            return &eng->sessions[i];
    }
    return nullptr;
}

void video_engine_release(struct video_engine *eng, uint64_t screen_runtime_id)
{
    struct video_session *s = video_engine_find(eng, screen_runtime_id);
    if (s)
        video_session_stop(s);
}

int video_session_start(struct video_session *s, const char *video_path,
                        int loop, uint64_t screen_runtime_id, int64_t now_ms)
{
    if (s->active)
        video_session_stop(s);

    memset(s, 0, sizeof(*s));

    enum mcv_error err = mcv_open(video_path, &s->mcv);
    if (err != MCV_OK)
        return (int)err;

    size_t buf_size = (size_t)s->mcv.raw_frame_size;
    if (buf_size > MCV_MAX_FRAME_SIZE) {
        mcv_close(&s->mcv);
        return -1;
    }
    s->frame_buf = malloc(buf_size);
    if (!s->frame_buf) {
        mcv_close(&s->mcv);
        return -1;
    }
    s->frame_buf_size = buf_size;

    size_t plen = strlen(video_path);
    if (plen >= sizeof(s->video_path))
        plen = sizeof(s->video_path) - 1;
    memcpy(s->video_path, video_path, plen);
    s->video_path[plen] = '\0';

    s->active = 1;
    s->state = PLAY_PLAYING;
    s->start_ms = now_ms;
    s->pause_start_ms = 0;
    s->accumulated_pause = 0;
    s->frame_duration_ms = mcv_frame_duration_ms(&s->mcv);
    s->current_frame = 0;
    s->frame_count = (uint32_t)s->mcv.header.frame_count;
    s->skipped_frames = 0;
    // Invalid loop counts play once.
    s->loop_total = (loop == -1 || loop > 0) ? loop : 1;
    s->loop_current = 1;
    s->screen_runtime_id = screen_runtime_id;

    return video_session_load_frame(s, 0);
}

void video_session_stop(struct video_session *s)
{
    if (!s->active)
        return;

    s->state = PLAY_STOPPED;
    s->active = 0;

    mcv_close(&s->mcv);

    if (s->frame_buf) {
        free(s->frame_buf);
        s->frame_buf = nullptr;
    }
    s->frame_buf_size = 0;
}

void video_session_pause(struct video_session *s, int64_t now_ms)
{
    if (!s->active || s->state != PLAY_PLAYING)
        return;
    s->state = PLAY_PAUSED;
    s->pause_start_ms = now_ms;
}

void video_session_resume(struct video_session *s, int64_t now_ms)
{
    if (!s->active || s->state != PLAY_PAUSED)
        return;
    s->accumulated_pause += now_ms - s->pause_start_ms;
    s->pause_start_ms = 0;
    s->state = PLAY_PLAYING;
}

void video_session_tick_detailed(struct video_session *s, int64_t now_ms,
                                 struct video_tick_result *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof(*result));
    if (!s->active || s->state != PLAY_PLAYING)
        return;

    int64_t effective_now = now_ms - s->accumulated_pause;
    int64_t elapsed = effective_now - s->start_ms;
    if (elapsed < 0)
        elapsed = 0;

    double frame_dur = s->frame_duration_ms;
    if (frame_dur <= 0.0)
        frame_dur = 50.0;

    uint32_t total_frames = s->frame_count;

    if (total_frames == 0)
        return;

    int previous_loop = s->loop_current;
    uint64_t absolute_frame = (uint64_t)((double)elapsed / frame_dur);
    uint64_t loop_index = absolute_frame / total_frames;
    uint64_t frame_in_loop = absolute_frame % total_frames;

    if (s->loop_total != -1 &&
        loop_index >= (uint64_t)s->loop_total) {
        s->state = PLAY_FINISHED;
        result->frame_changed = true;
        result->finished = true;
        result->frame = total_frames - 1;
        result->loop_current = s->loop_total;
        return;
    }

    if (loop_index >= (uint64_t)INT_MAX)
        s->loop_current = INT_MAX;
    else
        s->loop_current = (int)loop_index + 1;
    uint32_t expected = (uint32_t)frame_in_loop;
    result->loop_changed = s->loop_current != previous_loop;
    result->loop_current = s->loop_current;

    double loop_duration_ms = frame_dur * total_frames;
    double loop_elapsed = fmod((double)elapsed, loop_duration_ms);
    if (loop_elapsed < 0.0)
        loop_elapsed = 0.0;
    result->loop_elapsed_ms = (int64_t)loop_elapsed;

    if (expected > s->current_frame + 1) {
        s->skipped_frames += expected - s->current_frame - 1;
    }

    if (expected != s->current_frame || result->loop_changed) {
        s->current_frame = expected;
        result->frame_changed = true;
        result->frame = expected;
    }
}

int video_session_tick(struct video_session *s, int64_t now_ms,
                       uint32_t *out_frame)
{
    struct video_tick_result result;
    video_session_tick_detailed(s, now_ms, &result);
    if (out_frame && result.frame_changed)
        *out_frame = result.frame;
    return result.frame_changed ? 1 : 0;
}

int video_session_load_frame(struct video_session *s, uint32_t frame_idx)
{
    if (!s->active || frame_idx >= s->frame_count)
        return -1;

    enum mcv_error err = mcv_read_frame(&s->mcv, frame_idx, s->frame_buf,
                                        s->frame_buf_size);
    return (err == MCV_OK) ? 0 : (int)err;
}

#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_COMMANDS_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_COMMANDS_H

#include "mediaplayer/screen/screen_registry.h"
#include "mediaplayer/screen/screen_persistence.h"
#include "mediaplayer/video/video_catalog.h"
#include "mediaplayer/video/video_policy.h"
#include "mediaplayer/video/video_preferences.h"
#include "mediaplayer/video/video_session.h"
#include "mediaplayer/image/mps_catalog.h"
#include "mediaplayer/image/mps_source.h"
#include "mediaplayer/map/map_render.h"
#include "mediaplayer/music/music_cache.h"
#include "mediaplayer/music/music_catalog.h"
#include "mediaplayer/music/screen_audio.h"

#define MPV_RESEND_JOBS_MAX SCREEN_REGISTRY_MAX

struct video_resend_job {
    uint64_t screen_runtime_id;
    uint64_t attachment_id;
    struct presenter_resident_cursor cursor;
};

struct video_online_player {
    void *player;
    char uuid[SCREEN_UUID_LEN + 1];
    bool public_media_enabled;
    struct mpv_public_snapshot snapshot;
    struct mpv_public_membership membership;
    struct video_resend_job resend_jobs[MPV_RESEND_JOBS_MAX];
    int resend_count;
};

// Central video plugin context owned by plugin.c.
struct video_ctx {
    struct screen_registry registry;
    struct video_catalog catalog;
    struct mps_catalog image_catalog;
    struct video_engine engine;
    struct mps_source_engine image_engine;
    struct screen_audio_engine audio;
    struct map_render_ctx render;
    struct music_catalog *music_catalog;
    struct music_cache *music_cache;
    struct mpv_preferences preferences;

    char data_dir[512];
    char save_path[560];
    char preferences_path[560];

    // Online players and cached public-viewer snapshots.
    struct video_online_player online_players[64];
    int online_count;
    unsigned int public_viewer_tick;

    int active;
};

void video_ctx_init(struct video_ctx *ctx, void *server, void *plugin,
                    const char *data_dir,
                    struct music_catalog *music_catalog,
                    struct music_cache *music_cache);
void video_ctx_shutdown(struct video_ctx *ctx);

// Advances active sessions and sends frames.
void video_tick(struct video_ctx *ctx);

// Handles an /mpv command from a player or the console.
void video_handle_command(struct video_ctx *ctx,
                          int argc, const char **argv,
                          void *sender, void *player,
                          const char *player_uuid);

// Tracks online players for public screen playback.
void video_on_player_join(struct video_ctx *ctx, void *player, const char *uuid);
void video_on_player_quit(struct video_ctx *ctx, const char *uuid);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_COMMANDS_H

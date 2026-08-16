#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mediaplayer/video/video_commands.h"
#include "mediaplayer/video/video_args.h"
#include "mediaplayer/screen/screen_geometry.h"
#include "mediaplayer/bedrock/map_abi.h"
#include "mediaplayer/bedrock/world_bridge.h"
#include "mediaplayer/bedrock/world_read_abi.h"
#include "mediaplayer/endstone_api.h"
#include "mediaplayer/api_provider.h"
#include "endstone_abi.h"
#include "abi_helpers.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef MC_WHITE
#define MC_WHITE "\xc2\xa7" "f"
#endif

// --- Time source ---
#if defined(ES_PLATFORM_WINDOWS)
#include <windows.h>
static int64_t get_mono_ms(void)
{
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (int64_t)(cnt.QuadPart * 1000 / freq.QuadPart);
}
#else
#include <time.h>
static int64_t get_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

static void clear_screen_checkpoint(struct screen_entry *screen);
static void sync_screen_checkpoint(struct screen_entry *screen,
                                   const struct video_session *sess);

// --- Message helper ---
#include <stdarg.h>
static void send_err(void *sender, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    int off = snprintf(buf, sizeof(buf), "%s[MediaPlayer] %s", MC_RED, MC_GRAY);
    vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    va_end(ap);
    sender_send_message(sender, buf);
}

// --- Init / Shutdown ---

void video_ctx_init(struct video_ctx *ctx, void *server, void *plugin,
                    const char *data_dir,
                    struct music_catalog *music_catalog,
                    struct music_cache *music_cache)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->music_catalog = music_catalog;
    ctx->music_cache = music_cache;

    size_t dlen = strlen(data_dir);
    if (dlen >= sizeof(ctx->data_dir)) dlen = sizeof(ctx->data_dir) - 1;
    memcpy(ctx->data_dir, data_dir, dlen);
    ctx->data_dir[dlen] = '\0';

    snprintf(ctx->save_path, sizeof(ctx->save_path), "%s/screens.json", data_dir);
    snprintf(ctx->preferences_path, sizeof(ctx->preferences_path),
             "%s/preferences.json", data_dir);

    screen_registry_init(&ctx->registry);

    char video_dir[560];
    snprintf(video_dir, sizeof(video_dir), "%s/video", data_dir);
    video_catalog_init(&ctx->catalog, video_dir);
    mps_catalog_init(&ctx->image_catalog, video_dir);

    video_engine_init(&ctx->engine);
    mps_source_engine_init(&ctx->image_engine);
    screen_audio_engine_init(&ctx->audio);
    map_render_init(&ctx->render, server, plugin);

    // Load persisted screens.
    screen_persistence_set_log_plugin(plugin);
    screen_persistence_load(&ctx->registry, ctx->save_path, nullptr);
    mpv_preferences_init(&ctx->preferences);
    mpv_preferences_load(&ctx->preferences, ctx->preferences_path);

    ctx->active = 1;
}

void video_ctx_shutdown(struct video_ctx *ctx)
{
    if (!ctx->active) return;
    for (int i = 0; i < ctx->registry.count; i++) {
        struct screen_entry *screen = ctx->registry.screens[i];
        struct video_session *sess =
            video_engine_find(&ctx->engine, screen->runtime_id);
        if (sess && (sess->state == PLAY_PLAYING ||
                     sess->state == PLAY_PAUSED))
            sync_screen_checkpoint(screen, sess);
        else if (sess && sess->state == PLAY_FINISHED)
            clear_screen_checkpoint(screen);
    }
    ctx->active = 0;

    // Detach renderers before freeing session frame buffers.
    for (int i = 0; i < ctx->registry.count; i++)
        map_render_clear(&ctx->render, ctx->registry.screens[i]);
    screen_audio_engine_shutdown(&ctx->audio);
    video_engine_shutdown(&ctx->engine);
    mps_source_engine_shutdown(&ctx->image_engine);

    for (int i = 0; i < ctx->registry.count; i++) {
        if (ctx->registry.screens[i]->tiles_initialized)
            map_render_destroy_screen(&ctx->render, ctx->registry.screens[i]);
    }

    screen_persistence_save(&ctx->registry, ctx->save_path);
    mpv_preferences_save(&ctx->preferences, ctx->preferences_path);
    screen_registry_cleanup(&ctx->registry);
}

// --- Public viewer snapshots and membership ---

static void refresh_player_snapshot(struct video_online_player *online)
{
    struct mp_player_snapshot snapshot = {0};
    char detail[192] = {0};
    memset(&online->snapshot, 0, sizeof(online->snapshot));
    if (!online->player ||
        !mp_player_get_snapshot(online->player, &snapshot, detail,
                                (int)sizeof(detail))) {
        return;
    }

    online->snapshot.valid = true;
    online->snapshot.x = snapshot.x;
    online->snapshot.y = snapshot.y;
    online->snapshot.z = snapshot.z;
    snprintf(online->snapshot.dimension,
             sizeof(online->snapshot.dimension), "%s", snapshot.dimension_id);
}

static int collect_public_viewers(struct video_ctx *ctx,
                                  const struct screen_entry *screen,
                                  void **players, const char **player_ids)
{
    struct mpv_public_candidate candidates[64];
    for (int i = 0; i < ctx->online_count; i++) {
        candidates[i].player = ctx->online_players[i].player;
        candidates[i].uuid = ctx->online_players[i].uuid;
        candidates[i].online = true;
        candidates[i].public_media_enabled =
            ctx->online_players[i].public_media_enabled;
        candidates[i].snapshot = ctx->online_players[i].snapshot;
    }
    return mpv_collect_public_viewers(&screen->geom, candidates,
                                      ctx->online_count, players, player_ids,
                                      64);
}

static int collect_presenter_viewers(
    struct video_ctx *ctx, struct presenter_viewer out[64])
{
    int count = 0;
    for (int i = 0; i < ctx->online_count; i++) {
        struct video_online_player *online = &ctx->online_players[i];
        if (!online->player || !online->public_media_enabled ||
            !online->snapshot.valid)
            continue;
        struct presenter_viewer *viewer = &out[count++];
        memset(viewer, 0, sizeof(*viewer));
        viewer->player = online->player;
        viewer->stable_id = (uint64_t)(i + 1);
        viewer->x = online->snapshot.x;
        viewer->y = online->snapshot.y;
        viewer->z = online->snapshot.z;
        snprintf(viewer->dimension, sizeof(viewer->dimension), "%s",
                 online->snapshot.dimension);
    }
    return count;
}

static struct presenter_viewer presenter_viewer_from_online(
    const struct video_online_player *online)
{
    struct presenter_viewer viewer = {0};
    viewer.player = online->player;
    viewer.x = online->snapshot.x;
    viewer.y = online->snapshot.y;
    viewer.z = online->snapshot.z;
    snprintf(viewer.dimension, sizeof(viewer.dimension), "%s",
             online->snapshot.dimension);
    return viewer;
}

static void queue_resend(struct video_online_player *online,
                         uint64_t screen_runtime_id, uint64_t attachment_id)
{
    for (int i = 0; i < online->resend_count; i++) {
        if (online->resend_jobs[i].screen_runtime_id == screen_runtime_id) {
            online->resend_jobs[i].attachment_id = attachment_id;
            online->resend_jobs[i].cursor.index = 0;
            return;
        }
    }
    if (online->resend_count >= MPV_RESEND_JOBS_MAX)
        return;
    struct video_resend_job *job =
        &online->resend_jobs[online->resend_count++];
    memset(job, 0, sizeof(*job));
    job->screen_runtime_id = screen_runtime_id;
    job->attachment_id = attachment_id;
}

static struct screen_entry *find_screen_runtime(struct video_ctx *ctx,
                                                uint64_t runtime_id)
{
    for (int i = 0; i < ctx->registry.count; i++) {
        if (ctx->registry.screens[i]->runtime_id == runtime_id)
            return ctx->registry.screens[i];
    }
    return nullptr;
}

static void remove_resend_job(struct video_online_player *online, int index)
{
    for (int i = index; i < online->resend_count - 1; i++)
        online->resend_jobs[i] = online->resend_jobs[i + 1];
    online->resend_count--;
    memset(&online->resend_jobs[online->resend_count], 0,
           sizeof(online->resend_jobs[0]));
}

static void stop_screen_playback(struct video_ctx *ctx,
                                 struct screen_entry *screen);

static void clear_screen_checkpoint(struct screen_entry *screen)
{
    if (!screen)
        return;
    memset(&screen->playback, 0, sizeof(screen->playback));
    screen->playback.state = SCREEN_PLAYBACK_STOPPED;
    screen->playing = 0;
}

static void sync_screen_checkpoint(struct screen_entry *screen,
                                   const struct video_session *sess)
{
    if (!screen || !sess || !sess->active ||
        (sess->state != PLAY_PLAYING && sess->state != PLAY_PAUSED))
        return;
    screen->playback.state = sess->state == PLAY_PAUSED
                                 ? SCREEN_PLAYBACK_PAUSED
                                 : SCREEN_PLAYBACK_PLAYING;
    screen->playback.current_frame = sess->current_frame;
    screen->playback.loop_total = sess->loop_total;
    screen->playback.loop_current = sess->loop_current;
    screen->playing = 1;
}

static const struct video_entry *find_catalog_video(
    const struct video_catalog *catalog, const char *name)
{
    if (!catalog || !name || !name[0])
        return nullptr;
    for (int i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->entries[i].name, name) == 0)
            return &catalog->entries[i];
    }
    return nullptr;
}

static int restore_screen_video(struct video_ctx *ctx,
                                struct screen_entry *screen,
                                void *viewer, int64_t now_ms,
                                int *catalog_refreshed)
{
    if (!ctx || !screen || !viewer || !catalog_refreshed ||
        !screen->plugin_managed || !screen->tiles_initialized ||
        (screen->playback.state != SCREEN_PLAYBACK_PLAYING &&
         screen->playback.state != SCREEN_PLAYBACK_PAUSED) ||
        video_engine_find(&ctx->engine, screen->runtime_id) ||
        mps_source_find(&ctx->image_engine, screen->runtime_id))
        return 0;

    if (!*catalog_refreshed) {
        video_catalog_refresh(&ctx->catalog);
        *catalog_refreshed = 1;
    }
    const struct video_entry *video =
        find_catalog_video(&ctx->catalog, screen->playback.video_name);
    if (!video || !mpv_video_fits_screen(video->tile_width, video->tile_height,
                                         screen->geom.width,
                                         screen->geom.height))
        return 0;

    struct video_session *sess =
        video_engine_acquire(&ctx->engine, screen->runtime_id);
    if (!sess)
        return 0;

    struct screen_playback_checkpoint checkpoint = screen->playback;
    if (video_session_restore(sess, video->path, checkpoint.loop_total,
                              checkpoint.loop_current,
                              checkpoint.current_frame,
                              screen->runtime_id, now_ms) != 0) {
        video_engine_release(&ctx->engine, screen->runtime_id);
        return 0;
    }

    struct nbs_error_info nbs_error = {0};
    if (ctx->music_catalog && ctx->music_cache)
        (void)screen_audio_start(&ctx->audio, ctx->music_cache,
                                 ctx->music_catalog, screen->runtime_id,
                                 checkpoint.video_name, &nbs_error);
    if (checkpoint.state == SCREEN_PLAYBACK_PAUSED)
        video_session_pause(sess, now_ms);
    sync_screen_checkpoint(screen, sess);
    if (map_render_submit_frame(screen, sess->frame_buf, sess->frame_buf_size,
                                SURFACE_FORMAT_ABGR8888) != MAP_RENDER_OK) {
        map_render_clear(&ctx->render, screen);
        screen_audio_stop(&ctx->audio, screen->runtime_id);
        video_engine_release(&ctx->engine, screen->runtime_id);
        screen->playback = checkpoint;
        screen->playing = 1;
        return 0;
    }
    return 1;
}

static void refresh_public_viewers(struct video_ctx *ctx)
{
    bool restore_attempted[SCREEN_REGISTRY_MAX] = {0};
    bool playback_restore_attempted[SCREEN_REGISTRY_MAX] = {0};
    int catalog_refreshed = 0;
    int64_t now = get_mono_ms();
    for (int p = 0; p < ctx->online_count; p++) {
        struct video_online_player *online = &ctx->online_players[p];
        refresh_player_snapshot(online);

        uint64_t eligible_ids[SCREEN_REGISTRY_MAX];
        int eligible_count = 0;
        for (int s = 0; s < ctx->registry.count; s++) {
            struct screen_entry *screen = ctx->registry.screens[s];
            bool eligible = online->public_media_enabled &&
                mpv_public_viewer_eligible(
                    true, &online->snapshot, &screen->geom);
            enum mpv_membership_transition transition =
                mpv_membership_transition(&online->membership,
                                          screen->runtime_id, eligible);
            if (!eligible) continue;

            eligible_ids[eligible_count++] = screen->runtime_id;
            bool restored_now = false;
            if (screen->plugin_managed && !screen->tiles_initialized &&
                !restore_attempted[s]) {
                restore_attempted[s] = true;
                restored_now = map_render_restore_screen(
                    &ctx->render, screen, online->player) == MAP_RENDER_OK;
            }
            bool video_restored_now = false;
            if (screen->plugin_managed && screen->tiles_initialized &&
                !playback_restore_attempted[s]) {
                playback_restore_attempted[s] = true;
                video_restored_now = restore_screen_video(
                    ctx, screen, online->player, now, &catalog_refreshed);
            }
            if (restored_now || video_restored_now ||
                (transition == MPV_MEMBERSHIP_ENTERED &&
                 screen->tiles_initialized)) {
                struct mps_source *source = mps_source_find(
                    &ctx->image_engine, screen->runtime_id);
                queue_resend(online, screen->runtime_id,
                             source ? source->attachment_id : 0);
            }
        }
        mpv_membership_replace(&online->membership, eligible_ids,
                               eligible_count);
    }
}

static void process_resend_jobs(struct video_ctx *ctx)
{
    size_t remaining = 64;
    for (int p = 0; p < ctx->online_count && remaining > 0; p++) {
        struct video_online_player *online = &ctx->online_players[p];
        int job_index = 0;
        while (job_index < online->resend_count && remaining > 0) {
            struct video_resend_job *job = &online->resend_jobs[job_index];
            struct screen_entry *screen = find_screen_runtime(
                ctx, job->screen_runtime_id);
            if (!screen || !online->public_media_enabled ||
                !mpv_membership_contains(&online->membership,
                                         job->screen_runtime_id)) {
                remove_resend_job(online, job_index);
                continue;
            }
            struct mps_source *source = mps_source_find(
                &ctx->image_engine, screen->runtime_id);
            if (source && job->attachment_id != source->attachment_id) {
                job->attachment_id = source->attachment_id;
                job->cursor.index = 0;
            } else if (!source && job->attachment_id != 0) {
                struct video_session *replacement = video_engine_find(
                    &ctx->engine, screen->runtime_id);
                if (!replacement) {
                    remove_resend_job(online, job_index);
                    continue;
                }
                job->attachment_id = 0;
                job->cursor.index = 0;
            }
            struct presenter_viewer viewer =
                presenter_viewer_from_online(online);
            struct presenter_stats stats;
            bool done = false;
            enum map_render_error render_error;
            if (source) {
                render_error = map_render_resend_stream(
                    &ctx->render, screen, &viewer,
                    source->file.header.tile_count, &job->cursor.index,
                    remaining, mps_source_presenter_read, source, &done,
                    &stats);
            } else {
                render_error = (enum map_render_error)map_render_resend(
                    &ctx->render, screen, &viewer, &job->cursor, remaining,
                    &done, &stats);
            }
            if (render_error != MAP_RENDER_OK) {
                if (source) {
                    stop_screen_playback(ctx, screen);
                    remove_resend_job(online, job_index);
                }
                break;
            }
            if (stats.examined > remaining)
                break;
            remaining -= stats.examined;
            if (done) {
                remove_resend_job(online, job_index);
                continue;
            }
            job_index++;
        }
    }
}

// --- Playback teardown ---
// Detach renderers before freeing the session frame buffer.
static void stop_screen_playback(struct video_ctx *ctx,
                                 struct screen_entry *screen)
{
    map_render_clear(&ctx->render, screen);
    screen_audio_stop(&ctx->audio, screen->runtime_id);
    video_engine_release(&ctx->engine, screen->runtime_id);
    mps_source_release(&ctx->image_engine, screen->runtime_id);
    clear_screen_checkpoint(screen);
}

// --- Tick ---

void video_tick(struct video_ctx *ctx)
{
    if (!ctx->active) return;

    if (ctx->public_viewer_tick == 0)
        refresh_public_viewers(ctx);
    ctx->public_viewer_tick =
        (ctx->public_viewer_tick + 1) % MPV_PUBLIC_VIEWER_REFRESH_TICKS;
    process_resend_jobs(ctx);

    int64_t now = get_mono_ms();

    for (int i = 0; i < ctx->registry.count; i++) {
        struct screen_entry *screen = ctx->registry.screens[i];
        struct presenter_viewer presenter_viewers[64];
        int presenter_viewer_count = collect_presenter_viewers(
            ctx, presenter_viewers);
        struct mps_source *source = mps_source_find(
            &ctx->image_engine, screen->runtime_id);
        struct video_session *sess =
            video_engine_find(&ctx->engine, screen->runtime_id);
        if (source) {
            struct presenter_stats stats;
            bool done = false;
            enum map_render_error render_error = map_render_present_stream(
                &ctx->render, screen, presenter_viewers,
                (size_t)presenter_viewer_count,
                source->file.header.tile_count, &source->initial_cursor, 28,
                mps_source_presenter_read, source, &done, &stats);
            if (render_error != MAP_RENDER_OK)
                stop_screen_playback(ctx, screen);
            continue;
        }
        if (!sess || sess->state != PLAY_PLAYING) {
            if (sess)
                sync_screen_checkpoint(screen, sess);
            struct presenter_stats stats;
            map_render_present(&ctx->render, screen, presenter_viewers,
                               (size_t)presenter_viewer_count, 28, &stats);
            continue;
        }

        struct video_tick_result tick;
        video_session_tick_detailed(sess, now, &tick);
        if (tick.finished) {
            stop_screen_playback(ctx, screen);
            continue;
        }
        sync_screen_checkpoint(screen, sess);

        void *op[64];
        const char *oids[64];
        int viewer_count = collect_public_viewers(ctx, screen, op, oids);
        struct screen_audio_session *audio =
            screen_audio_find(&ctx->audio, screen->runtime_id);
        if (audio) {
            screen_audio_tick(audio, ctx->music_cache, tick.loop_current,
                              tick.loop_elapsed_ms, tick.loop_changed,
                              op, viewer_count);
        }

        if (tick.frame_changed) {
            if (video_session_load_frame(sess, tick.frame) != 0 ||
                map_render_submit_frame(
                    screen, sess->frame_buf, sess->frame_buf_size,
                    SURFACE_FORMAT_ABGR8888) != MAP_RENDER_OK) {
                stop_screen_playback(ctx, screen);
                continue;
            }
        }
        struct presenter_stats stats;
        if (map_render_present(&ctx->render, screen, presenter_viewers,
                               (size_t)presenter_viewer_count, 28, &stats) !=
            MAP_RENDER_OK)
            stop_screen_playback(ctx, screen);
    }
}

// --- Command handlers ---

static void cmd_help(struct video_ctx *ctx, void *sender)
{
    (void)ctx;
    sender_send_message(sender, MC_GRAY "───── " MC_GREEN "MediaPlayer " MC_AQUA "Video" MC_GRAY " ─────");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "list " MC_GRAY "[filter]");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "create " MC_GRAY "<name>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "materialize " MC_GRAY "<name>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "delete " MC_GRAY "<name>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "screens");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "info " MC_GRAY "<name>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "play " MC_GRAY "<screen> <index> [loop]");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "images " MC_GRAY "[filter]");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "image " MC_GRAY "<screen> <image-index>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "pause " MC_GRAY "<screen>" MC_GRAY " | " MC_YELLOW "resume " MC_GRAY "<screen>" MC_GRAY " | " MC_YELLOW "stop " MC_GRAY "<screen>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "status " MC_GRAY "<screen>");
    sender_send_message(sender, MC_GRAY " /mpv " MC_YELLOW "watch " MC_GRAY "[on|off]");
    sender_send_message(sender, MC_GRAY "── " MC_AQUA "Loop" MC_GRAY " ──  " MC_GRAY "-1=infinite  1=once  N=times");
    sender_send_message(sender, MC_GRAY "── " MC_AQUA "Facing" MC_GRAY " ──  " MC_GRAY "detected from the backing wall");
    sender_send_message(sender, MC_GRAY "── " MC_AQUA "Access" MC_GRAY " ──  " MC_GRAY "public within 16 blocks");
}

static void cmd_list(struct video_ctx *ctx, void *sender, int argc, const char **argv)
{
    video_catalog_refresh(&ctx->catalog);
    const char *filter = (argc > 1) ? argv[1] : nullptr;

    int shown = 0;
    for (int i = 0; i < ctx->catalog.count; i++) {
        const struct video_entry *e = &ctx->catalog.entries[i];
        if (filter && strstr(e->name, filter) == nullptr)
            continue;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 MC_GREEN "[MediaPlayer] " MC_GRAY "[%d] " MC_YELLOW "%s "
                 MC_GRAY "(%dx%d, %llu frames, %dfps)",
                 i, e->name, e->tile_width, e->tile_height,
                 (unsigned long long)e->frame_count,
                 e->fps_num / (e->fps_den ? e->fps_den : 1));
        sender_send_message(sender, buf);
        shown++;
    }
    if (shown == 0) {
        sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "No .mcv files in video folder");
    } else if (filter) {
        char buf[64];
        snprintf(buf, sizeof(buf), MC_GREEN "[MediaPlayer] " MC_GRAY "[MediaPlayer] " MC_GRAY "%d matched", shown);
        sender_send_message(sender, buf);
    }
}

static void cmd_images(struct video_ctx *ctx, void *sender, int argc,
                       const char **argv)
{
    mps_catalog_refresh(&ctx->image_catalog);
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int shown = 0;
    for (int i = 0; i < ctx->image_catalog.count; i++) {
        const struct mps_image_entry *entry =
            &ctx->image_catalog.entries[i];
        if (filter && strstr(entry->name, filter) == nullptr)
            continue;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 MC_GREEN "[MediaPlayer] " MC_GRAY "[%d] " MC_YELLOW
                 "%s " MC_GRAY "(%ux%u tiles)", i, entry->name,
                 entry->tile_width, entry->tile_height);
        sender_send_message(sender, buf);
        shown++;
    }
    if (shown == 0) {
        sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY
                             "No valid .mps files in video folder");
    } else if (filter) {
        char buf[96];
        snprintf(buf, sizeof(buf), MC_GRAY "%d matched", shown);
        sender_send_message(sender, buf);
    }
}

static struct screen_pos adjacent_backing(struct screen_pos cell,
                                          enum screen_facing facing)
{
    switch (facing) {
    case SCREEN_FACE_SOUTH: cell.z--; break;
    case SCREEN_FACE_NORTH: cell.z++; break;
    case SCREEN_FACE_EAST: cell.x--; break;
    case SCREEN_FACE_WEST: cell.x++; break;
    }
    return cell;
}

static struct screen_pos screen_cell_for_backing(struct screen_pos backing,
                                                  enum screen_facing facing)
{
    switch (facing) {
    case SCREEN_FACE_SOUTH: backing.z++; break;
    case SCREEN_FACE_NORTH: backing.z--; break;
    case SCREEN_FACE_EAST: backing.x++; break;
    case SCREEN_FACE_WEST: backing.x--; break;
    }
    return backing;
}

static int same_pos(struct screen_pos a, struct screen_pos b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static int discover_backing_geometry(
    void *player, const struct mp_player_snapshot *snapshot,
    struct screen_geom *geom, char *detail, size_t detail_size)
{
    struct screen_pos seed = {
        snapshot->block_x, snapshot->block_y, snapshot->block_z
    };
    struct mp_world_block_probe seed_probe = {0};
    if (mp_world_probe_block(player, snapshot->dimension_id, seed,
                             &seed_probe, detail, (int)detail_size) !=
        MP_WORLD_OK)
        return -1;
    if (!seed_probe.is_air) {
        snprintf(detail, detail_size,
                 "Player seed (%d,%d,%d) is %s, not air",
                 seed.x, seed.y, seed.z, seed_probe.block_type);
        return -1;
    }

    const enum screen_facing facings[4] = {
        SCREEN_FACE_SOUTH, SCREEN_FACE_NORTH,
        SCREEN_FACE_EAST, SCREEN_FACE_WEST
    };
    enum screen_facing facing = SCREEN_FACE_SOUTH;
    struct screen_pos seed_backing = {0};
    int adjacent_count = 0;
    for (int i = 0; i < 4; i++) {
        struct screen_pos position = adjacent_backing(seed, facings[i]);
        struct mp_world_block_probe probe = {0};
        if (mp_world_probe_block(player, snapshot->dimension_id, position,
                                 &probe, detail, (int)detail_size) !=
            MP_WORLD_OK)
            return -1;
        if (probe.support_candidate) {
            adjacent_count++;
            facing = facings[i];
            seed_backing = position;
        }
    }
    if (adjacent_count == 0) {
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
        snprintf(detail, detail_size,
                 "No backing block touches player seed (%d,%d,%d); run /mpv debug backing",
                 seed.x, seed.y, seed.z);
#else
        snprintf(detail, detail_size,
                 "No backing block touches player seed (%d,%d,%d)",
                 seed.x, seed.y, seed.z);
#endif
        return -1;
    }
    if (adjacent_count > 1) {
        snprintf(detail, detail_size,
                 "Player seed touches %d backing candidates; leave exactly one horizontal side connected",
                 adjacent_count);
        return -1;
    }

    struct screen_pos blocks[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT] = {0};
    int head = 0;
    int count = 1;
    blocks[0] = seed_backing;
    int min_h = (facing == SCREEN_FACE_SOUTH || facing == SCREEN_FACE_NORTH)
                    ? seed_backing.x : seed_backing.z;
    int max_h = min_h;
    int min_y = seed_backing.y;
    int max_y = seed_backing.y;

    while (head < count) {
        struct screen_pos current = blocks[head++];
        struct screen_pos neighbours[4] = {
            current, current, current, current
        };
        if (facing == SCREEN_FACE_SOUTH || facing == SCREEN_FACE_NORTH) {
            neighbours[0].x--;
            neighbours[1].x++;
        } else {
            neighbours[0].z--;
            neighbours[1].z++;
        }
        neighbours[2].y--;
        neighbours[3].y++;

        for (int n = 0; n < 4; n++) {
            // The player's feet-level air cell defines the screen bottom.
            // Floors and backing connected below it are construction support,
            // not part of the discovered display rectangle.
            if (neighbours[n].y < seed.y) continue;

            int seen = 0;
            for (int i = 0; i < count; i++) {
                if (same_pos(blocks[i], neighbours[n])) {
                    seen = 1;
                    break;
                }
            }
            if (seen) continue;

            struct mp_world_block_probe probe = {0};
            if (mp_world_probe_block(player, snapshot->dimension_id,
                                     neighbours[n], &probe, detail,
                                     (int)detail_size) != MP_WORLD_OK)
                return -1;
            if (!probe.support_candidate) continue;

            int h = (facing == SCREEN_FACE_SOUTH ||
                     facing == SCREEN_FACE_NORTH)
                        ? neighbours[n].x : neighbours[n].z;
            int next_min_h = h < min_h ? h : min_h;
            int next_max_h = h > max_h ? h : max_h;
            int next_min_y = neighbours[n].y < min_y ? neighbours[n].y : min_y;
            int next_max_y = neighbours[n].y > max_y ? neighbours[n].y : max_y;
            if (next_max_h - next_min_h + 1 > SCREEN_MAX_WIDTH) {
                snprintf(detail, detail_size,
                         "Backing width exceeds %d near (%d,%d,%d)",
                         SCREEN_MAX_WIDTH, neighbours[n].x,
                         neighbours[n].y, neighbours[n].z);
                return -1;
            }
            if (next_max_y - next_min_y + 1 > SCREEN_MAX_HEIGHT) {
                snprintf(detail, detail_size,
                         "Backing height exceeds %d near (%d,%d,%d)",
                         SCREEN_MAX_HEIGHT, neighbours[n].x,
                         neighbours[n].y, neighbours[n].z);
                return -1;
            }
            if (count >= SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT) {
                snprintf(detail, detail_size,
                         "Backing contains more than %d blocks",
                         SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT);
                return -1;
            }
            blocks[count++] = neighbours[n];
            min_h = next_min_h;
            max_h = next_max_h;
            min_y = next_min_y;
            max_y = next_max_y;
        }
    }

    int width = max_h - min_h + 1;
    int height = max_y - min_y + 1;
    if (count != width * height) {
        snprintf(detail, detail_size,
                 "Backing is not a complete rectangle (%d connected blocks, bounds %dx%d)",
                 count, width, height);
        return -1;
    }

    struct screen_pos p1 = seed;
    struct screen_pos p2 = seed;
    p1.y = max_y;
    p2.y = min_y;
    if (facing == SCREEN_FACE_SOUTH || facing == SCREEN_FACE_NORTH) {
        p1.x = min_h;
        p2.x = max_h;
        p1.z = p2.z = screen_cell_for_backing(seed_backing, facing).z;
    } else {
        p1.z = min_h;
        p2.z = max_h;
        p1.x = p2.x = screen_cell_for_backing(seed_backing, facing).x;
    }

    enum screen_geom_err geometry_result = screen_geom_validate(
        p1, p2, snapshot->dimension_id, snapshot->dimension_id,
        facing, geom);
    if (geometry_result != SCREEN_GEOM_OK) {
        snprintf(detail, detail_size,
                 "Discovered backing geometry failed validation (%d)",
                 (int)geometry_result);
        return -1;
    }

    for (int row = 0; row < geom->height; row++) {
        for (int col = 0; col < geom->width; col++) {
            int tile = screen_geom_tile_index(geom, col, row);
            struct screen_pos cell = screen_geom_tile_pos(geom, col, row);
            struct mp_world_block_probe probe = {0};
            if (mp_world_probe_block(player, snapshot->dimension_id, cell,
                                     &probe, detail, (int)detail_size) !=
                MP_WORLD_OK)
                return -1;
            if (!probe.is_air) {
                snprintf(detail, detail_size,
                         "Tile %d at (%d,%d,%d) is %s, not air",
                         tile, cell.x, cell.y, cell.z, probe.block_type);
                return -1;
            }
        }
    }

    return 0;
}

static void cmd_screen_create(struct video_ctx *ctx, void *sender, void *player,
                              const char *player_uuid, int argc, const char **argv)
{
    if (!player_uuid || !player) {
        send_err(sender, "This command requires a player.");
        return;
    }
    if (argc < 2) {
        send_err(sender, "Usage: /mpv create <name>");
        return;
    }

    const char *name = argv[1];
    if (!screen_name_valid(name)) {
        send_err(sender, "Invalid screen name. Use letters, digits, _ and -.");
        return;
    }
    if (screen_registry_find(&ctx->registry, name) >= 0) {
        send_err(sender, "Screen '%s' already exists.", name);
        return;
    }
    if (ctx->registry.count >= SCREEN_REGISTRY_MAX) {
        send_err(sender, "Screen registry is full.");
        return;
    }

    struct mp_player_snapshot snapshot = {0};
    char detail[256] = {0};
    if (!mp_player_get_snapshot(player, &snapshot, detail,
                                (int)sizeof(detail))) {
        send_err(sender, "Unable to resolve player location: %s.",
                 detail[0] ? detail : "unknown bridge error");
        return;
    }

    struct screen_geom geom = {0};
    if (discover_backing_geometry(player, &snapshot, &geom,
                                  detail, sizeof(detail)) != 0) {
        send_err(sender, "%s.", detail);
        return;
    }

    int tile_count = screen_geom_tile_count(&geom);
    struct screen_pos seed_cell = {
        snapshot.block_x, snapshot.block_y, snapshot.block_z
    };
    struct screen_pos seed_backing = adjacent_backing(seed_cell, geom.facing);
    if (!mp_world_managed_frames_supported()) {
        send_err(sender,
                 "Detected %dx%d backing from (%d,%d,%d), facing %s; "
                 "Endstone 0.11 item-frame map insertion is not implemented yet.",
                 geom.width, geom.height,
                 seed_backing.x, seed_backing.y, seed_backing.z,
                 screen_facing_name(geom.facing));
        return;
    }

    int available_slots = 0;
    enum mp_world_result capacity = mp_world_check_inventory_capacity(
        player, tile_count, &available_slots, detail, (int)sizeof(detail));
    if (capacity != MP_WORLD_OK) {
        send_err(sender, "Clear at least %d inventory slots before creation: %s.",
                 tile_count, detail[0] ? detail : mp_world_result_name(capacity));
        return;
    }

    struct screen_entry draft = {0};
    snprintf(draft.name, sizeof(draft.name), "%s", name);
    snprintf(draft.owner_uuid, sizeof(draft.owner_uuid), "%s", player_uuid);
    draft.geom = geom;
    if (screen_entry_materialize_tiles(&draft) != SCREEN_OK) {
        send_err(sender, "Unable to allocate screen tile storage.");
        return;
    }
    if (map_render_init_screen(&ctx->render, &draft, player) !=
        MAP_RENDER_OK) {
        screen_entry_cleanup_tiles(&draft);
        send_err(sender, "Unable to allocate all %d maps.", tile_count);
        return;
    }

    struct mp_world_prepared_tile *prepared[
        SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT] = {0};
    int prepared_count = 0;
    for (int tile = 0; tile < tile_count; tile++) {
        int col = tile % geom.width;
        int row = tile / geom.width;
        struct screen_pos cell = screen_geom_tile_pos(&geom, col, row);
        struct screen_pos backing = screen_geom_backing_pos(&geom, col, row);
        detail[0] = '\0';
        enum mp_world_result result = mp_world_prepare_tile(
            ctx->render.server, player, geom.dimension, cell, backing,
            geom.facing, draft.tiles[tile].map_view,
            draft.tiles[tile].map_id, name, tile, tile_count, row, col,
            &prepared[tile], detail, (int)sizeof(detail));
        if (result != MP_WORLD_OK) {
            for (int i = 0; i < prepared_count; i++)
                mp_world_prepared_destroy(prepared[i]);
            map_render_destroy_screen(&ctx->render, &draft);
            screen_entry_cleanup_tiles(&draft);
            send_err(sender, "Unable to prepare tile %d: %s.", tile,
                     detail[0] ? detail : mp_world_result_name(result));
            return;
        }
        prepared_count++;
    }

    int placed_count = 0;
    for (int tile = 0; tile < tile_count; tile++) {
        detail[0] = '\0';
        enum mp_world_result result = mp_world_place_prepared(
            prepared[tile], detail, (int)sizeof(detail));
        if (result != MP_WORLD_OK) {
            for (int i = 0; i < tile_count; i++) {
                mp_world_prepared_destroy(prepared[i]);
                prepared[i] = nullptr;
            }
            for (int i = 0; i < placed_count; i++) {
                int col = i % geom.width;
                int row = i / geom.width;
                mp_world_rollback_placed(
                    ctx->render.server, player, geom.dimension,
                    screen_geom_tile_pos(&geom, col, row));
            }
            map_render_destroy_screen(&ctx->render, &draft);
            screen_entry_cleanup_tiles(&draft);
            send_err(sender,
                     "Item-frame placement failed at tile %d; rolled back %d tiles: %s.",
                     tile, placed_count,
                     detail[0] ? detail : mp_world_result_name(result));
            return;
        }
        placed_count++;
    }

    detail[0] = '\0';
    enum mp_world_result delivered = mp_world_deliver_prepared_maps(
        player, prepared, tile_count, detail, (int)sizeof(detail));
    if (delivered != MP_WORLD_OK) {
        for (int tile = 0; tile < placed_count; tile++) {
            int col = tile % geom.width;
            int row = tile / geom.width;
            mp_world_rollback_placed(
                ctx->render.server, player, geom.dimension,
                screen_geom_tile_pos(&geom, col, row));
        }
        for (int i = 0; i < tile_count; i++)
            mp_world_prepared_destroy(prepared[i]);
        map_render_destroy_screen(&ctx->render, &draft);
        screen_entry_cleanup_tiles(&draft);
        send_err(sender, "Map delivery failed; frame placement was rolled back: %s.",
                 detail[0] ? detail : mp_world_result_name(delivered));
        return;
    }

    int index = 0;
    enum screen_error registry_result = screen_registry_create(
        &ctx->registry, name, player_uuid, &geom, &index);
    if (registry_result != SCREEN_OK) {
        mp_world_retract_prepared_maps(player, prepared, tile_count);
        for (int tile = 0; tile < tile_count; tile++) {
            int col = tile % geom.width;
            int row = tile / geom.width;
            mp_world_rollback_placed(
                ctx->render.server, player, geom.dimension,
                screen_geom_tile_pos(&geom, col, row));
        }
        map_render_destroy_screen(&ctx->render, &draft);
        screen_entry_cleanup_tiles(&draft);
        for (int i = 0; i < tile_count; i++)
            mp_world_prepared_destroy(prepared[i]);
        send_err(sender, "Screen registration failed; creation was rolled back.");
        return;
    }

    struct screen_entry *committed = ctx->registry.screens[index];
    committed->plugin_managed = 1;
    committed->tiles = draft.tiles;
    committed->tiles_capacity = draft.tiles_capacity;
    draft.tiles = nullptr;
    draft.tiles_capacity = 0;
    committed->tiles_initialized = 1;
    draft.tiles_initialized = 0;
    if (screen_persistence_save(&ctx->registry, ctx->save_path) != 0) {
        mp_world_retract_prepared_maps(player, prepared, tile_count);
        for (int tile = 0; tile < tile_count; tile++) {
            int col = tile % geom.width;
            int row = tile / geom.width;
            mp_world_rollback_placed(
                ctx->render.server, player, geom.dimension,
                screen_geom_tile_pos(&geom, col, row));
        }
        map_render_destroy_screen(&ctx->render, committed);
        screen_registry_delete(&ctx->registry, name);
        screen_entry_cleanup_tiles(&draft);
        for (int i = 0; i < tile_count; i++)
            mp_world_prepared_destroy(prepared[i]);
        send_err(sender, "Screen persistence failed; creation was rolled back.");
        return;
    }

    for (int i = 0; i < tile_count; i++)
        mp_world_prepared_destroy(prepared[i]);
    ctx->public_viewer_tick = 0;

    char buffer[192];
    snprintf(buffer, sizeof(buffer), MC_GREEN "[MediaPlayer] " MC_GRAY
             "Screen '%s' created (%dx%d, facing %s); %d empty frames placed.",
             name, geom.width, geom.height,
             screen_facing_name(geom.facing), tile_count);
    sender_send_message(sender, buffer);
    snprintf(buffer, sizeof(buffer), MC_YELLOW "[MediaPlayer] " MC_GRAY
             "%d labeled maps were put in your inventory. Install them "
             "left-to-right, top-to-bottom by their row/col labels.",
             tile_count);
    sender_send_message(sender, buffer);
}

static int materialize_screen_busy(struct video_ctx *ctx,
                                   const struct screen_entry *screen)
{
    if (!ctx || !screen) return 0;
    return screen->playing ||
           video_engine_find(&ctx->engine, screen->runtime_id) != nullptr ||
           mps_source_find(&ctx->image_engine, screen->runtime_id) != nullptr ||
           screen_audio_find(&ctx->audio, screen->runtime_id) != nullptr ||
           mp_api_provider_screen_has_active_frame(screen->runtime_id);
}

static void materialize_discard_stage(
    struct video_ctx *ctx, struct screen_entry *draft,
    struct mp_world_prepared_tile *const *prepared, int prepared_count,
    void *player, int placed_count, int maps_delivered)
{
    if (maps_delivered)
        mp_world_retract_prepared_maps(player, prepared, prepared_count);
    for (int i = 0; i < placed_count; i++) {
        int col = i % draft->geom.width;
        int row = i / draft->geom.width;
        mp_world_rollback_placed(
            ctx->render.server, player, draft->geom.dimension,
            screen_geom_tile_pos(&draft->geom, col, row));
    }
    for (int i = 0; i < prepared_count; i++)
        mp_world_prepared_destroy(prepared[i]);
    if (draft->tiles_initialized)
        map_render_destroy_screen(&ctx->render, draft);
    screen_entry_cleanup_tiles(draft);
}

static int materialize_restore_persisted_state(
    struct video_ctx *ctx, struct screen_entry *screen,
    const struct screen_entry *old_state)
{
    map_render_destroy_screen(&ctx->render, screen);
    screen_entry_cleanup_tiles(screen);
    screen->geom = old_state->geom;
    memcpy(screen->owner_uuid, old_state->owner_uuid,
           sizeof(screen->owner_uuid));
    screen->plugin_managed = old_state->plugin_managed;
    screen->tiles = old_state->tiles;
    screen->tiles_capacity = old_state->tiles_capacity;
    screen->tiles_initialized = old_state->tiles_initialized;
    screen->playing = old_state->playing;
    screen->playback = old_state->playback;
    return screen_persistence_save(&ctx->registry, ctx->save_path);
}

static void cmd_screen_materialize(struct video_ctx *ctx, void *sender,
                                   void *player, const char *player_uuid,
                                   int argc, const char **argv)
{
    if (!player || !player_uuid) {
        send_err(sender, "Materializing a screen requires an OP player.");
        return;
    }
    if (argc != 2) {
        send_err(sender, "Usage: /mpv materialize <name>");
        return;
    }

    int index = screen_registry_find(&ctx->registry, argv[1]);
    if (index < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }
    struct screen_entry *screen = ctx->registry.screens[index];
    if (!screen || screen->plugin_managed ||
        strcmp(screen->geom.dimension, "logical") != 0 ||
        screen->tiles != nullptr || screen->tiles_initialized) {
        send_err(sender,
                 "Screen '%s' is not an eligible logical screen; only an "
                 "API-created logical screen can be materialized.", argv[1]);
        return;
    }
    if (materialize_screen_busy(ctx, screen)) {
        send_err(sender,
                 "Screen '%s' is busy; stop playback, image/audio producers, "
                 "and active frame updates before materializing it.", argv[1]);
        return;
    }

    struct mp_player_snapshot snapshot = {0};
    char detail[256] = {0};
    if (!mp_player_get_snapshot(player, &snapshot, detail,
                                (int)sizeof(detail))) {
        send_err(sender, "Unable to resolve player location: %s.",
                 detail[0] ? detail : "unknown bridge error");
        return;
    }

    struct screen_geom geom = {0};
    if (discover_backing_geometry(player, &snapshot, &geom,
                                  detail, sizeof(detail)) != 0) {
        send_err(sender, "Backing/world validation failed: %s.",
                 detail[0] ? detail : "unknown bridge error");
        return;
    }
    if (geom.width != screen->geom.width || geom.height != screen->geom.height) {
        send_err(sender,
                 "Backing size mismatch: logical screen is %dx%d but the "
                 "discovered backing is %dx%d.",
                 screen->geom.width, screen->geom.height,
                 geom.width, geom.height);
        return;
    }
    if (!mp_world_managed_frames_supported()) {
        send_err(sender,
                 "Backing/world materialization is unavailable on this "
                 "Endstone build.");
        return;
    }

    int tile_count = screen_geom_tile_count(&geom);
    int available_slots = 0;
    enum mp_world_result capacity = mp_world_check_inventory_capacity(
        player, tile_count, &available_slots, detail, (int)sizeof(detail));
    if (capacity != MP_WORLD_OK) {
        send_err(sender, "Backing/world inventory validation failed: %s.",
                 detail[0] ? detail : mp_world_result_name(capacity));
        return;
    }

    struct screen_entry draft = {0};
    memcpy(draft.name, screen->name, sizeof(draft.name));
    draft.geom = geom;
    if (screen_entry_materialize_tiles(&draft) != SCREEN_OK) {
        send_err(sender, "Unable to allocate materialization tile storage.");
        return;
    }
    if (map_render_init_screen(&ctx->render, &draft, player) != MAP_RENDER_OK) {
        screen_entry_cleanup_tiles(&draft);
        send_err(sender, "Unable to allocate the materialization maps.");
        return;
    }

    struct mp_world_prepared_tile *prepared[
        SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT] = {0};
    int prepared_count = 0;
    for (int tile = 0; tile < tile_count; tile++) {
        int col = tile % geom.width;
        int row = tile / geom.width;
        struct screen_pos cell = screen_geom_tile_pos(&geom, col, row);
        struct screen_pos backing = screen_geom_backing_pos(&geom, col, row);
        detail[0] = '\0';
        enum mp_world_result result = mp_world_prepare_tile(
            ctx->render.server, player, geom.dimension, cell, backing,
            geom.facing, draft.tiles[tile].map_view, draft.tiles[tile].map_id,
            draft.name, tile, tile_count, row, col, &prepared[tile], detail,
            (int)sizeof(detail));
        if (result != MP_WORLD_OK) {
            materialize_discard_stage(ctx, &draft, prepared, prepared_count,
                                      player, 0, 0);
            send_err(sender, "Backing/world preparation failed at tile %d: %s.",
                     tile, detail[0] ? detail : mp_world_result_name(result));
            return;
        }
        prepared_count++;
    }

    int placed_count = 0;
    for (int tile = 0; tile < tile_count; tile++) {
        detail[0] = '\0';
        enum mp_world_result result = mp_world_place_prepared(
            prepared[tile], detail, (int)sizeof(detail));
        if (result != MP_WORLD_OK) {
            materialize_discard_stage(ctx, &draft, prepared, prepared_count,
                                      player, placed_count, 0);
            send_err(sender,
                     "Backing/world frame placement failed at tile %d; "
                     "rolled back %d tiles: %s.", tile, placed_count,
                     detail[0] ? detail : mp_world_result_name(result));
            return;
        }
        placed_count++;
    }

    detail[0] = '\0';
    enum mp_world_result delivered = mp_world_deliver_prepared_maps(
        player, prepared, tile_count, detail, (int)sizeof(detail));
    if (delivered != MP_WORLD_OK) {
        materialize_discard_stage(ctx, &draft, prepared, prepared_count,
                                  player, placed_count, 1);
        send_err(sender,
                 "Backing/world map delivery failed; frame placement was "
                 "rolled back: %s.",
                 detail[0] ? detail : mp_world_result_name(delivered));
        return;
    }

    struct screen_entry old_state = *screen;
    screen->geom = draft.geom;
    snprintf(screen->owner_uuid, sizeof(screen->owner_uuid), "%s", player_uuid);
    screen->plugin_managed = 1;
    screen->tiles = draft.tiles;
    screen->tiles_capacity = draft.tiles_capacity;
    screen->tiles_initialized = draft.tiles_initialized;
    draft.tiles = nullptr;
    draft.tiles_capacity = 0;
    draft.tiles_initialized = 0;

    if (screen_persistence_save(&ctx->registry, ctx->save_path) != 0) {
        materialize_discard_stage(ctx, &draft, prepared, prepared_count,
                                  player, placed_count, 1);
        int restored = materialize_restore_persisted_state(ctx, screen,
                                                            &old_state);
        send_err(sender,
                 "Screen persistence failed; materialization was rolled back "
                 "(%s).",
                 restored == 0 ? "persistent state restored"
                               : "persistent state restore also failed");
        return;
    }

    for (int i = 0; i < prepared_count; i++)
        mp_world_prepared_destroy(prepared[i]);
    ctx->public_viewer_tick = 0;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), MC_GREEN "[MediaPlayer] " MC_GRAY
             "Logical screen '%s' materialized (%dx%d, facing %s).",
             screen->name, geom.width, geom.height,
             screen_facing_name(geom.facing));
    sender_send_message(sender, buffer);
    snprintf(buffer, sizeof(buffer), MC_YELLOW "[MediaPlayer] " MC_GRAY
             "%d labeled maps were put in your inventory. Install them "
             "left-to-right, top-to-bottom by their row/col labels.",
             tile_count);
    sender_send_message(sender, buffer);
}

static void cmd_screen_delete(struct video_ctx *ctx, void *sender, void *player,
                              int argc, const char **argv)
{
    if (argc != 2) {
        send_err(sender, "Usage: /mpv delete <name>");
        return;
    }

    int idx = screen_registry_find(&ctx->registry, argv[1]);
    if (idx < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }

    struct screen_entry *screen = ctx->registry.screens[idx];
    if (screen->plugin_managed) {
        if (!player) {
            send_err(sender, "Deleting a managed screen requires a player in its dimension.");
            return;
        }
        struct mp_player_snapshot snapshot = {0};
        char bridge_detail[192] = {0};
        if (!mp_player_get_snapshot(player, &snapshot, bridge_detail,
                                    (int)sizeof(bridge_detail)) ||
            strcmp(snapshot.dimension_id, screen->geom.dimension) != 0) {
            send_err(sender, "Go to dimension '%s' before deleting this screen.",
                     screen->geom.dimension);
            return;
        }
        int tile_count = screen_geom_tile_count(&screen->geom);
        for (int tile = 0; tile < tile_count; tile++) {
            int col = tile % screen->geom.width;
            int row = tile / screen->geom.width;
            struct screen_pos cell = screen_geom_tile_pos(
                &screen->geom, col, row);
            struct screen_pos backing = screen_geom_backing_pos(
                &screen->geom, col, row);
            struct mp_world_tile_state state = {0};
            char detail[192] = {0};
            enum mp_world_result inspected = mp_world_inspect_tile(
                player, screen->geom.dimension, cell, backing,
                screen->tiles[tile].map_id, &state,
                detail, (int)sizeof(detail));
            if (state.cell_is_air) continue;
            if (inspected == MP_WORLD_MAP_ID_UNVERIFIABLE &&
                state.frame_present) {
                continue;
            }
            send_err(sender,
                     "Refusing to delete: tile %d at (%d,%d,%d) is not a plugin frame (%s).",
                     tile, cell.x, cell.y, cell.z,
                     detail[0] ? detail : mp_world_result_name(inspected));
            return;
        }
        for (int tile = 0; tile < tile_count; tile++) {
            int col = tile % screen->geom.width;
            int row = tile / screen->geom.width;
            struct screen_pos cell = screen_geom_tile_pos(
                &screen->geom, col, row);
            char detail[192] = {0};
            enum mp_world_result removed = mp_world_remove_managed(
                ctx->render.server, player, screen->geom.dimension, cell,
                screen->tiles[tile].map_id,
                detail, (int)sizeof(detail));
            if (removed != MP_WORLD_OK) {
                send_err(sender,
                         "Managed frame removal failed at tile %d (%d,%d,%d): %s.",
                         tile, cell.x, cell.y, cell.z,
                         detail[0] ? detail : mp_world_result_name(removed));
                return;
            }
        }
    }

    // Stop by runtime ID before compacting the registry.
    stop_screen_playback(ctx, ctx->registry.screens[idx]);

    if (ctx->registry.screens[idx]->tiles_initialized)
        map_render_destroy_screen(&ctx->render, ctx->registry.screens[idx]);

    screen_registry_delete(&ctx->registry, argv[1]);
    screen_persistence_save(&ctx->registry, ctx->save_path);
    ctx->public_viewer_tick = 0;

    char buf[128];
    snprintf(buf, sizeof(buf), MC_GREEN "[MediaPlayer] " MC_GRAY "Screen '%s' deleted.", argv[1]);
    sender_send_message(sender, buf);
}

static void cmd_screen_list(struct video_ctx *ctx, void *sender)
{
    sender_send_message(sender, MC_AQUA "=== Screens ===");
    if (ctx->registry.count == 0) {
        sender_send_message(sender, MC_GRAY "No screens registered.");
        return;
    }
    for (int i = 0; i < ctx->registry.count; i++) {
        struct screen_entry *e = ctx->registry.screens[i];
        char buf[256];
        snprintf(buf, sizeof(buf), MC_GRAY "%d. " MC_WHITE "%s " MC_GRAY "(%dx%d, public, %s)",
                 i + 1, e->name, e->geom.width, e->geom.height,
                 e->playing ? "playing" : "idle");
        sender_send_message(sender, buf);
    }
}

static void cmd_screen_info(struct video_ctx *ctx, void *sender, int argc, const char **argv)
{
    if (argc < 2) {
        send_err(sender, "Usage: /mpv info <name>");
        return;
    }

    int idx = screen_registry_find(&ctx->registry, argv[1]);
    if (idx < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }

    struct screen_entry *e = ctx->registry.screens[idx];
    char buf[256];
    snprintf(buf, sizeof(buf), MC_AQUA "Screen: " MC_WHITE "%s", e->name);
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Size: %dx%d (%dx%d pixels)",
             e->geom.width, e->geom.height,
             screen_geom_pixel_width(&e->geom), screen_geom_pixel_height(&e->geom));
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Facing: %s", screen_facing_name(e->geom.facing));
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Dimension: %s", e->geom.dimension);
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Corner1: (%d, %d, %d)",
             e->geom.corner1.x, e->geom.corner1.y, e->geom.corner1.z);
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Corner2: (%d, %d, %d)",
             e->geom.corner2.x, e->geom.corner2.y, e->geom.corner2.z);
    sender_send_message(sender, buf);
    sender_send_message(sender, MC_GRAY "Access: public");
    sender_send_message(sender, MC_GRAY "View distance: 16 blocks");
}

static int ensure_screen_maps(struct video_ctx *ctx,
                              struct screen_entry *screen, void *player)
{
    if (!screen->plugin_managed) return -4;
    if (screen->tiles_initialized) {
        return 0;
    }
    if (!player) {
        return -1;
    }

    struct mp_player_snapshot snapshot = {0};
    if (!mp_player_get_snapshot(player, &snapshot, nullptr, 0) ||
        strcmp(snapshot.dimension_id, screen->geom.dimension) != 0) {
        return -5;
    }
    enum map_render_error result =
        map_render_init_screen(&ctx->render, screen, player);
    return result;
}

static void cmd_play(struct video_ctx *ctx, void *sender, void *player,
                     int argc, const char **argv)
{
    if (argc < 3) {
        send_err(sender, "Usage: /mpv play <screen> <video-index> [loop]");
        return;
    }

    int scr_idx = screen_registry_find(&ctx->registry, argv[1]);
    if (scr_idx < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }

    struct screen_entry *screen = ctx->registry.screens[scr_idx];
    int map_init_result = ensure_screen_maps(ctx, screen, player);
    if (map_init_result != 0) {
        send_err(sender, "Screen maps could not be initialized (%d).",
                 map_init_result);
        return;
    }

    video_catalog_refresh(&ctx->catalog);
    int vid_idx = 0;
    if (!mpv_parse_index(argv[2], ctx->catalog.count, &vid_idx)) {
        send_err(sender, "Video index out of range. Use /mpv list to see indices.");
        return;
    }

    const struct video_entry *vid = &ctx->catalog.entries[vid_idx];

    if (!mpv_video_fits_screen(vid->tile_width, vid->tile_height,
                               screen->geom.width, screen->geom.height)) {
        char buf[128];
        snprintf(buf, sizeof(buf), MC_RED "Video is %dx%d tiles but screen is %dx%d.",
                 vid->tile_width, vid->tile_height, screen->geom.width, screen->geom.height);
        sender_send_message(sender, buf);
        return;
    }

    int loop = 1;
    if (argc > 3 && !mpv_parse_loop(argv[3], &loop)) {
        send_err(sender, "Loop must be -1 (forever) or a positive count.");
        return;
    }

    // Detach renderers before replacing the session buffer.
    stop_screen_playback(ctx, screen);

    struct video_session *sess =
        video_engine_acquire(&ctx->engine, screen->runtime_id);
    if (!sess) {
        send_err(sender, "No free playback session slot.");
        return;
    }
    struct nbs_error_info nbs_error = {0};
    enum screen_audio_start_result audio_result = SCREEN_AUDIO_NOT_FOUND;
    if (ctx->music_catalog && ctx->music_cache) {
        audio_result = screen_audio_start(
            &ctx->audio, ctx->music_cache, ctx->music_catalog,
            screen->runtime_id, vid->name, &nbs_error);
    }
    int64_t now = get_mono_ms();

    int err = video_session_start(sess, vid->path, loop, screen->runtime_id,
                                  now);
    if (err != 0) {
        screen_audio_stop(&ctx->audio, screen->runtime_id);
        if (err > 0)
            send_err(sender, "Failed to open video file: %s",
                     mcv_error_name((enum mcv_error)err));
        else
            send_err(sender, "Failed to open video file.");
        return;
    }

    screen->playing = 1;
    screen->playback.state = SCREEN_PLAYBACK_PLAYING;
    snprintf(screen->playback.video_name,
             sizeof(screen->playback.video_name), "%s", vid->name);
    screen->playback.current_frame = 0;
    screen->playback.loop_total = sess->loop_total;
    screen->playback.loop_current = sess->loop_current;

    char buf[128];
    snprintf(buf, sizeof(buf), MC_GREEN "[MediaPlayer] " MC_GRAY "Playing '%s' on screen '%s' (loop: %d)",
             vid->name, screen->name, loop);
    sender_send_message(sender, buf);
    if (audio_result == SCREEN_AUDIO_OK) {
        sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY
                            "Synchronized matching NBS soundtrack.");
    } else if (audio_result != SCREEN_AUDIO_NOT_FOUND) {
        snprintf(buf, sizeof(buf), MC_YELLOW "[MediaPlayer] " MC_GRAY
                 "Video is playing without soundtrack: %s.",
                 screen_audio_start_result_name(audio_result));
        sender_send_message(sender, buf);
    }
}

static void cmd_image(struct video_ctx *ctx, void *sender, void *player,
                      int argc, const char **argv)
{
    if (argc < 3) {
        send_err(sender, "Usage: /mpv image <screen> <image-index>");
        return;
    }
    int screen_index = screen_registry_find(&ctx->registry, argv[1]);
    if (screen_index < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }
    struct screen_entry *screen = ctx->registry.screens[screen_index];
    int map_init_result = ensure_screen_maps(ctx, screen, player);
    if (map_init_result != 0) {
        send_err(sender, "Screen maps could not be initialized (%d).",
                 map_init_result);
        return;
    }

    mps_catalog_refresh(&ctx->image_catalog);
    int image_index = 0;
    if (!mpv_parse_index(argv[2], ctx->image_catalog.count, &image_index)) {
        send_err(sender,
                 "Image index out of range. Use /mpv images to see indices.");
        return;
    }
    const struct mps_image_entry *image =
        &ctx->image_catalog.entries[image_index];
    if ((int)image->tile_width != screen->geom.width ||
        (int)image->tile_height != screen->geom.height) {
        char buf[160];
        snprintf(buf, sizeof(buf), MC_RED "Image is %ux%u tiles but screen is %dx%d.",
                 image->tile_width, image->tile_height,
                 screen->geom.width, screen->geom.height);
        sender_send_message(sender, buf);
        return;
    }

    stop_screen_playback(ctx, screen);
    struct mps_source *source = nullptr;
    enum mps_source_error source_error = mps_source_start(
        &ctx->image_engine, screen->runtime_id, image->path, image->name,
        screen->geom.width, screen->geom.height, &source);
    if (source_error != MPS_SOURCE_OK) {
        send_err(sender, "Failed to open image '%s': %s.", image->name,
                 mps_source_error_name(source_error));
        return;
    }
    screen->playing = 1;
    char buf[160];
    snprintf(buf, sizeof(buf), MC_GREEN "[MediaPlayer] " MC_GRAY
             "Displaying image '%s' on screen '%s'.", image->name,
             screen->name);
    sender_send_message(sender, buf);
}

static void cmd_pause_resume_stop(struct video_ctx *ctx, void *sender,
                                  int argc, const char **argv, int action)
{
    if (argc < 2) {
        const char *names[] = { "stop", "pause", "resume" };
        char buf[64];
        snprintf(buf, sizeof(buf), "Usage: /mpv %s <screen>", names[action]);
        send_err(sender, buf, nullptr);
        return;
    }

    int scr_idx = screen_registry_find(&ctx->registry, argv[1]);
    if (scr_idx < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }

    struct screen_entry *screen = ctx->registry.screens[scr_idx];
    struct video_session *sess = video_engine_find(
        &ctx->engine, screen->runtime_id);
    struct mps_source *source = mps_source_find(
        &ctx->image_engine, screen->runtime_id);
    if (action != 0 && !sess && source) {
        send_err(sender, source ? "Static images do not support pause/resume."
                                : "Screen is not playing.");
        return;
    }

    if (action != 0 && !sess) {
        if (screen->playback.state != SCREEN_PLAYBACK_PLAYING &&
            screen->playback.state != SCREEN_PLAYBACK_PAUSED) {
            send_err(sender, "Screen is not playing.");
            return;
        }
        if (action == 1) {
            if (screen->playback.state == SCREEN_PLAYBACK_PAUSED) {
                sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY
                                    "Already paused");
            } else {
                screen->playback.state = SCREEN_PLAYBACK_PAUSED;
                screen->playing = 1;
                sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY
                                    "Paused");
            }
        } else if (screen->playback.state != SCREEN_PLAYBACK_PAUSED) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY
                                "Not paused");
        } else {
            screen->playback.state = SCREEN_PLAYBACK_PLAYING;
            screen->playing = 1;
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY
                                "Resumed");
        }
        return;
    }

    int64_t now = get_mono_ms();

    switch (action) {
    case 0:
        if (!sess && !source &&
            screen->playback.state == SCREEN_PLAYBACK_STOPPED) {
            send_err(sender, "Screen is not playing.");
            return;
        }
        stop_screen_playback(ctx, screen);
        sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Stopped");
        break;
    case 1:
        if (sess->state == PLAY_PAUSED) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Already paused");
        } else {
            video_session_pause(sess, now);
            sync_screen_checkpoint(screen, sess);
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Paused");
        }
        break;
    case 2:
        if (sess->state != PLAY_PAUSED) {
            sender_send_message(sender, MC_RED "[MediaPlayer] " MC_GRAY "Not paused");
        } else {
            video_session_resume(sess, now);
            sync_screen_checkpoint(screen, sess);
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY "Resumed");
        }
        break;
    }
}

static void cmd_status(struct video_ctx *ctx, void *sender, int argc, const char **argv)
{
    if (argc < 2) {
        send_err(sender, "Usage: /mpv status <screen>");
        return;
    }

    int scr_idx = screen_registry_find(&ctx->registry, argv[1]);
    if (scr_idx < 0) {
        send_err(sender, "Screen '%s' not found.", argv[1]);
        return;
    }

    struct screen_entry *screen = ctx->registry.screens[scr_idx];
    struct mps_source *source = mps_source_find(
        &ctx->image_engine, screen->runtime_id);
    char buf[256];

    struct video_session *sess =
        video_engine_find(&ctx->engine, screen->runtime_id);
    if (!sess) {
        if (source) {
            snprintf(buf, sizeof(buf), MC_AQUA "Screen '%s': " MC_WHITE
                     "static image", screen->name);
            sender_send_message(sender, buf);
            snprintf(buf, sizeof(buf), MC_GRAY "Image: %s",
                     source->image_name[0] ? source->image_name : "(unnamed)");
            sender_send_message(sender, buf);
            return;
        }
        if (screen->playback.state == SCREEN_PLAYBACK_PLAYING ||
            screen->playback.state == SCREEN_PLAYBACK_PAUSED) {
            const char *state = screen->playback.state ==
                                        SCREEN_PLAYBACK_PAUSED
                                    ? "paused (waiting for viewer)"
                                    : "playing (waiting for viewer)";
            snprintf(buf, sizeof(buf), MC_AQUA "Screen '%s': " MC_WHITE "%s",
                     screen->name, state);
            sender_send_message(sender, buf);
            snprintf(buf, sizeof(buf), MC_GRAY "Video: %s; frame: %u",
                     screen->playback.video_name,
                     screen->playback.current_frame + 1);
            sender_send_message(sender, buf);
            snprintf(buf, sizeof(buf), MC_GRAY "Loop: %d/%d",
                     screen->playback.loop_current,
                     screen->playback.loop_total == -1
                         ? 0
                         : screen->playback.loop_total);
            sender_send_message(sender, buf);
            return;
        }
        snprintf(buf, sizeof(buf), MC_GRAY "Screen '%s': idle", screen->name);
        sender_send_message(sender, buf);
        return;
    }

    const char *state_str = "unknown";
    switch (sess->state) {
    case PLAY_PLAYING: state_str = "playing"; break;
    case PLAY_PAUSED: state_str = "paused"; break;
    case PLAY_FINISHED: state_str = "finished"; break;
    default: break;
    }

    snprintf(buf, sizeof(buf), MC_AQUA "Screen '%s': " MC_WHITE "%s", screen->name, state_str);
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Frame: %u/%u (skipped: %u)",
             sess->current_frame + 1, sess->frame_count, sess->skipped_frames);
    sender_send_message(sender, buf);
    snprintf(buf, sizeof(buf), MC_GRAY "Loop: %d/%d",
             sess->loop_current, sess->loop_total == -1 ? 0 : sess->loop_total);
    sender_send_message(sender, buf);
    struct screen_audio_session *audio =
        screen_audio_find(&ctx->audio, screen->runtime_id);
    sender_send_message(sender, audio
        ? MC_GRAY "Soundtrack: synchronized NBS"
        : MC_GRAY "Soundtrack: none");
}

static struct video_online_player *find_online_player(
    struct video_ctx *ctx, const char *uuid)
{
    if (!uuid)
        return nullptr;
    for (int i = 0; i < ctx->online_count; i++) {
        if (strcmp(ctx->online_players[i].uuid, uuid) == 0)
            return &ctx->online_players[i];
    }
    return nullptr;
}

static void cmd_watch(struct video_ctx *ctx, void *sender, void *player,
                      const char *player_uuid, int argc, const char **argv)
{
    if (!player || !player_uuid) {
        send_err(sender, "This command can only be used by a player.");
        return;
    }
    if (argc > 2) {
        send_err(sender, "Usage: /mpv watch [on|off]");
        return;
    }

    bool enabled =
        mpv_preferences_enabled(&ctx->preferences, player_uuid);
    if (argc == 1) {
        sender_send_message(
            sender, enabled
                ? MC_GREEN "[MediaPlayer] " MC_GRAY
                    "Public screen video and music are enabled."
                : MC_YELLOW "[MediaPlayer] " MC_GRAY
                    "Public screen video and music are disabled.");
        return;
    }

    bool requested;
    if (strcmp(argv[1], "on") == 0) {
        requested = true;
    } else if (strcmp(argv[1], "off") == 0) {
        requested = false;
    } else {
        send_err(sender, "Usage: /mpv watch [on|off]");
        return;
    }

    if (requested == enabled) {
        sender_send_message(
            sender, enabled
                ? MC_GREEN "[MediaPlayer] " MC_GRAY
                    "Public screen video and music are already enabled."
                : MC_YELLOW "[MediaPlayer] " MC_GRAY
                    "Public screen video and music are already disabled.");
        return;
    }

    if (!mpv_preferences_set_enabled(&ctx->preferences, player_uuid,
                                     requested)) {
        send_err(sender, "Could not update your screen media preference.");
        return;
    }
    if (mpv_preferences_save(&ctx->preferences,
                             ctx->preferences_path) != 0) {
        mpv_preferences_set_enabled(&ctx->preferences, player_uuid, enabled);
        send_err(sender, "Could not save your screen media preference.");
        return;
    }

    struct video_online_player *online =
        find_online_player(ctx, player_uuid);
    if (online)
        online->public_media_enabled = requested;

    if (!requested && online) {
        for (int i = 0; i < ctx->registry.count; i++) {
            struct screen_entry *screen = ctx->registry.screens[i];
            if (screen->tiles_initialized &&
                mpv_membership_contains(&online->membership,
                                        screen->runtime_id)) {
                map_render_hide_viewer(&ctx->render, screen, player,
                                       player_uuid);
            }
        }
        mpv_membership_replace(&online->membership, nullptr, 0);
    } else if (requested) {
        refresh_public_viewers(ctx);
    }
    ctx->public_viewer_tick = 0;

    sender_send_message(
        sender, requested
            ? MC_GREEN "[MediaPlayer] " MC_GRAY
                "Public screen video and music are now enabled."
            : MC_YELLOW "[MediaPlayer] " MC_GRAY
                "Public screen video and music are now disabled.");
}

#if defined(ENABLE_MPV_DEBUG_COMMANDS)

static enum map_test_pattern parse_test_pattern(const char *name)
{
    if (strcmp(name, "red") == 0) return MAP_TEST_RED;
    if (strcmp(name, "green") == 0) return MAP_TEST_GREEN;
    if (strcmp(name, "blue") == 0) return MAP_TEST_BLUE;
    if (strcmp(name, "checker") == 0) return MAP_TEST_CHECKER;
    if (strcmp(name, "quadrants") == 0) return MAP_TEST_QUADRANTS;
    return MAP_TEST_NONE;
}

// Dumps the pointers and values observed by the pure-C world path.
static void debug_world_abi_dump(struct video_ctx *ctx, void *sender,
                                 void *player)
{
    if (!player) {
        send_err(sender, "World ABI diagnostics require a player.");
        return;
    }
    if (!mp_world_c_supported()) {
        send_err(sender, "Pure-C world ABI is not measured on this platform.");
        return;
    }

    char buffer[512];
    snprintf(buffer, sizeof(buffer), MC_AQUA "World ABI slots: " MC_GRAY
             "player.getLocation=%d player.getDimension=%d "
             "dimension.getName=%d dimension.getBlockAt=%d",
#if defined(ES_PLATFORM_WINDOWS)
             ES_PLAYER_SLOT_GET_LOCATION, ES_PLAYER_SLOT_GET_DIMENSION,
             ES_DIMENSION_SLOT_GET_NAME, ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ);
#else
             -1, -1, -1, -1);
#endif
    sender_send_message(sender, buffer);

    struct mp_player_snapshot snapshot = {0};
    struct mp_world_c_trace snapshot_trace = {0};
    char detail[192] = {0};
    bool snapshot_ok = mp_world_c_debug_player_get_snapshot(
        player, &snapshot, &snapshot_trace, detail, (int)sizeof(detail));

    snprintf(buffer, sizeof(buffer), MC_AQUA "Player snapshot: " MC_GRAY
             "ok=%d pos=(%.3f,%.3f,%.3f) rot=(%.3f,%.3f) block=(%d,%d,%d)",
             snapshot_ok, snapshot.x, snapshot.y, snapshot.z,
             snapshot.pitch, snapshot.yaw, snapshot.block_x,
             snapshot.block_y, snapshot.block_z);
    sender_send_message(sender, buffer);
    snprintf(buffer, sizeof(buffer), MC_GRAY
             " dimension=%s player=%p vptr=%p location_target=%p "
             "dimension_target=%p name_target=%p dimension_ptr=%p",
             snapshot.dimension_id, snapshot_trace.player,
             snapshot_trace.player_vptr, snapshot_trace.get_location_target,
             snapshot_trace.get_dimension_target,
             snapshot_trace.get_name_target, snapshot_trace.dimension);
    sender_send_message(sender, buffer);
    if (!snapshot_ok) {
        snprintf(buffer, sizeof(buffer), MC_RED " snapshot failed: %s",
                 detail[0] ? detail : "unknown bridge error");
        sender_send_message(sender, buffer);
        return;
    }

    const int offsets[6][3] = {
        {0, 0, 0}, {0, -1, 0}, {0, 0, -1},
        {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}
    };
    const char *labels[6] = {
        "current", "below", "north", "south", "west", "east"
    };
    for (int i = 0; i < 6; i++) {
        struct screen_pos position = {
            snapshot.block_x + offsets[i][0],
            snapshot.block_y + offsets[i][1],
            snapshot.block_z + offsets[i][2]
        };
        struct mp_world_block_probe probe = {0};
        struct mp_world_c_trace trace = {0};
        detail[0] = '\0';
        enum mp_world_result result = mp_world_c_debug_probe_block(
            player, snapshot.dimension_id, position, &probe, &trace,
            detail, (int)sizeof(detail));
        snprintf(buffer, sizeof(buffer), MC_AQUA "block %s " MC_GRAY
                 "(%d,%d,%d): result=%s found=%d air=%d support=%d type=%s",
                 labels[i], position.x, position.y, position.z,
                 mp_world_result_name(result), probe.block_found,
                 probe.is_air, probe.support_candidate, probe.block_type);
        sender_send_message(sender, buffer);
        snprintf(buffer, sizeof(buffer), MC_GRAY
                 " dimension=%p vptr=%p getBlock=%p block=%p bv=%p "
                 "getType=%p source=%p sv=%p delete=%p destroyed=%u%s%s",
                 trace.dimension, trace.dimension_vptr,
                 trace.get_block_target, trace.block_address,
                 trace.block_vptr, trace.get_type_target,
                 trace.block_source, trace.block_source_vptr,
                 trace.block_delete_target, trace.block_destroy_count,
                 detail[0] ? " err=" : "", detail[0] ? detail : "");
        sender_send_message(sender, buffer);
    }

    struct screen_entry *map_screen = nullptr;
    int tile_index = -1;
    for (int s = 0; s < ctx->registry.count && !map_screen; s++) {
        int count = screen_geom_tile_count(&ctx->registry.screens[s]->geom);
        for (int tile = 0; tile < count; tile++) {
            if (ctx->registry.screens[s]->tiles[tile].map_id_valid) {
                map_screen = ctx->registry.screens[s];
                tile_index = tile;
                break;
            }
        }
    }
    if (!map_screen) {
        sender_send_message(sender, MC_YELLOW
                            "getMap: skipped, no persisted map id");
        return;
    }
    int64_t expected_id = map_screen->tiles[tile_index].map_id;
    void *map_view = mp_world_get_map(ctx->render.server, expected_id);
    int64_t resolved_id = map_view ? es_map_view_get_id(map_view) : -1;
    snprintf(buffer, sizeof(buffer), MC_AQUA "getMap: " MC_GRAY
             "screen=%s tile=%d requested=%lld map=%p resolved=%lld",
             map_screen->name, tile_index, (long long)expected_id,
             map_view, (long long)resolved_id);
    sender_send_message(sender, buffer);
}

static void cmd_debug(struct video_ctx *ctx, void *sender, void *player,
                      int argc, const char **argv)
{
    if (argc < 2) {
        send_err(sender, "Usage: /mpv debug abi|backing|maps|renderer|test-pattern|resend ...");
        return;
    }

    if (strcmp(argv[1], "abi") == 0) {
        if (argc >= 3 && strcmp(argv[2], "world") == 0) {
            debug_world_abi_dump(ctx, sender, player);
            return;
        }
        char buffer[256];
#if defined(ES_PLATFORM_WINDOWS)
        const char *platform = "windows-msvc";
#else
        const char *platform = "linux-libc++";
#endif
        snprintf(buffer, sizeof(buffer),
                 MC_AQUA "Map ABI: " MC_GRAY
                 "platform=%s api=%s supported=%d sendMap=%d "
                 "addRenderer=%d removeRenderer=%d render=%d",
                 platform, ES_API_VERSION, es_map_abi_supported(),
                 ES_PLAYER_SLOT_SEND_MAP,
                 ES_MAPVIEW_SLOT_ADD_RENDERER,
                 ES_MAPVIEW_SLOT_REMOVE_RENDERER,
                 ES_MAPRENDERER_SLOT_RENDER);
        sender_send_message(sender, buffer);
        snprintf(buffer, sizeof(buffer),
                 MC_AQUA "Map layout: " MC_GRAY
                 "renderer=%d canvas=%d buffer=+0x%x shared_ptr=%d "
                 "refcount=%d",
                 ES_MAPRENDERER_SIZE, ES_MAPCANVAS_SIZE,
                 ES_MAPCANVAS_OFF_BUFFER_BEGIN, ES_SHARED_PTR_SIZE,
                 ES_REFCOUNT_SIZE);
        sender_send_message(sender, buffer);
        return;
    }

    if (strcmp(argv[1], "backing") == 0) {
        if (!player) {
            send_err(sender, "Backing diagnostics require a player.");
            return;
        }
        struct mp_player_snapshot snapshot = {0};
        char detail[192] = {0};
        if (!mp_player_get_snapshot(player, &snapshot, detail,
                                    (int)sizeof(detail))) {
            send_err(sender, "Unable to read player position: %s.", detail);
            return;
        }
        const int offsets[5][3] = {
            {0, 0, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}
        };
        const char *labels[5] = {"seed", "z-1", "z+1", "x-1", "x+1"};
        for (int i = 0; i < 5; i++) {
            struct screen_pos position = {
                snapshot.block_x + offsets[i][0],
                snapshot.block_y + offsets[i][1],
                snapshot.block_z + offsets[i][2]
            };
            struct mp_world_block_probe probe = {0};
            detail[0] = '\0';
            enum mp_world_result result = mp_world_probe_block(
                player, snapshot.dimension_id, position, &probe,
                detail, (int)sizeof(detail));
            char buffer[448];
            if (result != MP_WORLD_OK) {
                snprintf(buffer, sizeof(buffer), MC_GRAY
                         " %s (%d,%d,%d): probe failed: %s",
                         labels[i], position.x, position.y, position.z,
                         detail[0] ? detail : mp_world_result_name(result));
            } else {
                snprintf(buffer, sizeof(buffer), MC_GRAY
                         " %s (%d,%d,%d) type=%s air=%d support=%d "
                         "block=%p bv=%p source=%p sv=%p",
                         labels[i], position.x, position.y, position.z,
                         probe.block_type, probe.is_air ? 1 : 0,
                         probe.support_candidate ? 1 : 0,
                         probe.block, probe.block_vptr, probe.block_source,
                         probe.block_source_vptr);
            }
            sender_send_message(sender, buffer);
        }
        return;
    }

    if (argc < 3) {
        send_err(sender, "This debug mode requires a screen name.");
        return;
    }
    int screen_index = screen_registry_find(&ctx->registry, argv[2]);
    if (screen_index < 0) {
        send_err(sender, "Screen '%s' not found.", argv[2]);
        return;
    }
    struct screen_entry *screen = ctx->registry.screens[screen_index];

    if (strcmp(argv[1], "maps") == 0) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), MC_AQUA "Screen %s: " MC_GRAY
                 "initialized=%d tiles=%d item-frame-association=unmanaged",
                 screen->name, screen->tiles_initialized,
                 screen_geom_tile_count(&screen->geom));
        sender_send_message(sender, buffer);
        for (int i = 0; i < screen_geom_tile_count(&screen->geom); i++) {
            snprintf(buffer, sizeof(buffer), MC_GRAY
                     " tile=%d map_id=%lld map=%p renderer_owner=%p valid=%d",
                     i, (long long)screen->tiles[i].map_id,
                     screen->tiles[i].map_view, screen->tiles[i].renderer,
                     screen->tiles[i].valid);
            sender_send_message(sender, buffer);
        }
        return;
    }

    if (strcmp(argv[1], "renderer") == 0) {
        char buffer[448];
        for (int i = 0; i < screen_geom_tile_count(&screen->geom); i++) {
            struct map_renderer_stats stats;
            if (!map_render_get_stats(screen, i, &stats)) {
                snprintf(buffer, sizeof(buffer), MC_GRAY
                         " tile=%d renderer unavailable", i);
            } else {
                snprintf(buffer, sizeof(buffer), MC_GRAY
                         " tile=%d renderer=%p vptr=%p refs=%d init=%llu "
                         "render=%llu destroy=%llu canvas=%p map=%p player=%p",
                         i, stats.renderer, stats.renderer_vptr,
                         stats.strong_references,
                         (unsigned long long)stats.initialize_count,
                         (unsigned long long)stats.render_count,
                         (unsigned long long)stats.destroy_count,
                         stats.last_canvas, stats.last_map_view,
                         stats.last_player);
                sender_send_message(sender, buffer);
                snprintf(buffer, sizeof(buffer), MC_GRAY
                         "  canvas_valid=%d pixels=%zu write_verified=%d "
                         "first=0x%08x last=0x%08x",
                         stats.last_canvas_valid,
                         stats.last_canvas_pixels,
                         stats.last_write_verified,
                         stats.last_first_pixel, stats.last_last_pixel);
            }
            sender_send_message(sender, buffer);
        }
        return;
    }

    void *players[64];
    const char *player_ids[64];
    int player_count = collect_public_viewers(
        ctx, screen, players, player_ids);
    if (strcmp(argv[1], "resend") == 0) {
        for (int i = 0; i < ctx->online_count; i++) {
            if (mpv_membership_contains(&ctx->online_players[i].membership,
                                        screen->runtime_id)) {
                struct mps_source *source = mps_source_find(
                    &ctx->image_engine, screen->runtime_id);
                queue_resend(&ctx->online_players[i], screen->runtime_id,
                             source ? source->attachment_id : 0);
            }
        }
        sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY
                            "Map resend queued for nearby public viewers.");
        return;
    }

    if (strcmp(argv[1], "test-pattern") == 0) {
        if (argc < 4) {
            send_err(sender, "Usage: /mpv debug test-pattern <screen> "
                     "red|green|blue|checker|quadrants");
            return;
        }
        enum map_test_pattern pattern = parse_test_pattern(argv[3]);
        if (pattern == MAP_TEST_NONE) {
            send_err(sender, "Unknown test pattern '%s'.", argv[3]);
            return;
        }
        int init_result = ensure_screen_maps(ctx, screen, player);
        if (init_result != 0) {
            send_err(sender, "Test pattern map initialization failed (%d).",
                     init_result);
            return;
        }
        int result = map_render_set_test_pattern(
            &ctx->render, screen, pattern, players, player_ids, player_count);
        if (result == 0) {
            sender_send_message(sender, MC_GREEN "[MediaPlayer] " MC_GRAY
                                "Test pattern sent to nearby public viewers.");
        } else {
            send_err(sender, "Test pattern failed (%d).", result);
        }
        return;
    }

    send_err(sender, "Unknown debug mode '%s'.", argv[1]);
}

#endif

// --- Main command dispatcher ---

void video_handle_command(struct video_ctx *ctx,
                          int argc, const char **argv,
                          void *sender, void *player,
                          const char *player_uuid)
{
    bool is_op = player ? es_player_is_op(player) : false;
    const char *action = argc > 0 ? argv[0] : nullptr;
    if (!mpv_command_allowed(player != nullptr, is_op, action)) {
        send_err(sender, "You do not have permission to use /mpv.");
        return;
    }

    if (argc == 0) {
        cmd_help(ctx, sender);
        return;
    }

    if (strcmp(action, "help") == 0) {
        cmd_help(ctx, sender);
    } else if (strcmp(action, "list") == 0) {
        cmd_list(ctx, sender, argc, argv);
    } else if (strcmp(action, "create") == 0) {
        cmd_screen_create(ctx, sender, player, player_uuid, argc, argv);
    } else if (strcmp(action, "materialize") == 0) {
        cmd_screen_materialize(ctx, sender, player, player_uuid, argc, argv);
    } else if (strcmp(action, "delete") == 0) {
        cmd_screen_delete(ctx, sender, player, argc, argv);
    } else if (strcmp(action, "screens") == 0) {
        cmd_screen_list(ctx, sender);
    } else if (strcmp(action, "info") == 0) {
        cmd_screen_info(ctx, sender, argc, argv);
    } else if (strcmp(action, "play") == 0) {
        cmd_play(ctx, sender, player, argc, argv);
    } else if (strcmp(action, "images") == 0) {
        cmd_images(ctx, sender, argc, argv);
    } else if (strcmp(action, "image") == 0) {
        cmd_image(ctx, sender, player, argc, argv);
    } else if (strcmp(action, "pause") == 0) {
        cmd_pause_resume_stop(ctx, sender, argc, argv, 1);
    } else if (strcmp(action, "resume") == 0) {
        cmd_pause_resume_stop(ctx, sender, argc, argv, 2);
    } else if (strcmp(action, "stop") == 0) {
        cmd_pause_resume_stop(ctx, sender, argc, argv, 0);
    } else if (strcmp(action, "status") == 0) {
        cmd_status(ctx, sender, argc, argv);
    } else if (strcmp(action, "watch") == 0) {
        cmd_watch(ctx, sender, player, player_uuid, argc, argv);
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
    } else if (strcmp(action, "debug") == 0) {
        cmd_debug(ctx, sender, player, argc, argv);
#endif
    } else {
        send_err(sender, "Unknown command. Try /mpv help");
    }
}

// --- Event handlers ---

void video_on_player_join(struct video_ctx *ctx, void *player, const char *uuid)
{
    if (!player || !uuid) return;
    for (int i = 0; i < ctx->online_count; i++) {
        if (strcmp(ctx->online_players[i].uuid, uuid) == 0) {
            ctx->online_players[i].player = player;
            memset(&ctx->online_players[i].snapshot, 0,
                   sizeof(ctx->online_players[i].snapshot));
            memset(&ctx->online_players[i].membership, 0,
                   sizeof(ctx->online_players[i].membership));
            ctx->online_players[i].public_media_enabled =
                mpv_preferences_enabled(&ctx->preferences, uuid);
            ctx->public_viewer_tick = 0;
            return;
        }
    }
    if (ctx->online_count >= 64) return;

    struct video_online_player *online =
        &ctx->online_players[ctx->online_count++];
    memset(online, 0, sizeof(*online));
    online->player = player;
    snprintf(online->uuid, sizeof(online->uuid), "%s", uuid);
    online->public_media_enabled =
        mpv_preferences_enabled(&ctx->preferences, uuid);
    ctx->public_viewer_tick = 0;
}

void video_on_player_quit(struct video_ctx *ctx, const char *uuid)
{
    for (int i = 0; i < ctx->online_count; i++) {
        if (strcmp(ctx->online_players[i].uuid, uuid) == 0) {
            for (int j = i; j < ctx->online_count - 1; j++)
                ctx->online_players[j] = ctx->online_players[j + 1];
            ctx->online_count--;
            memset(&ctx->online_players[ctx->online_count], 0,
                   sizeof(ctx->online_players[ctx->online_count]));
            break;
        }
    }
}

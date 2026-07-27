#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_POLICY_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_POLICY_H

#include "mediaplayer/screen/screen_geometry.h"
#include <stdbool.h>
#include <stdint.h>

#define MPV_PUBLIC_VIEW_DISTANCE 64.0
#define MPV_PUBLIC_VIEW_DISTANCE_SQUARED \
    (MPV_PUBLIC_VIEW_DISTANCE * MPV_PUBLIC_VIEW_DISTANCE)
#define MPV_PUBLIC_VIEWER_REFRESH_TICKS 20
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
#define MPV_COMMAND_USAGE_COUNT 13
#else
#define MPV_COMMAND_USAGE_COUNT 12
#endif

struct mpv_public_snapshot {
    bool valid;
    double x;
    double y;
    double z;
    char dimension[64];
};

struct mpv_screen_center {
    double x;
    double y;
    double z;
};

struct mpv_public_candidate {
    void *player;
    const char *uuid;
    bool online;
    struct mpv_public_snapshot snapshot;
};

struct mpv_public_membership {
    uint64_t screen_ids[64];
    int count;
};

enum mpv_membership_transition {
    MPV_MEMBERSHIP_OUTSIDE = 0,
    MPV_MEMBERSHIP_ENTERED,
    MPV_MEMBERSHIP_STAYED,
    MPV_MEMBERSHIP_LEFT,
};

extern const char *const mpv_command_usages[MPV_COMMAND_USAGE_COUNT];

bool mpv_command_allowed(bool is_player, bool is_op);
bool mpv_command_action_registered(const char *action);

struct mpv_screen_center mpv_screen_center(const struct screen_geom *screen);
bool mpv_public_viewer_eligible(bool online,
                                const struct mpv_public_snapshot *snapshot,
                                const struct screen_geom *screen);
int mpv_collect_public_viewers(const struct screen_geom *screen,
                               const struct mpv_public_candidate *candidates,
                               int candidate_count,
                               void **players,
                               const char **player_ids,
                               int capacity);

bool mpv_membership_contains(const struct mpv_public_membership *membership,
                             uint64_t screen_id);
enum mpv_membership_transition mpv_membership_transition(
    const struct mpv_public_membership *membership,
    uint64_t screen_id, bool eligible);
void mpv_membership_replace(struct mpv_public_membership *membership,
                            const uint64_t *screen_ids, int count);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_POLICY_H

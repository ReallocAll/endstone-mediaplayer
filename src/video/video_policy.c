#include "mediaplayer/video/video_policy.h"
#include <string.h>

const char *const mpv_command_usages[MPV_COMMAND_USAGE_COUNT] = {
    "/mpv",
    "/mpv (help)<a: MpvHelp>",
    "/mpv (screens)<a: MpvScreens>",
    "/mpv (list)<a: MpvList> [filter: string]",
    "/mpv (create)<a: MpvCreate> <name: string>",
    "/mpv (delete)<a: MpvDelete> <name: string>",
    "/mpv (info)<a: MpvInfo> <name: string>",
    "/mpv (play)<a: MpvPlay> <screen: string> <video: int> [loop: int]",
    "/mpv (pause)<a: MpvPause> <name: string>",
    "/mpv (resume)<a: MpvResume> <name: string>",
    "/mpv (stop)<a: MpvStop> <name: string>",
    "/mpv (status)<a: MpvStatus> <name: string>",
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
    "/mpv (debug)<a: MpvDebug> <mode: string> [screen: string] [value: string]",
#endif
};

bool mpv_command_allowed(bool is_player, bool is_op)
{
    return !is_player || is_op;
}

bool mpv_command_action_registered(const char *action)
{
    static const char *const actions[] = {
        "help", "screens", "list", "create", "delete", "info",
        "play", "pause", "resume", "stop", "status",
#if defined(ENABLE_MPV_DEBUG_COMMANDS)
        "debug",
#endif
    };
    if (!action || !action[0]) return true;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        if (strcmp(action, actions[i]) == 0) return true;
    }
    return false;
}

struct mpv_screen_center mpv_screen_center(const struct screen_geom *screen)
{
    struct mpv_screen_center center = {0};
    if (!screen) return center;
    center.x = ((double)screen->corner1.x + screen->corner2.x) * 0.5 + 0.5;
    center.y = ((double)screen->corner1.y + screen->corner2.y) * 0.5 + 0.5;
    center.z = ((double)screen->corner1.z + screen->corner2.z) * 0.5 + 0.5;
    return center;
}

bool mpv_public_viewer_eligible(bool online,
                                const struct mpv_public_snapshot *snapshot,
                                const struct screen_geom *screen)
{
    if (!online || !snapshot || !snapshot->valid || !screen ||
        !snapshot->dimension[0] ||
        strcmp(snapshot->dimension, screen->dimension) != 0) {
        return false;
    }

    struct mpv_screen_center center = mpv_screen_center(screen);
    double dx = snapshot->x - center.x;
    double dy = snapshot->y - center.y;
    double dz = snapshot->z - center.z;
    return dx * dx + dy * dy + dz * dz <=
           MPV_PUBLIC_VIEW_DISTANCE_SQUARED;
}

int mpv_collect_public_viewers(const struct screen_geom *screen,
                               const struct mpv_public_candidate *candidates,
                               int candidate_count,
                               void **players,
                               const char **player_ids,
                               int capacity)
{
    if (!screen || !candidates || !players || capacity <= 0) return 0;
    int count = 0;
    for (int i = 0; i < candidate_count && count < capacity; i++) {
        const struct mpv_public_candidate *candidate = &candidates[i];
        if (!candidate->player ||
            !mpv_public_viewer_eligible(candidate->online,
                                        &candidate->snapshot, screen)) {
            continue;
        }
        players[count] = candidate->player;
        if (player_ids) player_ids[count] = candidate->uuid;
        count++;
    }
    return count;
}

bool mpv_membership_contains(const struct mpv_public_membership *membership,
                             uint64_t screen_id)
{
    if (!membership || screen_id == 0) return false;
    for (int i = 0; i < membership->count; i++) {
        if (membership->screen_ids[i] == screen_id) return true;
    }
    return false;
}

enum mpv_membership_transition mpv_membership_transition(
    const struct mpv_public_membership *membership,
    uint64_t screen_id, bool eligible)
{
    bool was_eligible = mpv_membership_contains(membership, screen_id);
    if (eligible) {
        return was_eligible ? MPV_MEMBERSHIP_STAYED : MPV_MEMBERSHIP_ENTERED;
    }
    return was_eligible ? MPV_MEMBERSHIP_LEFT : MPV_MEMBERSHIP_OUTSIDE;
}

void mpv_membership_replace(struct mpv_public_membership *membership,
                            const uint64_t *screen_ids, int count)
{
    if (!membership) return;
    if (count < 0) count = 0;
    int capacity = (int)(sizeof(membership->screen_ids) /
                         sizeof(membership->screen_ids[0]));
    if (count > capacity) count = capacity;
    if (count > 0 && screen_ids) {
        memcpy(membership->screen_ids, screen_ids,
               (size_t)count * sizeof(screen_ids[0]));
    }
    if (count < membership->count) {
        memset(&membership->screen_ids[count], 0,
               (size_t)(membership->count - count) *
                   sizeof(membership->screen_ids[0]));
    }
    membership->count = count;
}

#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_REGISTRY_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_REGISTRY_H

#include "mediaplayer/screen/screen_geometry.h"
#include "mediaplayer/screen/surface.h"
#include <stddef.h>
#include <stdint.h>

#define SCREEN_NAME_MAX 64
#define SCREEN_UUID_LEN 36
#define SCREEN_MATERIALIZED_MAX_TILES 4096
#define SCREEN_PLAYBACK_VIDEO_NAME_MAX 128

enum screen_error {
    SCREEN_OK = 0,
    SCREEN_ERR_NOT_FOUND,
    SCREEN_ERR_NAME_EXISTS,
    SCREEN_ERR_NAME_INVALID,
    SCREEN_ERR_FULL,
    SCREEN_ERR_DIMENSION_INVALID,
    SCREEN_ERR_MATERIALIZATION_LIMIT,
    SCREEN_ERR_NO_MEMORY,
    SCREEN_ERR_RUNTIME_ID_EXHAUSTED,
};

const char *screen_error_name(enum screen_error error);

struct screen_tile_rt {
    int64_t map_id; // Persistent map ID for plugin-managed screens.
    int map_id_valid;
    void *map_view; // Borrowed MapView.
    void *renderer;
    int valid;

};

enum screen_playback_state {
    SCREEN_PLAYBACK_STOPPED = 0,
    SCREEN_PLAYBACK_PLAYING,
    SCREEN_PLAYBACK_PAUSED,
};

struct screen_playback_checkpoint {
    enum screen_playback_state state;
    char video_name[SCREEN_PLAYBACK_VIDEO_NAME_MAX];
    uint32_t current_frame;
    int loop_total;
    int loop_current;
};

struct screen_entry {
    char name[SCREEN_NAME_MAX];
    char owner_uuid[SCREEN_UUID_LEN + 1];
    struct screen_geom geom;
    int64_t created_at;
    uint64_t runtime_id; // Non-persistent process identity.

    // True for screens physically created by this plugin.
    int plugin_managed;

    // Sparse display content owned by this screen.
    struct surface *surface;

    // Map IDs persist; map pointers and renderers do not.
    struct screen_tile_rt *tiles;
    size_t tiles_capacity;
    int tiles_initialized;

    int playing;
    struct screen_playback_checkpoint playback;
};

#define SCREEN_REGISTRY_MAX 64

struct screen_registry {
    struct screen_entry *screens[SCREEN_REGISTRY_MAX];
    int count;
    uint64_t next_runtime_id;
};

void screen_registry_init(struct screen_registry *reg);

// Releases tile compatibility storage.  Renderers and history must already
// have been torn down by the owning subsystem.
void screen_entry_cleanup_tiles(struct screen_entry *entry);
void screen_entry_cleanup(struct screen_entry *entry);
void screen_registry_cleanup(struct screen_registry *reg);

// Allocates bounded compatibility storage for physically materialized maps.
// Logical geometry may be larger than this explicit resource limit.
enum screen_error screen_entry_materialize_tiles(struct screen_entry *entry);

// Creates a screen after validating its name.
enum screen_error screen_registry_create(
    struct screen_registry *reg,
    const char *name,
    const char *owner_uuid,
    const struct screen_geom *geom,
    int *out_index);

// Deletes a screen by name.
enum screen_error screen_registry_delete(struct screen_registry *reg, const char *name);

// Returns a screen index or -1.
int screen_registry_find(const struct screen_registry *reg, const char *name);

// Validates a screen name.
int screen_name_valid(const char *name);

#endif // ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_REGISTRY_H

#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_REGISTRY_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_SCREEN_REGISTRY_H

#include "mediaplayer/screen/screen_geometry.h"
#include <stdint.h>

#define SCREEN_NAME_MAX 64
#define SCREEN_UUID_LEN 36

enum screen_error {
    SCREEN_OK = 0,
    SCREEN_ERR_NOT_FOUND,
    SCREEN_ERR_NAME_EXISTS,
    SCREEN_ERR_NAME_INVALID,
    SCREEN_ERR_FULL,
};

const char *screen_error_name(enum screen_error error);

struct screen_tile_rt {
    int64_t map_id; // Persistent map ID for plugin-managed screens.
    int map_id_valid;
    void *map_view; // Borrowed MapView.
    void *renderer;
    int valid;

    // Last pixels sent, owned by map_render.
    uint8_t *last_sent;
    int last_sent_valid;
};

struct screen_entry {
    char name[SCREEN_NAME_MAX];
    char owner_uuid[SCREEN_UUID_LEN + 1];
    struct screen_geom geom;
    int64_t created_at;
    uint64_t runtime_id; // Non-persistent process identity.

    // True for screens physically created by this plugin.
    int plugin_managed;

    // Map IDs persist; map pointers and renderers do not.
    struct screen_tile_rt tiles[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];
    int tiles_initialized;

    int playing;
};

#define SCREEN_REGISTRY_MAX 64

struct screen_registry {
    struct screen_entry screens[SCREEN_REGISTRY_MAX];
    int count;
    uint64_t next_runtime_id;
};

void screen_registry_init(struct screen_registry *reg);

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

#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_PERSISTENCE_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_PERSISTENCE_H

#include "mediaplayer/screen/screen_registry.h"

#define SCREEN_SAVE_VERSION 2

// Plugin pointer for load diagnostics via PLUGIN_LOG (nullptr silences them)
void screen_persistence_set_log_plugin(void *plugin);

// Save registry to JSON atomically (temp file + rename); 0 on success, -1 on error
int screen_persistence_save(const struct screen_registry *reg, const char *path);

// Load registry from JSON; skipped entries are counted in *warnings (may be
// nullptr).  On -1 the file is moved aside to <path>.bad, never overwritten.
int screen_persistence_load(struct screen_registry *reg, const char *path, int *warnings);

#endif // ENDSTONE_MEDIAPLAYER_SCREEN_PERSISTENCE_H

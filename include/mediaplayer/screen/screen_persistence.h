#ifndef ENDSTONE_MEDIAPLAYER_SCREEN_PERSISTENCE_H
#define ENDSTONE_MEDIAPLAYER_SCREEN_PERSISTENCE_H

#include "mediaplayer/screen/screen_registry.h"

#define SCREEN_SAVE_VERSION 2

// The sidecar stores the physical map IDs as little-endian int64 values.
// These constants are part of the on-disk contract so diagnostics and tests
// can inspect a generated sidecar without depending on host layout.
#define SCREEN_SIDECAR_VERSION 1
#define SCREEN_SIDECAR_HEADER_SIZE 32
#define SCREEN_SIDECAR_MAGIC "ESMPID2\0"
#define SCREEN_SIDECAR_FIELD "map_ids_sidecar"

// Plugin pointer for load diagnostics via PLUGIN_LOG (nullptr silences them)
void screen_persistence_set_log_plugin(void *plugin);

// Save registry to JSON atomically (temp file + rename); 0 on success, -1 on error
int screen_persistence_save(const struct screen_registry *reg, const char *path);

// Load registry from JSON; reg must have been initialized with
// screen_registry_init().  Skipped entries are counted in *warnings (may be
// nullptr).  The destination is replaced only after the complete manifest
// and, for v2, sidecar validate successfully.  On a corrupt/truncated input,
// a diagnostic copy is made at the first free <path>.bad[.N] name and the
// original files remain untouched.
int screen_persistence_load(struct screen_registry *reg, const char *path, int *warnings);

#endif // ENDSTONE_MEDIAPLAYER_SCREEN_PERSISTENCE_H

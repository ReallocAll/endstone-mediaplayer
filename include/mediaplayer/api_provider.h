#ifndef ENDSTONE_MEDIAPLAYER_API_PROVIDER_H
#define ENDSTONE_MEDIAPLAYER_API_PROVIDER_H

#include "endstone_mediaplayer_api.h"
#include "mediaplayer/video/video_commands.h"

// These lifecycle hooks are private to the plugin.  The provider is exposed
// publicly only through endstone_mediaplayer_get_api().
void mp_api_provider_activate(struct video_ctx *ctx);
void mp_api_provider_deactivate(struct video_ctx *ctx);

// Returns nonzero when an external frame producer currently owns a screen.
// This private query does not abort or otherwise modify the producer.
int mp_api_provider_screen_has_active_frame(uint64_t runtime_id);

#endif // ENDSTONE_MEDIAPLAYER_API_PROVIDER_H

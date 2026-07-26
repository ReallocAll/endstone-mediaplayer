#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_COMMANDS_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_COMMANDS_H

#include "mediaplayer/music/music_catalog.h"
#include "mediaplayer/music/music_cache.h"
#include "mediaplayer/music/music_session.h"

#define MUSIC_COMMAND_USAGE_COUNT 9

struct music_ctx {
    void *plugin;
    struct music_catalog catalog;
    struct music_cache cache;
    struct music_engine engine;
};

extern const char *const music_command_usages[MUSIC_COMMAND_USAGE_COUNT];

void music_ctx_init(struct music_ctx *ctx, void *plugin,
                    const char *data_dir);
void music_ctx_shutdown(struct music_ctx *ctx);
void music_tick(struct music_ctx *ctx);
void music_on_player_join(struct music_ctx *ctx, void *player);
void music_on_player_quit(struct music_ctx *ctx, void *player);
void music_handle_command(struct music_ctx *ctx,
                          int argc, const char **argv,
                          void *sender, void *player);

#endif // ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_COMMANDS_H

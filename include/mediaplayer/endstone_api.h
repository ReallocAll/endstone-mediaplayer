#ifndef ENDSTONE_MEDIAPLAYER_ENDSTONE_API_H
#define ENDSTONE_MEDIAPLAYER_ENDSTONE_API_H

#include "endstone_abi.h"
#include <stdio.h>

#define ENDSTONE_MEDIAPLAYER_PATH_MAX 4096

FILE *fopen_utf8(const char *path, const char *mode);
// Copies borrowed Player pointers from Server::getOnlinePlayers() and releases
// every temporary vector handle before returning.
int server_get_online_players(void *server, void **players, int capacity);
void *endstone_expected_image_base(void);
bool endstone_expected_image_matches(void *base);
bool server_find_player_handle(void *server, void *player,
                               struct es_shared_handle *out);
void *server_player_from_sender(void *server,
                                const struct es_shared_handle *sender);
void sender_send_message(void *sender, const char *message);
void player_get_location(void *player, struct es_location *location);
void player_play_sound(void *player, const char *sound,
                       float volume, float pitch);
void player_send_popup(void *player, const char *message);
void player_send_tip(void *player, const char *message);
void *boss_bar_create(void *player, const char *title);
void boss_bar_destroy(void *boss);
void boss_bar_set_progress(void *boss, float progress);
void boss_bar_set_title(void *boss, const char *title);

#endif // ENDSTONE_MEDIAPLAYER_ENDSTONE_API_H

#ifndef ENDSTONE_MEDIAPLAYER_ENDSTONE_API_H
#define ENDSTONE_MEDIAPLAYER_ENDSTONE_API_H

#include "endstone_abi.h"
#include <stdio.h>

#define ENDSTONE_MEDIAPLAYER_PATH_MAX 4096

FILE *fopen_utf8(const char *path, const char *mode);
// Copies borrowed Player pointers from Server::getOnlinePlayers().
// Returns the copied count, or -1 when the server ABI result is invalid.
int server_get_online_players(void *server, void **players, int capacity);
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

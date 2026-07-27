#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_SOUND_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_SOUND_H

#include "mediaplayer/music/music_cache.h"

const char *music_instrument_sound(int instrument);
void music_note_play(const struct music_note *note, void **players,
                     int player_count);

#endif // ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_SOUND_H

#include "mediaplayer/music/music_sound.h"
#include "mediaplayer/endstone_api.h"

static const char *const g_instruments[MUSIC_INSTRUMENT_COUNT] = {
    "note.harp", "note.bassattack", "note.bd", "note.snare",
    "note.hat", "note.guitar", "note.flute", "note.bell",
    "note.chime", "note.xylobone", "note.iron_xylophone", "note.cow_bell",
    "note.didgeridoo", "note.bit", "note.banjo", "note.pling",
};

const char *music_instrument_sound(int instrument)
{
    if (instrument < 0 || instrument >= MUSIC_INSTRUMENT_COUNT)
        instrument = 0;
    return g_instruments[instrument];
}

void music_note_play(const struct music_note *note, void **players,
                     int player_count)
{
    if (!note || !players || player_count <= 0)
        return;
    const char *sound = music_instrument_sound(note->instrument);
    for (int i = 0; i < player_count; i++) {
        if (players[i])
            player_play_sound(players[i], sound, note->volume, note->pitch);
    }
}

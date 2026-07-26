#ifndef ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_CACHE_H
#define ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_CACHE_H

#include "nbsparser.h"
#include <stdint.h>
#include <stdio.h>

#define MUSIC_INSTRUMENT_COUNT 16
#define MUSIC_SONG_NAME_MAX 256

struct music_note {
    int64_t time_ms;
    int instrument;
    float volume;
    float pitch;
};

struct music_cache_entry {
    char song_name[MUSIC_SONG_NAME_MAX];
    struct music_note *notes;
    int64_t duration_ms;
};

struct music_cache {
    struct music_cache_entry *entries;
};

void music_cache_init(struct music_cache *cache);
void music_cache_shutdown(struct music_cache *cache);
long long music_cache_find(const struct music_cache *cache,
                           const char *song_name);
long long music_cache_parse(struct music_cache *cache, FILE *file,
                            const char *song_name,
                            struct nbs_error_info *error);
struct music_cache_entry *music_cache_get(struct music_cache *cache,
                                          long long index);

#endif // ENDSTONE_MEDIAPLAYER_MUSIC_MUSIC_CACHE_H

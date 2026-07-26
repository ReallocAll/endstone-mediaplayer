#include "mediaplayer/music/music_cache.h"
#include <stb_ds.h>
#include <math.h>
#include <string.h>

void music_cache_init(struct music_cache *cache)
{
    cache->entries = nullptr;
}

void music_cache_shutdown(struct music_cache *cache)
{
    int count = (int)arrlen(cache->entries);
    for (int i = 0; i < count; i++)
        arrfree(cache->entries[i].notes);
    arrfree(cache->entries);
    cache->entries = nullptr;
}

long long music_cache_find(const struct music_cache *cache,
                           const char *song_name)
{
    int count = (int)arrlen(cache->entries);
    for (int i = 0; i < count; i++) {
        if (strcmp(cache->entries[i].song_name, song_name) == 0)
            return i;
    }
    return -1;
}

long long music_cache_parse(struct music_cache *cache, FILE *file,
                            const char *song_name,
                            struct nbs_error_info *error)
{
    struct nbs_error_info parse_error = {0};
    struct nbs_song *song = nbs_parse(file, &parse_error);
    if (!song) {
        if (error) *error = parse_error;
        return -1;
    }

    if (song->tempo <= 0.0f) {
        if (error) {
            error->code = NBS_ERROR_INVALID_VALUE;
            error->section = NBS_SECTION_HEADER;
        }
        nbs_free(song);
        return -1;
    }

    struct music_cache_entry entry = {0};
    snprintf(entry.song_name, sizeof(entry.song_name), "%s", song_name);

    float time_per_tick = 20.0f / song->tempo * 50.0f;
    int note_count = (int)arrlen(song->notes);
    int layer_count = (int)arrlen(song->layers);

    for (int i = 0; i < note_count; i++) {
        struct nbs_note *source = &song->notes[i];
        struct nbs_layer *layer =
            source->layer < (size_t)layer_count
                ? &song->layers[source->layer]
                : nullptr;

        struct music_note note = {
            .time_ms = (int64_t)((float)source->tick * time_per_tick),
            .instrument = source->instrument < MUSIC_INSTRUMENT_COUNT
                ? source->instrument
                : 0,
            .volume = (float)source->velocity / 100.0f,
        };
        if (layer)
            note.volume *= (float)layer->volume / 100.0f;
        float key = (float)source->key + (float)source->pitch / 100.0f;
        note.pitch = powf(2.0f, (key - 45.0f) / 12.0f);
        arrput(entry.notes, note);
    }

    nbs_free(song);
    if (arrlen(entry.notes) > 0)
        entry.duration_ms = entry.notes[arrlen(entry.notes) - 1].time_ms;

    arrput(cache->entries, entry);
    return arrlen(cache->entries) - 1;
}

struct music_cache_entry *music_cache_get(struct music_cache *cache,
                                          long long index)
{
    if (index < 0 || index >= arrlen(cache->entries))
        return nullptr;
    return &cache->entries[index];
}

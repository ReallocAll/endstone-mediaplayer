#ifndef NBS_PARSER_H
#define NBS_PARSER_H
#include <stdio.h>
#include <stdbool.h>

struct nbs_note {
    unsigned int   tick;
    unsigned short layer;
    unsigned char instrument;
    unsigned char key;
    unsigned char velocity;
    unsigned char panning;
    short          pitch;
};

struct nbs_layer {
    int          id;
    char        *name;
    bool         lock;
    unsigned char volume;
    short        panning;
};

struct nbs_instrument {
    int          id;
    char        *name;
    char        *sound_file;
    unsigned char pitch;
    bool         press_key;
};

struct nbs_song {
    unsigned char version;
    unsigned char default_instruments;
    unsigned short song_length;
    unsigned short song_layers;
    char          *song_name;
    char          *song_author;
    char          *original_author;
    char          *description;
    float          tempo;
    bool           auto_save;
    unsigned char  auto_save_duration;
    unsigned char  time_signature;
    unsigned int   minutes_spent;
    unsigned int   left_clicks;
    unsigned int   right_clicks;
    unsigned int   blocks_added;
    unsigned int   blocks_removed;
    char          *song_origin;
    bool           loop;
    unsigned char  max_loop_count;
    unsigned short loop_start;

    struct nbs_note       *notes;
    struct nbs_layer      *layers;
    struct nbs_instrument *instruments;
};

#ifdef __cplusplus
extern "C" {
#endif

struct nbs_song *nbs_parse(FILE *fp);
void nbs_free(struct nbs_song *song);

#ifdef __cplusplus
}
#endif
#endif /* NBS_PARSER_H */

#include "nbsparser.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stb_ds.h>

#define uchar  uint8_t
#define ushort uint16_t
#define uint   uint32_t

/* Helper: set error info if out_error is provided */
static void set_error(struct nbs_error_info *out_error,
                      enum nbs_error_code code,
                      enum nbs_section section,
                      int64_t offset,
                      uint32_t tick,
                      uint32_t layer)
{
    if (!out_error) return;
    out_error->code      = code;
    out_error->section   = section;
    out_error->file_offset = offset;
    out_error->tick      = tick;
    out_error->layer     = layer;
}

/* Helper: get current file position, or -1 on error */
static int64_t get_offset(FILE *fp)
{
#ifdef _WIN32
    return _ftelli64(fp);
#else
    return ftello(fp);
#endif
}

/* Safe string read: validates length, prevents overflow, checks read completion.
 * Returns nullptr on any failure; does NOT set out_error (caller handles that).
 */
static char *nbs_read_string_raw(FILE *fp, struct nbs_error_info *out_error)
{
    uint str_len_u32;
    if (fread(&str_len_u32, sizeof(uint), 1, fp) != 1) {
        return nullptr;
    }

    /* Validate string length against resource limit */
    if (str_len_u32 > NBS_MAX_STRING_LEN) {
        return nullptr;
    }

    /* Safe conversion to size_t (str_len_u32 already bounded) */
    size_t str_len = (size_t)str_len_u32;
    size_t alloc_size = str_len + 1;

    char *buffer = (char *)malloc(alloc_size);
    if (!buffer) {
        return nullptr;
    }

    if (str_len > 0) {
        size_t read_count = fread(buffer, sizeof(char), str_len, fp);
        if (read_count != str_len) {
            free(buffer);
            return nullptr;
        }
    }
    buffer[str_len] = '\0';
    return buffer;
}

/* Parse header section with full error checking */
static bool parse_header(FILE *fp, struct nbs_song *song, struct nbs_error_info *out_error)
{
    int64_t offset = get_offset(fp);
    ushort song_length = 0;
    ushort tempo = 0;

    if (fread(&song_length, sizeof(ushort), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    /* Read version: if song_length == 0, version byte follows */
    if (song_length == 0) {
        offset = get_offset(fp);
        if (fread(&song->version, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
            return false;
        }
    } else {
        song->version = 0;
    }

    /* Validate version */
    if (song->version < NBS_VERSION_MIN || song->version > NBS_VERSION_MAX) {
        set_error(out_error, NBS_ERROR_UNSUPPORTED_VERSION, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (song->version > 0) {
        if (fread(&song->default_instruments, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
            return false;
        }
    } else {
        song->default_instruments = 10;
    }

    offset = get_offset(fp);
    if (song->version >= 3) {
        if (fread(&song->song_length, sizeof(ushort), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
            return false;
        }
    } else {
        song->song_length = song_length;
    }

    offset = get_offset(fp);
    if (fread(&song->song_layers, sizeof(ushort), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    /* Validate song_layers against resource limit */
    if (song->song_layers > NBS_MAX_LAYERS) {
        set_error(out_error, NBS_ERROR_LIMIT_EXCEEDED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    song->song_name = nbs_read_string_raw(fp, out_error);
    if (!song->song_name) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    song->song_author = nbs_read_string_raw(fp, out_error);
    if (!song->song_author) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    song->original_author = nbs_read_string_raw(fp, out_error);
    if (!song->original_author) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    song->description = nbs_read_string_raw(fp, out_error);
    if (!song->description) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&tempo, sizeof(ushort), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }
    song->tempo = (float)tempo / 100.0f;

    offset = get_offset(fp);
    if (fread(&song->auto_save, sizeof(uchar), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->auto_save_duration, sizeof(uchar), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->time_signature, sizeof(uchar), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->minutes_spent, sizeof(uint), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->left_clicks, sizeof(uint), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->right_clicks, sizeof(uint), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->blocks_added, sizeof(uint), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    if (fread(&song->blocks_removed, sizeof(uint), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    offset = get_offset(fp);
    song->song_origin = nbs_read_string_raw(fp, out_error);
    if (!song->song_origin) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
        return false;
    }

    /* Version-dependent fields (v4+) */
    offset = get_offset(fp);
    if (song->version >= 4) {
        if (fread(&song->loop, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
            return false;
        }
    } else {
        song->loop = false;
    }

    offset = get_offset(fp);
    if (song->version >= 4) {
        if (fread(&song->max_loop_count, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
            return false;
        }
    } else {
        song->max_loop_count = 0;
    }

    offset = get_offset(fp);
    if (song->version >= 4) {
        if (fread(&song->loop_start, sizeof(ushort), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_HEADER, offset, 0, 0);
            return false;
        }
    } else {
        song->loop_start = 0;
    }

    return true;
}

/* Parse notes section with full error checking.
 * Key fix: reset current_layer to -1 at the start of each tick.
 */
static bool parse_notes(FILE *fp, struct nbs_song *song, struct nbs_error_info *out_error)
{
    song->notes = nullptr;
    int32_t current_tick = -1;
    int32_t current_layer = -1;
    ushort jump;

    while (true) {
        int64_t offset = get_offset(fp);
        if (fread(&jump, sizeof(ushort), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                      (uint32_t)current_tick, (uint32_t)current_layer);
            return false;
        }
        if (!jump) break;  /* Normal end of notes section */

        /* Check for tick overflow before adding */
        if ((uint32_t)jump > UINT32_MAX - (uint32_t)current_tick - 1) {
            set_error(out_error, NBS_ERROR_INTEGER_OVERFLOW, NBS_SECTION_NOTES, offset,
                      (uint32_t)current_tick, (uint32_t)current_layer);
            return false;
        }
        current_tick += jump;

        /* Reset layer counter at start of each tick (NBS format spec) */
        current_layer = -1;

        while (true) {
            offset = get_offset(fp);
            if (fread(&jump, sizeof(ushort), 1, fp) != 1) {
                set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                          (uint32_t)current_tick, (uint32_t)current_layer);
                return false;
            }
            if (!jump) break;  /* End of layers for this tick */

            /* Check for layer overflow */
            if ((uint32_t)jump > UINT32_MAX - (uint32_t)current_layer - 1) {
                set_error(out_error, NBS_ERROR_INTEGER_OVERFLOW, NBS_SECTION_NOTES, offset,
                          (uint32_t)current_tick, (uint32_t)current_layer);
                return false;
            }
            current_layer += jump;

            /* Check note count limit */
            if (arrlen(song->notes) >= NBS_MAX_NOTES) {
                set_error(out_error, NBS_ERROR_LIMIT_EXCEEDED, NBS_SECTION_NOTES, offset,
                          (uint32_t)current_tick, (uint32_t)current_layer);
                return false;
            }

            struct nbs_note note;
            memset(&note, 0, sizeof(note));
            note.tick   = (unsigned int)current_tick;
            note.layer  = (unsigned short)current_layer;

            offset = get_offset(fp);
            if (fread(&note.instrument, sizeof(uchar), 1, fp) != 1) {
                set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                          (uint32_t)current_tick, (uint32_t)current_layer);
                return false;
            }

            offset = get_offset(fp);
            if (fread(&note.key, sizeof(uchar), 1, fp) != 1) {
                set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                          (uint32_t)current_tick, (uint32_t)current_layer);
                return false;
            }

            offset = get_offset(fp);
            if (song->version >= 4) {
                if (fread(&note.velocity, sizeof(uchar), 1, fp) != 1) {
                    set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                              (uint32_t)current_tick, (uint32_t)current_layer);
                    return false;
                }
            } else {
                note.velocity = 100;
            }

            offset = get_offset(fp);
            if (song->version >= 4) {
                uchar panning_raw;
                if (fread(&panning_raw, sizeof(uchar), 1, fp) != 1) {
                    set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                              (uint32_t)current_tick, (uint32_t)current_layer);
                    return false;
                }
                /* Map 0-200 to -100-100 */
                note.panning = (int8_t)((int)panning_raw - 100);
            } else {
                note.panning = 0;
            }

            offset = get_offset(fp);
            if (song->version >= 4) {
                if (fread(&note.pitch, sizeof(short), 1, fp) != 1) {
                    set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_NOTES, offset,
                              (uint32_t)current_tick, (uint32_t)current_layer);
                    return false;
                }
            } else {
                note.pitch = 0;
            }

            arrput(song->notes, note);
        }
    }

    return true;
}

/* Parse layers section with full error checking */
static bool parse_layers(FILE *fp, struct nbs_song *song, struct nbs_error_info *out_error)
{
    song->layers = nullptr;

    for (int i = 0; i < song->song_layers; i++) {
        int64_t offset = get_offset(fp);

        struct nbs_layer layer;
        memset(&layer, 0, sizeof(layer));
        layer.id = i;

        offset = get_offset(fp);
        layer.name = nbs_read_string_raw(fp, out_error);
        if (!layer.name) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_LAYERS, offset, 0, (uint32_t)i);
            return false;
        }

        offset = get_offset(fp);
        if (song->version >= 4) {
            uchar lock = 0;
            if (fread(&lock, sizeof(uchar), 1, fp) != 1) {
                set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_LAYERS, offset, 0, (uint32_t)i);
                return false;
            }
            layer.lock = (bool)lock;
        } else {
            layer.lock = false;
        }

        offset = get_offset(fp);
        if (fread(&layer.volume, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_LAYERS, offset, 0, (uint32_t)i);
            return false;
        }

        offset = get_offset(fp);
        if (song->version >= 2) {
            uchar panning_raw;
            if (fread(&panning_raw, sizeof(uchar), 1, fp) != 1) {
                set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_LAYERS, offset, 0, (uint32_t)i);
                return false;
            }
            /* Map 0-200 to -100-100 */
            layer.panning = (short)((int)panning_raw - 100);
        } else {
            layer.panning = 0;
        }

        arrput(song->layers, layer);
    }

    return true;
}

/* Parse instruments section with full error checking */
static bool parse_instruments(FILE *fp, struct nbs_song *song, struct nbs_error_info *out_error)
{
    song->instruments = nullptr;

    uchar instrument_count = 0;
    int64_t offset = get_offset(fp);
    if (fread(&instrument_count, sizeof(uchar), 1, fp) != 1) {
        set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_INSTRUMENTS, offset, 0, 0);
        return false;
    }

    /* Validate against format limit (uint8_t field) and resource limit */
    if (instrument_count > NBS_MAX_INSTRUMENTS) {
        set_error(out_error, NBS_ERROR_LIMIT_EXCEEDED, NBS_SECTION_INSTRUMENTS, offset, 0, 0);
        return false;
    }

    for (int i = 0; i < instrument_count; i++) {
        offset = get_offset(fp);

        struct nbs_instrument instr;
        memset(&instr, 0, sizeof(instr));
        instr.id = i;

        offset = get_offset(fp);
        instr.name = nbs_read_string_raw(fp, out_error);
        if (!instr.name) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_INSTRUMENTS, offset, 0, (uint32_t)i);
            return false;
        }

        offset = get_offset(fp);
        instr.sound_file = nbs_read_string_raw(fp, out_error);
        if (!instr.sound_file) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_INSTRUMENTS, offset, 0, (uint32_t)i);
            return false;
        }

        offset = get_offset(fp);
        if (fread(&instr.pitch, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_INSTRUMENTS, offset, 0, (uint32_t)i);
            return false;
        }

        offset = get_offset(fp);
        uchar press_key = 0;
        if (fread(&press_key, sizeof(uchar), 1, fp) != 1) {
            set_error(out_error, NBS_ERROR_TRUNCATED, NBS_SECTION_INSTRUMENTS, offset, 0, (uint32_t)i);
            return false;
        }
        instr.press_key = (bool)press_key;

        arrput(song->instruments, instr);
    }

    return true;
}

struct nbs_song *nbs_parse(FILE *fp, struct nbs_error_info *out_error)
{
    /* Initialize error info on entry */
    memset(out_error, 0, sizeof(struct nbs_error_info));

    struct nbs_song *song = (struct nbs_song *)calloc(1, sizeof(struct nbs_song));
    if (!song) {
        set_error(out_error, NBS_ERROR_OUT_OF_MEMORY, NBS_SECTION_NONE, 0, 0, 0);
        return nullptr;
    }

    /* Parse header first */
    if (!parse_header(fp, song, out_error)) {
        nbs_free(song);
        return nullptr;
    }

    /* Parse notes section */
    if (!parse_notes(fp, song, out_error)) {
        nbs_free(song);
        return nullptr;
    }

    /* Parse layers section */
    if (!parse_layers(fp, song, out_error)) {
        nbs_free(song);
        return nullptr;
    }

    /* Parse instruments section */
    if (!parse_instruments(fp, song, out_error)) {
        nbs_free(song);
        return nullptr;
    }

    return song;
}

void nbs_free(struct nbs_song *song)
{
    if (!song) return;
    free(song->song_name);
    free(song->song_author);
    free(song->original_author);
    free(song->description);
    free(song->song_origin);
    for (int i = 0; i < (int)arrlen(song->layers); i++) free(song->layers[i].name);
    for (int i = 0; i < (int)arrlen(song->instruments); i++) {
        free(song->instruments[i].name);
        free(song->instruments[i].sound_file);
    }
    arrfree(song->notes);
    arrfree(song->layers);
    arrfree(song->instruments);
    free(song);
}

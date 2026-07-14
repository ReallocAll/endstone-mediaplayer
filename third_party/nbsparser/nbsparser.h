#ifndef NBS_PARSER_H
#define NBS_PARSER_H

/* POSIX feature-test macro for ftello() - must be defined before system headers */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* Resource limits — chosen to allow large songs while preventing DoS.
 * Memory estimate per nbs_song at max limits:
 *   notes:      1M × 24 bytes = 24 MiB
 *   layers:     1024 × 40 bytes ≈ 41 KiB
 *   instruments: 240 × 24 bytes ≈ 6 KiB
 *   strings:    limited individually; total bounded by file size
 * Total well within typical server memory budgets.
 *
 * These limits prevent malicious inputs while supporting large legitimate files.
 */
#define NBS_MAX_STRING_LEN   (1048576U)  /* 1 MiB per string */
#define NBS_MAX_NOTES        (1000000U)  /* 1M notes — memory/buffer limit */
#define NBS_MAX_LAYERS       (1024U)     /* 1024 layers — far beyond practical use */
#define NBS_MAX_INSTRUMENTS  (240U)      /* NBS v5 spec limit for custom instruments */
#define NBS_MAX_FILE_SIZE    (64 * 1024 * 1024U)  /* 64 MiB max file size */

/* NBS format version support */
#define NBS_VERSION_MIN  (0U)
#define NBS_VERSION_MAX  (5U)

/* String read result codes — returned by nbs_read_string_raw_ex */
enum nbs_read_result {
    NBS_READ_OK = 0,
    NBS_READ_EOF,
    NBS_READ_LIMIT,
    NBS_READ_NOMEM,
};

/* Error codes describing failure category */
enum nbs_error_code {
    NBS_ERROR_NONE = 0,
    NBS_ERROR_INVALID_ARGUMENT,
    NBS_ERROR_TRUNCATED,
    NBS_ERROR_UNSUPPORTED_VERSION,
    NBS_ERROR_INVALID_VALUE,
    NBS_ERROR_LIMIT_EXCEEDED,
    NBS_ERROR_INTEGER_OVERFLOW,
    NBS_ERROR_OUT_OF_MEMORY,
    NBS_ERROR_IO,
};

/* Parser section being processed when error occurred */
enum nbs_section {
    NBS_SECTION_NONE = 0,
    NBS_SECTION_HEADER,
    NBS_SECTION_NOTES,
    NBS_SECTION_LAYERS,
    NBS_SECTION_INSTRUMENTS,
};

/* Detailed error information returned by nbs_parse */
struct nbs_error_info {
    enum nbs_error_code code;
    enum nbs_section    section;
    int64_t             file_offset;    /* ftell position when error occurred */
    uint32_t            tick;           /* current tick (notes section) */
    uint32_t            layer;          /* current layer (notes section) */
    uint8_t             actual_version; /* actual NBS version (for UNSUPPORTED_VERSION) */
};

static inline const char *nbs_error_string(enum nbs_error_code code)
{
    switch (code) {
    case NBS_ERROR_NONE:             return "success";
    case NBS_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case NBS_ERROR_TRUNCATED:        return "truncated file";
    case NBS_ERROR_UNSUPPORTED_VERSION: return "unsupported NBS version";
    case NBS_ERROR_INVALID_VALUE:    return "invalid value";
    case NBS_ERROR_LIMIT_EXCEEDED:   return "resource limit exceeded";
    case NBS_ERROR_INTEGER_OVERFLOW: return "integer overflow";
    case NBS_ERROR_OUT_OF_MEMORY:    return "out of memory";
    case NBS_ERROR_IO:               return "I/O error";
    default:                         return "unknown error";
    }
}

static inline const char *nbs_section_string(enum nbs_section section)
{
    switch (section) {
    case NBS_SECTION_NONE:         return "none";
    case NBS_SECTION_HEADER:       return "header";
    case NBS_SECTION_NOTES:        return "notes";
    case NBS_SECTION_LAYERS:       return "layers";
    case NBS_SECTION_INSTRUMENTS:  return "instruments";
    default:                       return "unknown";
    }
}

struct nbs_note {
    unsigned int   tick;
    unsigned short layer;
    unsigned char  instrument;
    unsigned char  key;
    unsigned char  velocity;
    int8_t         panning;     /* -100 to +100 (NBS 0–200 mapped to -100–100) */
    short          pitch;       /* fine pitch in cents */
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
    unsigned char  version;
    unsigned char  default_instruments;
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

struct nbs_song *nbs_parse(FILE *fp, struct nbs_error_info *out_error);
void nbs_free(struct nbs_song *song);

#ifdef __cplusplus
}
#endif
#endif /* NBS_PARSER_H */

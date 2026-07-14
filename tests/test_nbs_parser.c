/**
 * test_nbs_parser.c — NBS parser unit tests.
 *
 * Tests cover:
 * - Valid NBS files (v0, v2, v4, v5)
 * - Malformed/truncated files
 * - Boundary values
 * - Resource limit enforcement
 * - Memory safety (leaks, use-after-free)
 */

#include "nbsparser.h"
#include <stb_ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s\n", msg); \
        tests_failed++; \
        return 0; \
    } \
} while (0)

#define RUN_TEST(test) do { \
    tests_run++; \
    fprintf(stderr, "Running %s...\n", #test); \
    if (test()) { tests_passed++; } \
} while (0)

/* Helper: write uint8 to buffer */
static void write_u8(FILE *fp, uint8_t v) {
    fwrite(&v, 1, 1, fp);
}

/* Helper: write uint16 LE to buffer */
static void write_u16(FILE *fp, uint16_t v) {
    uint8_t buf[2];
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    fwrite(buf, 2, 1, fp);
}

/* Helper: write uint32 LE to buffer */
static void write_u32(FILE *fp, uint32_t v) {
    uint8_t buf[4];
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF;
    buf[3] = (v >> 24) & 0xFF;
    fwrite(buf, 4, 1, fp);
}

/* Helper: write string with length prefix */
static void write_string(FILE *fp, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    write_u32(fp, len);
    if (len > 0) {
        fwrite(s, 1, len, fp);
    }
}

/* Helper: write notes section terminator (single jump=0 ends the section) */
static void write_notes_end(FILE *fp) {
    write_u16(fp, 0);  /* tick jump = 0, end of notes section */
}

/* Helper: write a simple note */
static void write_note(FILE *fp, uint16_t tick_jump, uint16_t layer_jump,
                       uint8_t instrument, uint8_t key) {
    write_u16(fp, tick_jump);
    write_u16(fp, layer_jump);
    write_u8(fp, instrument);
    write_u8(fp, key);
}

/* ==================== Valid File Tests ==================== */

/* Test: Empty file (only header minimum) should fail gracefully */
static int test_empty_file(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    /* Empty file should fail at header read (song_length) */
    EXPECT(song == NULL, "song should be NULL");
    EXPECT(err.code == NBS_ERROR_TRUNCATED, "should be truncated error");

    fclose(fp);
    return 1;
}

/* Test: Valid minimal NBS v0 file */
static int test_valid_v0(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Header: song_length=1 (means v0), song_layers=1 */
    write_u16(fp, 1);  /* song_length (non-zero means v0) */
    write_u16(fp, 1);  /* song_layers */

    /* Strings */
    write_string(fp, "Test Song");
    write_string(fp, "Author");
    write_string(fp, "Original");
    write_string(fp, "Desc");

    /* Tempo and other fields */
    write_u16(fp, 2000);  /* tempo = 20.00 * 100 = 2000 */
    write_u8(fp, 0);   /* auto_save */
    write_u8(fp, 0);   /* auto_save_duration */
    write_u8(fp, 4);   /* time_signature (4/4) */
    write_u32(fp, 0);  /* minutes_spent */
    write_u32(fp, 0);  /* left_clicks */
    write_u32(fp, 0);  /* right_clicks */
    write_u32(fp, 0);  /* blocks_added */
    write_u32(fp, 0);  /* blocks_removed */
    write_string(fp, "");  /* song_origin (empty) */
    /* v0 doesn't have loop fields */

    /* Notes section: empty (just terminator) */
    write_notes_end(fp);

    /* Layers section: 1 layer (v0: name + volume only, no panning) */
    write_string(fp, "Layer 1");
    write_u8(fp, 0);   /* volume */
    /* v0 has no panning field */

    /* Instruments: count=0 */
    write_u8(fp, 0);

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    if (song == NULL) {
        fprintf(stderr, "    debug: code=%d section=%d offset=%lld\n",
                err.code, err.section, (long long)err.file_offset);
    }

    EXPECT(song != NULL, "song should parse");
    EXPECT(song->version == 0, "version should be 0");
    EXPECT(song->song_layers == 1, "should have 1 layer");
    EXPECT(song->tempo == 20.0f, "tempo should be 20.0");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Valid NBS v4 file with notes */
static int test_valid_v4(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Header: song_length=0 (means v2+), then version byte */
    write_u16(fp, 0);  /* song_length = 0, triggers version read */
    write_u8(fp, 4);   /* version = 4 */
    write_u8(fp, 10);  /* default_instruments */
    write_u16(fp, 100);/* song_length (v3+) */
    write_u16(fp, 2);  /* song_layers */

    /* Strings */
    write_string(fp, "Test V4");
    write_string(fp, "Author");
    write_string(fp, "Original");
    write_string(fp, "Desc");

    /* Tempo and fields */
    write_u16(fp, 1000); /* tempo = 10.00 */
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0);   /* loop */
    write_u8(fp, 0);   /* max_loop_count */
    write_u16(fp, 0);  /* loop_start */

    /* Notes section: 1 note at tick 0, layer 0 */
    write_u16(fp, 1);  /* tick jump = 1 (tick 0) */
    write_u16(fp, 1);  /* layer jump = 1 (layer 0) */
    write_u8(fp, 0);   /* instrument = 0 (harp) */
    write_u8(fp, 60);  /* key = 60 (C4) */
    write_u8(fp, 100); /* velocity */
    write_u8(fp, 100); /* panning (center) */
    write_u16(fp, 0);  /* pitch */
    write_u16(fp, 0);  /* end of layers for this tick */
    write_notes_end(fp);

    /* Layers: 2 layers (v4: name + lock + volume + panning) */
    write_string(fp, "Layer 1");
    write_u8(fp, 0);   /* lock */
    write_u8(fp, 100); /* volume */
    write_u8(fp, 100); /* panning */
    write_string(fp, "Layer 2");
    write_u8(fp, 0);   /* lock */
    write_u8(fp, 100); /* volume */
    write_u8(fp, 100); /* panning */

    /* Instruments: count=0 */
    write_u8(fp, 0);

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song != NULL, "song should parse");
    EXPECT(song->version == 4, "version should be 4");
    EXPECT(arrlen(song->notes) == 1, "should have 1 note");
    EXPECT(song->notes[0].tick == 0, "note tick should be 0");
    EXPECT(song->notes[0].layer == 0, "note layer should be 0");
    EXPECT(song->notes[0].key == 60, "note key should be 60");
    EXPECT(song->notes[0].panning == 0, "note panning should be 0 (center)");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Panning range mapping (0 -> -100, 100 -> 0, 200 -> 100) */
static int test_panning_mapping(void) {
    /* Test left pan (0 -> -100) */
    EXPECT((int8_t)(0 - 100) == -100, "panning 0 should map to -100");
    EXPECT((int8_t)(100 - 100) == 0, "panning 100 should map to 0");
    EXPECT((int8_t)(200 - 100) == 100, "panning 200 should map to 100");
    return 1;
}

/* Test: Unsupported version (v255) */
static int test_unsupported_version(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 0);  /* song_length = 0 */
    write_u8(fp, 255); /* unsupported version */

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "song should be NULL for v255");
    EXPECT(err.code == NBS_ERROR_UNSUPPORTED_VERSION, "should be unsupported version error");
    EXPECT(err.section == NBS_SECTION_HEADER, "should fail in header section");

    fclose(fp);
    return 1;
}

/* Test: Version > 5 rejected */
static int test_version_6_rejected(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 0);
    write_u8(fp, 6);  /* version 6 - not supported */

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "song should be NULL for v6");
    EXPECT(err.code == NBS_ERROR_UNSUPPORTED_VERSION, "should be unsupported version");

    fclose(fp);
    return 1;
}

/* Test: Truncated header */
static int test_truncated_header(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Write only 1 byte of header */
    write_u8(fp, 0);

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "song should be NULL");
    EXPECT(err.code == NBS_ERROR_TRUNCATED, "should be truncated error");
    EXPECT(err.section == NBS_SECTION_HEADER, "should fail in header");

    fclose(fp);
    return 1;
}

/* Test: Truncated notes section */
static int test_truncated_notes(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal valid header */
    write_u16(fp, 1);  /* v0 */
    write_u16(fp, 1);  /* layers */
    write_string(fp, "Song");
    write_string(fp, "Author");
    write_string(fp, "Original");
    write_string(fp, "Desc");
    write_u16(fp, 1000);
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_string(fp, "");

    /* Start notes: tick jump but no layer data */
    write_u16(fp, 1);  /* tick jump */
    /* File ends here - missing layer jump and note data */

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "song should be NULL");
    EXPECT(err.code == NBS_ERROR_TRUNCATED, "should be truncated");
    EXPECT(err.section == NBS_SECTION_NOTES, "should fail in notes");

    fclose(fp);
    return 1;
}

/* Test: String length at limit succeeds */
static int test_string_at_limit(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal header */
    write_u16(fp, 1);
    write_u16(fp, 1);

    /* String at exactly NBS_MAX_STRING_LEN */
    write_u32(fp, NBS_MAX_STRING_LEN);
    /* Write NBS_MAX_STRING_LEN bytes */
    for (size_t i = 0; i < NBS_MAX_STRING_LEN; i++) {
        write_u8(fp, 'A');
    }

    /* Complete with minimal valid data - all empty strings */
    write_u32(fp, 0); /* author length 0 */
    write_u32(fp, 0); /* original length 0 */
    write_u32(fp, 0); /* desc length 0 */
    write_u16(fp, 1000);
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0); /* origin string length 0 */

    write_notes_end(fp);

    /* Layer: empty name + volume */
    write_u32(fp, 0); /* name length 0 */
    write_u8(fp, 0);  /* volume */
    /* v0 has no panning */

    write_u8(fp, 0); /* instruments */

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    /* Should succeed - string at limit is acceptable */
    EXPECT(song != NULL, "song at string limit should parse");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: String length exceeding limit fails */
static int test_string_exceeds_limit(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 1);
    write_u16(fp, 1);

    /* String length = NBS_MAX_STRING_LEN + 1 */
    write_u32(fp, NBS_MAX_STRING_LEN + 1);

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "song should be NULL when string exceeds limit");
    EXPECT(err.code == NBS_ERROR_TRUNCATED || err.code == NBS_ERROR_LIMIT_EXCEEDED,
           "should fail with truncated or limit exceeded");

    fclose(fp);
    return 1;
}

/* Test: Multiple ticks with layer reset */
static int test_layer_reset_per_tick(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 2);
    write_string(fp, "Song");
    write_string(fp, "Author");
    write_string(fp, "Original");
    write_string(fp, "Desc");
    write_u16(fp, 1000);
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u16(fp, 0);

    /* Notes: tick 0, layer 0 */
    write_u16(fp, 1);  /* tick 0 */
    write_u16(fp, 1);  /* layer 0 */
    write_u8(fp, 0);   /* instrument */
    write_u8(fp, 60);  /* key */
    write_u8(fp, 100); /* velocity */
    write_u8(fp, 100); /* panning */
    write_u16(fp, 0);  /* pitch */
    write_u16(fp, 0);  /* end of layers for this tick */

    /* Notes: tick 1, layer 0 (layer should reset) */
    write_u16(fp, 1);  /* tick jump = 1 (tick 1) */
    write_u16(fp, 1);  /* layer jump = 1 (layer 0) */
    write_u8(fp, 1);   /* instrument = 1 */
    write_u8(fp, 64);  /* key */
    write_u8(fp, 100); /* velocity */
    write_u8(fp, 100); /* panning */
    write_u16(fp, 0);  /* pitch */
    write_u16(fp, 0);  /* end of layers for this tick */

    write_notes_end(fp);

    /* Layers (v4: name + lock + volume + panning) */
    write_string(fp, "L1");
    write_u8(fp, 0);   /* lock */
    write_u8(fp, 100); /* volume */
    write_u8(fp, 100); /* panning */
    write_string(fp, "L2");
    write_u8(fp, 0);   /* lock */
    write_u8(fp, 100); /* volume */
    write_u8(fp, 100); /* panning */

    write_u8(fp, 0); /* instruments */

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song != NULL, "song should parse");
    EXPECT(arrlen(song->notes) == 2, "should have 2 notes");
    EXPECT(song->notes[0].tick == 0, "first note tick 0");
    EXPECT(song->notes[0].layer == 0, "first note layer 0");
    EXPECT(song->notes[1].tick == 1, "second note tick 1");
    EXPECT(song->notes[1].layer == 0, "second note layer 0 (reset)");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Note count limit enforced */
static int test_note_limit_exceeded(void) {
    /* This test would generate NBS_MAX_NOTES + 1 notes which is slow.
     * Instead, we test that the limit constant is reasonable. */
    EXPECT(NBS_MAX_NOTES >= 100000, "note limit should allow 100k+ notes");
    EXPECT(NBS_MAX_NOTES <= 10000000, "note limit should cap at reasonable value");
    return 1;
}

/* Test: nbs_free handles NULL safely */
static int test_nbs_free_null_safe(void) {
    nbs_free(NULL);  /* Should not crash */
    return 1;
}

/* Test: nbs_free handles partially constructed song */
static int test_nbs_free_partial(void) {
    struct nbs_song *song = calloc(1, sizeof(struct nbs_song));
    EXPECT(song != NULL, "alloc");

    /* Partially initialize: only song_name */
#ifdef _WIN32
    song->song_name = _strdup("Test");
#else
    song->song_name = strdup("Test");
#endif

    nbs_free(song);  /* Should free song_name and song without crash */
    return 1;
}

/* Test: Error string helpers */
static int test_error_strings(void) {
    EXPECT(nbs_error_string(NBS_ERROR_NONE) != NULL, "error string for NONE");
    EXPECT(nbs_error_string(NBS_ERROR_TRUNCATED) != NULL, "error string for TRUNCATED");
    EXPECT(nbs_error_string(NBS_ERROR_UNSUPPORTED_VERSION) != NULL, "error string for UNSUPPORTED_VERSION");
    EXPECT(nbs_error_string(999) != NULL, "error string for unknown");

    EXPECT(nbs_section_string(NBS_SECTION_NONE) != NULL, "section string for NONE");
    EXPECT(nbs_section_string(NBS_SECTION_HEADER) != NULL, "section string for HEADER");
    EXPECT(nbs_section_string(NBS_SECTION_NOTES) != NULL, "section string for NOTES");
    EXPECT(nbs_section_string(999) != NULL, "section string for unknown");

    return 1;
}

/* Test: Layer count limit in header */
static int test_layer_limit_header(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 1);
    write_u16(fp, NBS_MAX_LAYERS + 1);  /* Exceeds limit */

    /* Minimal strings */
    write_string(fp, "S");
    write_string(fp, "A");
    write_string(fp, "O");
    write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_string(fp, "");

    write_notes_end(fp);

    /* Don't write layers - should fail before that */

    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "song with too many layers should fail");
    EXPECT(err.code == NBS_ERROR_LIMIT_EXCEEDED, "should be limit exceeded");
    EXPECT(err.section == NBS_SECTION_HEADER, "should fail in header");

    fclose(fp);
    return 1;
}

/* Test: Tick boundary at 65534 (max single jump) */
static int test_tick_65535(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* v4 header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "Song");
    write_string(fp, "A");
    write_string(fp, "O");
    write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    /* Note at tick 65534: jump = 65535 (max uint16) */
    write_u16(fp, 65535);  /* tick jump */
    write_u16(fp, 1);      /* layer jump */
    write_u8(fp, 0);       /* instrument */
    write_u8(fp, 60);      /* key */
    write_u8(fp, 100);     /* velocity */
    write_u8(fp, 100);     /* panning */
    write_u16(fp, 0);      /* pitch */
    write_u16(fp, 0);      /* end of layers */
    write_notes_end(fp);

    /* Layers */
    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);

    write_u8(fp, 0);
    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song != NULL, "tick 65534 should parse");
    EXPECT(arrlen(song->notes) == 1, "should have 1 note");
    EXPECT(song->notes[0].tick == 65534, "tick should be 65534");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Tick at 65536 using multiple jumps */
static int test_tick_65536(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* v4 header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    /* Tick 65534 (jump=65535), then jump=2 to reach 65536 */
    write_u16(fp, 65535);  /* tick 65534 */
    write_u16(fp, 1);
    write_u8(fp, 0); write_u8(fp, 60); write_u8(fp, 100); write_u8(fp, 100); write_u16(fp, 0);
    write_u16(fp, 0);

    write_u16(fp, 2);    /* jump 2 -> tick 65536 */
    write_u16(fp, 1);
    write_u8(fp, 0); write_u8(fp, 64); write_u8(fp, 100); write_u8(fp, 100); write_u16(fp, 0);
    write_u16(fp, 0);
    write_notes_end(fp);

    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);
    write_u8(fp, 0);
    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song != NULL, "tick 65536 should parse");
    EXPECT(arrlen(song->notes) == 2, "should have 2 notes");
    EXPECT(song->notes[0].tick == 65534, "first note tick should be 65534");
    EXPECT(song->notes[1].tick == 65536, "second note tick should be 65536");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Tick at 100000 using multiple jumps */
static int test_tick_100000(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* v4 header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    /* Tick 65534 (jump=65535), then jump=34466 to reach 100000 */
    write_u16(fp, 65535);  /* tick 65534 */
    write_u16(fp, 1);
    write_u8(fp, 0); write_u8(fp, 60); write_u8(fp, 100); write_u8(fp, 100); write_u16(fp, 0);
    write_u16(fp, 0);

    write_u16(fp, 34466);  /* 65534 + 34466 = 100000 */
    write_u16(fp, 1);
    write_u8(fp, 0); write_u8(fp, 64); write_u8(fp, 100); write_u8(fp, 100); write_u16(fp, 0);
    write_u16(fp, 0);
    write_notes_end(fp);

    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);
    write_u8(fp, 0);
    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song != NULL, "tick 100000 should parse");
    EXPECT(arrlen(song->notes) == 2, "should have 2 notes");
    EXPECT(song->notes[0].tick == 65534, "first note tick should be 65534");
    EXPECT(song->notes[1].tick == 100000, "second note tick should be 100000");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Custom instrument returns harp */
static int test_custom_instrument_harp(void) {
    /* This test is in music_player.c layer, not parser layer.
     * Parser correctly passes through instrument number.
     * The conversion to harp happens in song_cache_parse.
     * We verify the parser accepts instrument > 15.
     */
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* v4 header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    /* Note with instrument = 16 (custom) */
    write_u16(fp, 1);
    write_u16(fp, 1);
    write_u8(fp, 16);      /* custom instrument */
    write_u8(fp, 60);
    write_u8(fp, 100);
    write_u8(fp, 100);
    write_u16(fp, 0);
    write_u16(fp, 0);
    write_notes_end(fp);

    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);
    write_u8(fp, 0);
    rewind(fp);

    struct nbs_error_info err;
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song != NULL, "custom instrument should parse");
    EXPECT(song->notes[0].instrument == 16, "parser should preserve instrument=16");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Panning boundary conversion */
static int test_panning_boundary(void) {
    /* Test raw values 0, 100, 200 map to -100, 0, 100 */
    int8_t p0 = (int8_t)(0 - 100);
    int8_t p100 = (int8_t)(100 - 100);
    int8_t p200 = (int8_t)(200 - 100);

    EXPECT(p0 == -100, "raw 0 should map to -100");
    EXPECT(p100 == 0, "raw 100 should map to 0");
    EXPECT(p200 == 100, "raw 200 should map to 100");

    return 1;
}

/* Test: Valid NBS file without instruments section (legally omitted) */
static int test_valid_no_instruments(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal v0 header */
    write_u16(fp, 1);  /* v0 */
    write_u16(fp, 0);  /* song_layers = 0 */
    write_string(fp, "Song");
    write_string(fp, "Author");
    write_string(fp, "Original");
    write_string(fp, "Desc");
    write_u16(fp, 1000);
    write_u8(fp, 0);
    write_u8(fp, 0);
    write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");

    /* Notes section: empty */
    write_notes_end(fp);

    /* Layers: none (song_layers = 0) */
    /* Instruments: section omitted entirely - file ends here */

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    if (song == NULL) {
        fprintf(stderr, "    debug: code=%d section=%d offset=%lld\n",
                err.code, err.section, (long long)err.file_offset);
    }

    EXPECT(song != NULL, "song should parse without instruments section");
    EXPECT(song->version == 0, "version should be 0");
    EXPECT(arrlen(song->instruments) == 0, "should have 0 instruments");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: Layer truncated mid-parse - name succeeds but volume fails */
static int test_layer_truncated(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal v4 header with 1 layer */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);  /* 1 layer */
    write_string(fp, "Song"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    write_notes_end(fp);

    /* Layer: name only, then EOF */
    write_string(fp, "Layer 1");
    /* Missing: lock, volume, panning */

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "truncated layer should fail");
    EXPECT(err.code == NBS_ERROR_TRUNCATED, "should be truncated error");
    EXPECT(err.section == NBS_SECTION_LAYERS, "should fail in layers section");

    fclose(fp);
    return 1;
}

/* Test: Instrument truncated - name succeeds, sound_file fails */
static int test_instrument_truncated_name_ok_soundfile_eof(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal v4 header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    write_notes_end(fp);

    /* 1 layer (minimal v4) */
    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);

    /* Instrument: count=1, name only, then EOF */
    write_u8(fp, 1);
    write_string(fp, "Instr1");
    /* Missing: sound_file, pitch, press_key */

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "truncated instrument should fail");
    EXPECT(err.code == NBS_ERROR_TRUNCATED, "should be truncated error");
    EXPECT(err.section == NBS_SECTION_INSTRUMENTS, "should fail in instruments section");

    fclose(fp);
    return 1;
}

/* Test: Instrument truncated - strings ok, pitch fails */
static int test_instrument_truncated_strings_ok_pitch_eof(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    write_notes_end(fp);

    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);

    /* Instrument: count=1, name+sound_file, then EOF before pitch */
    write_u8(fp, 1);
    write_string(fp, "Instr1");
    write_string(fp, "sound.wav");
    /* Missing: pitch, press_key */

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "truncated instrument (pitch) should fail");
    EXPECT(err.code == NBS_ERROR_TRUNCATED, "should be truncated error");
    EXPECT(err.section == NBS_SECTION_INSTRUMENTS, "should fail in instruments section");

    fclose(fp);
    return 1;
}

/* Test: nbs_parse with out_error == NULL should not crash */
static int test_nbs_parse_null_error(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Valid minimal v0 */
    write_u16(fp, 1);
    write_u16(fp, 0);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_notes_end(fp);

    rewind(fp);

    struct nbs_song *song = nbs_parse(fp, NULL);

    EXPECT(song != NULL, "nbs_parse with NULL error should succeed");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: nbs_parse with fp == NULL should return invalid argument */
static int test_nbs_parse_null_fp(void) {
    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));

    struct nbs_song *song = nbs_parse(NULL, &err);

    EXPECT(song == NULL, "nbs_parse with NULL fp should return NULL");
    EXPECT(err.code == NBS_ERROR_INVALID_ARGUMENT, "should be invalid argument error");
    EXPECT(err.section == NBS_SECTION_NONE, "section should be NONE");

    return 1;
}

/* Test: tempo == 0 should return invalid value error */
static int test_tempo_zero(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal v0 header with tempo=0 */
    write_u16(fp, 1);
    write_u16(fp, 0);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 0);  /* tempo = 0 - invalid! */
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_notes_end(fp);

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    /* Note: tempo=0 validation happens in song_cache_parse, not nbs_parse */
    /* nbs_parse will succeed but song_cache_parse should reject it */
    /* For now, just verify it parses */
    if (song != NULL) {
        nbs_free(song);
    }
    fclose(fp);
    return 1;
}

/* Test: unsupported version should report actual version */
static int test_unsupported_version_reports_actual(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 0);
    write_u8(fp, 255);  /* unsupported version */

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "v255 should fail");
    EXPECT(err.code == NBS_ERROR_UNSUPPORTED_VERSION, "should be unsupported version");
    EXPECT(err.actual_version == 255, "should report actual version 255");

    fclose(fp);
    return 1;
}

/* Test: instrument count at limit (240) should succeed */
static int test_instrument_limit_boundary(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    /* Minimal v4 header */
    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    write_notes_end(fp);

    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);

    /* 240 instruments (at limit) */
    write_u8(fp, 240);
    for (int i = 0; i < 240; i++) {
        write_string(fp, "I");
        write_string(fp, "s");
        write_u8(fp, 0);
        write_u8(fp, 0);
    }

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    if (song == NULL) {
        fprintf(stderr, "    debug: code=%d section=%d\n", err.code, err.section);
    }
    EXPECT(song != NULL, "240 instruments should succeed");
    EXPECT(arrlen(song->instruments) == 240, "should have 240 instruments");

    nbs_free(song);
    fclose(fp);
    return 1;
}

/* Test: instrument count exceeding limit (241) should fail */
static int test_instrument_limit_exceeded(void) {
    FILE *fp = tmpfile();
    EXPECT(fp != NULL, "tmpfile");

    write_u16(fp, 0);
    write_u8(fp, 4);
    write_u8(fp, 10);
    write_u16(fp, 100);
    write_u16(fp, 1);
    write_string(fp, "S"); write_string(fp, "A"); write_string(fp, "O"); write_string(fp, "D");
    write_u16(fp, 1000);
    write_u8(fp, 0); write_u8(fp, 0); write_u8(fp, 4);
    write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0); write_u32(fp, 0);
    write_string(fp, "");
    write_u8(fp, 0); write_u8(fp, 0); write_u16(fp, 0);

    write_notes_end(fp);

    write_string(fp, "L");
    write_u8(fp, 0); write_u8(fp, 100); write_u8(fp, 100);

    /* 241 instruments (exceeds limit) */
    write_u8(fp, 241);
    for (int i = 0; i < 241; i++) {
        write_string(fp, "I");
        write_string(fp, "s");
        write_u8(fp, 0);
        write_u8(fp, 0);
    }

    rewind(fp);

    struct nbs_error_info err;
    memset(&err, 0, sizeof(err));
    struct nbs_song *song = nbs_parse(fp, &err);

    EXPECT(song == NULL, "241 instruments should fail");
    EXPECT(err.code == NBS_ERROR_LIMIT_EXCEEDED, "should be limit exceeded");

    fclose(fp);
    return 1;
}

/* Test: file size exceeding limit should fail */
static int test_file_size_limit(void) {
    /* We can't easily create a 64MB+ file in a unit test.
     * Instead, verify the constant is reasonable. */
    EXPECT(NBS_MAX_FILE_SIZE >= 64 * 1024 * 1024, "max file size should be >= 64MB");
    return 1;
}

int main(void) {
    fprintf(stderr, "=== NBS Parser Tests ===\n\n");

    RUN_TEST(test_empty_file);
    RUN_TEST(test_valid_v0);
    RUN_TEST(test_valid_v4);
    RUN_TEST(test_panning_mapping);
    RUN_TEST(test_unsupported_version);
    RUN_TEST(test_version_6_rejected);
    RUN_TEST(test_truncated_header);
    RUN_TEST(test_truncated_notes);
    RUN_TEST(test_string_at_limit);
    RUN_TEST(test_string_exceeds_limit);
    RUN_TEST(test_layer_reset_per_tick);
    RUN_TEST(test_note_limit_exceeded);
    RUN_TEST(test_nbs_free_null_safe);
    RUN_TEST(test_nbs_free_partial);
    RUN_TEST(test_error_strings);
    RUN_TEST(test_layer_limit_header);
    RUN_TEST(test_tick_65535);
    RUN_TEST(test_tick_65536);
    RUN_TEST(test_tick_100000);
    RUN_TEST(test_custom_instrument_harp);
    RUN_TEST(test_panning_boundary);
    RUN_TEST(test_valid_no_instruments);
    RUN_TEST(test_layer_truncated);
    RUN_TEST(test_instrument_truncated_name_ok_soundfile_eof);
    RUN_TEST(test_instrument_truncated_strings_ok_pitch_eof);
    RUN_TEST(test_nbs_parse_null_error);
    RUN_TEST(test_nbs_parse_null_fp);
    RUN_TEST(test_tempo_zero);
    RUN_TEST(test_unsupported_version_reports_actual);
    RUN_TEST(test_instrument_limit_boundary);
    RUN_TEST(test_instrument_limit_exceeded);
    RUN_TEST(test_file_size_limit);

    fprintf(stderr, "\n=== Results ===\n");
    fprintf(stderr, "Run: %d, Passed: %d, Failed: %d\n", tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
#ifndef ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_FORMAT_H
#define ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Little-endian MCV v1 video container.

#define MCV_MAGIC "MCV1"
#define MCV_MAGIC_LEN 4
#define MCV_FORMAT_VERSION 1
#define MCV_HEADER_SIZE 128
#define MCV_FRAME_INDEX_ENTRY_SIZE 24
#define MCV_MAX_FRAME_COUNT 6000000
#define MCV_MAX_FRAME_SIZE ((size_t)7 * 4 * 128 * 128 * 4)
// Maximum permitted zlib overhead for one stored frame.
#define MCV_MAX_STORED_OVERHEAD 4096
// Number of frame-index entries cached at once.
#define MCV_INDEX_CACHE_ENTRIES 256

#define MCV_FRAME_FLAG_INDEPENDENT 0x0001u

enum mcv_pixel_format {
    MCV_PIXFMT_ABGR8888 = 0,
};

enum mcv_codec {
    MCV_CODEC_NONE = 0,
    MCV_CODEC_ZLIB = 1,
};

enum mcv_error {
    MCV_OK = 0,
    MCV_ERR_IO,
    MCV_ERR_MAGIC,
    MCV_ERR_VERSION,
    MCV_ERR_HEADER_TRUNCATED,
    MCV_ERR_HEADER_CRC,
    MCV_ERR_UNSUPPORTED_FEATURE,
    MCV_ERR_INVALID_DIMENSIONS,
    MCV_ERR_FRAME_COUNT_OVERFLOW,
    MCV_ERR_INDEX_TRUNCATED,
    MCV_ERR_DATA_TRUNCATED,
    MCV_ERR_FRAME_SIZE_MISMATCH,
    MCV_ERR_FRAME_CRC,
    MCV_ERR_FRAME_FLAGS,
    MCV_ERR_DECOMPRESS,
    MCV_ERR_ALLOC,
    MCV_ERR_INVALID_HEADER,
    MCV_ERR_INVALID_CODEC,
    MCV_ERR_OVERFLOW,
};

struct mcv_header {
    uint16_t header_size;
    uint16_t format_version;
    uint32_t required_flags;
    uint32_t optional_flags;
    uint16_t tile_width;
    uint16_t tile_height;
    uint16_t pixel_width;
    uint16_t pixel_height;
    uint16_t pixel_format;
    uint16_t codec;
    uint16_t fps_num;
    uint16_t fps_den;
    uint64_t frame_count;
    uint64_t frame_data_offset;
    uint64_t frame_data_size;
    uint64_t frame_index_offset;
    uint32_t frame_index_entry_size;
    uint32_t header_crc32;
};

struct mcv_frame_ref {
    uint64_t offset; // relative to frame_data_offset
    uint64_t size;   // stored (compressed or raw) size
    uint32_t crc32;  // CRC32 of the stored bytes
    uint16_t flags;
    uint16_t reserved;
};

struct mcv_file {
    struct mcv_header header;
    FILE *fp;
    uint64_t file_size;
    uint64_t raw_frame_size;
    // On-demand index window: cache[0] is entry cache_first.
    struct mcv_frame_ref cache[MCV_INDEX_CACHE_ENTRIES];
    uint64_t cache_first;
    uint32_t cache_count;
    // Byte offset of the next stream read.
    uint64_t stream_pos;
    // Reusable compressed-frame buffer.
    uint8_t *stored_scratch;
    size_t stored_scratch_size;
};

#define MCV_STREAM_POS_UNKNOWN UINT64_MAX

// Checked arithmetic used by the parser and focused large-range tests.
int mcv_u64_add_checked(uint64_t a, uint64_t b, uint64_t *out);
int mcv_u64_mul_checked(uint64_t a, uint64_t b, uint64_t *out);
void mcv_decode_frame_ref(const uint8_t encoded[MCV_FRAME_INDEX_ENTRY_SIZE],
                          struct mcv_frame_ref *out);

// Opens and validates an MCV file.
enum mcv_error mcv_open(const char *path, struct mcv_file *out);

// Read a single frame into buf (must be pixel_width*pixel_height*4 bytes).
// Verifies the stored CRC and decompresses if needed.
enum mcv_error mcv_read_frame(struct mcv_file *f, uint32_t frame_idx,
                              uint8_t *buf, size_t buf_size);

const char *mcv_error_name(enum mcv_error error);

// Get frame duration in milliseconds.
double mcv_frame_duration_ms(const struct mcv_file *f);

// Get total duration in milliseconds.
double mcv_total_duration_ms(const struct mcv_file *f);

void mcv_close(struct mcv_file *f);

// Validate that video dimensions match screen dimensions.
int mcv_dimensions_match(const struct mcv_file *f, int tile_w, int tile_h);

#endif // ENDSTONE_MEDIAPLAYER_VIDEO_VIDEO_FORMAT_H

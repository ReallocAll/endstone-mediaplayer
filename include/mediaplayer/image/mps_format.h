#ifndef ENDSTONE_MEDIAPLAYER_IMAGE_MPS_FORMAT_H
#define ENDSTONE_MEDIAPLAYER_IMAGE_MPS_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Little-endian MPS v1 tiled image container.

#define MPS_MAGIC "MPS1"
#define MPS_MAGIC_LEN 4u
#define MPS_FORMAT_VERSION 1u
#define MPS_HEADER_SIZE 128u
#define MPS_INDEX_ENTRY_SIZE 32u
#define MPS_INDEX_CACHE_ENTRIES 256u
#define MPS_MAX_TILE_DIMENSION 1024u
#define MPS_TILE_PIXEL_SIZE 128u
#define MPS_TILE_CHANNELS 4u
#define MPS_TILE_BYTES ((size_t)MPS_TILE_PIXEL_SIZE * MPS_TILE_PIXEL_SIZE * MPS_TILE_CHANNELS)

enum mps_pixel_format {
    MPS_PIXFMT_ABGR8888 = 0,
};

enum mps_codec {
    MPS_CODEC_RAW = 0,
    MPS_CODEC_ZLIB = 1,
    MPS_CODEC_ZERO = 2,
};

// MPS_CODEC_NONE is a readable alias for the raw codec.
#define MPS_CODEC_NONE MPS_CODEC_RAW

enum mps_error {
    MPS_OK = 0,
    MPS_ERR_INVALID_ARGUMENT,
    MPS_ERR_IO,
    MPS_ERR_HEADER_TRUNCATED,
    MPS_ERR_MAGIC,
    MPS_ERR_VERSION,
    MPS_ERR_HEADER_SIZE,
    MPS_ERR_UNSUPPORTED_FLAGS,
    MPS_ERR_HEADER_CRC,
    MPS_ERR_RESERVED,
    MPS_ERR_INVALID_HEADER,
    MPS_ERR_INVALID_DIMENSIONS,
    MPS_ERR_TILE_COUNT,
    MPS_ERR_OVERFLOW,
    MPS_ERR_FILE_SIZE,
    MPS_ERR_INDEX_ENTRY,
    MPS_ERR_INDEX_TRUNCATED,
    MPS_ERR_DATA_TRUNCATED,
    MPS_ERR_CODEC,
    MPS_ERR_FLAGS,
    MPS_ERR_RAW_SIZE,
    MPS_ERR_STORED_SIZE,
    MPS_ERR_DATA_RANGE,
    MPS_ERR_STORED_CRC,
    MPS_ERR_DECOMPRESS,
    MPS_ERR_DECODED_CRC,
    MPS_ERR_ALLOC,
};

struct mps_header {
    uint16_t version;
    uint16_t header_size;
    uint32_t required_flags;
    uint32_t optional_flags;
    uint32_t tile_width;
    uint32_t tile_height;
    uint16_t tile_pixel_size;
    uint16_t pixel_format;
    uint16_t index_entry_size;
    uint16_t reserved;
    uint64_t tile_count;
    uint64_t index_offset;
    uint64_t data_offset;
    uint64_t file_size;
    uint32_t header_crc32;
};

struct mps_tile_ref {
    uint64_t data_offset;
    uint32_t stored_size;
    uint32_t raw_size;
    uint32_t decoded_crc32;
    uint32_t stored_crc32;
    uint16_t codec;
    uint16_t flags;
    uint32_t reserved;
};

struct mps_stats {
    uint64_t index_refills;
    size_t max_allocated_bytes;
    size_t max_read_bytes;
};

struct mps_file {
    struct mps_header header;
    FILE *fp;
    uint64_t actual_file_size;
    struct mps_tile_ref index_cache[MPS_INDEX_CACHE_ENTRIES];
    uint64_t cache_first;
    uint32_t cache_count;
    uint64_t stream_pos;
    uint8_t *stored_buffer;
    size_t stored_buffer_size;
    struct mps_stats stats;
};

#define MPS_STREAM_POS_UNKNOWN UINT64_MAX

int mps_u64_add_checked(uint64_t a, uint64_t b, uint64_t *out);
int mps_u64_mul_checked(uint64_t a, uint64_t b, uint64_t *out);

// Opens and validates the fixed MPS1 header. The index is loaded on demand.
enum mps_error mps_open(const char *path, struct mps_file *out);

// Reads one row-major tile into the first MPS_TILE_BYTES bytes of buf.
enum mps_error mps_read_tile(struct mps_file *file, uint64_t tile_index,
                             uint8_t *buf, size_t buf_size);

// Reads one tile by its row-major x/y coordinates.
enum mps_error mps_read_tile_xy(struct mps_file *file, uint32_t tile_x,
                                uint32_t tile_y, uint8_t *buf,
                                size_t buf_size);

enum mps_error mps_get_metadata(const struct mps_file *file,
                                struct mps_header *out);
enum mps_error mps_get_stats(const struct mps_file *file,
                             struct mps_stats *out);
const char *mps_error_name(enum mps_error error);
void mps_close(struct mps_file *file);

#endif // ENDSTONE_MEDIAPLAYER_IMAGE_MPS_FORMAT_H

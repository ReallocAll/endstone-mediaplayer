#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "mediaplayer/video/video_format.h"
#include "mediaplayer/screen/screen_geometry.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

#if !defined(_WIN32)
#include <sys/types.h>
_Static_assert(sizeof(off_t) >= 8, "MCV reader requires 64-bit off_t");
#endif

_Static_assert(SCREEN_TILE_SIZE == 128, "MCV pixel math assumes 128px tiles");

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

int mcv_u64_add_checked(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || a > UINT64_MAX - b) return 0;
    *out = a + b;
    return 1;
}

int mcv_u64_mul_checked(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || (a != 0 && b > UINT64_MAX / a)) return 0;
    *out = a * b;
    return 1;
}

void mcv_decode_frame_ref(
    const uint8_t encoded[MCV_FRAME_INDEX_ENTRY_SIZE],
    struct mcv_frame_ref *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!encoded) return;
    out->offset = read_u64(encoded);
    out->size = read_u64(encoded + 8);
    out->crc32 = read_u32(encoded + 16);
    out->flags = read_u16(encoded + 20);
    out->reserved = read_u16(encoded + 22);
}

static int seek_u64(FILE *fp, uint64_t position, int origin)
{
    if (!fp || position > (uint64_t)INT64_MAX) return -1;
#if defined(_WIN32)
    return _fseeki64(fp, (int64_t)position, origin);
#else
    return fseeko(fp, (off_t)position, origin);
#endif
}

static int tell_u64(FILE *fp, uint64_t *position)
{
    if (!fp || !position) return -1;
#if defined(_WIN32)
    int64_t value = _ftelli64(fp);
#else
    off_t value = ftello(fp);
#endif
    if (value < 0) return -1;
    *position = (uint64_t)value;
    return 0;
}

static int file_size_u64(FILE *fp, uint64_t *size)
{
    if (seek_u64(fp, 0, SEEK_END) != 0 || tell_u64(fp, size) != 0 ||
        seek_u64(fp, 0, SEEK_SET) != 0) {
        return -1;
    }
    return 0;
}

static enum mcv_error fail_open(struct mcv_file *file, enum mcv_error error)
{
    mcv_close(file);
    return error;
}

enum mcv_error mcv_open(const char *path, struct mcv_file *out)
{
    if (!out) return MCV_ERR_INVALID_HEADER;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return MCV_ERR_IO;

    out->fp = fopen(path, "rb");
    if (!out->fp) return MCV_ERR_IO;
    out->stream_pos = MCV_STREAM_POS_UNKNOWN;
    if (file_size_u64(out->fp, &out->file_size) != 0)
        return fail_open(out, MCV_ERR_IO);

    uint8_t header[MCV_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), out->fp) != sizeof(header))
        return fail_open(out, MCV_ERR_HEADER_TRUNCATED);
    out->stream_pos = MCV_HEADER_SIZE;
    if (memcmp(header, MCV_MAGIC, MCV_MAGIC_LEN) != 0)
        return fail_open(out, MCV_ERR_MAGIC);

    struct mcv_header *h = &out->header;
    h->header_size = read_u16(header + 4);
    h->format_version = read_u16(header + 6);
    h->required_flags = read_u32(header + 8);
    h->optional_flags = read_u32(header + 12);
    h->tile_width = read_u16(header + 16);
    h->tile_height = read_u16(header + 18);
    h->pixel_width = read_u16(header + 20);
    h->pixel_height = read_u16(header + 22);
    h->pixel_format = read_u16(header + 24);
    h->codec = read_u16(header + 26);
    h->fps_num = read_u16(header + 28);
    h->fps_den = read_u16(header + 30);
    h->frame_count = read_u64(header + 32);
    h->frame_data_offset = read_u64(header + 40);
    h->frame_data_size = read_u64(header + 48);
    h->frame_index_offset = read_u64(header + 56);
    h->frame_index_entry_size = read_u32(header + 64);
    h->header_crc32 = read_u32(header + 124);

    uint32_t computed_crc =
        (uint32_t)mz_crc32(MZ_CRC32_INIT, header, MCV_HEADER_SIZE - 4);
    if (computed_crc != h->header_crc32)
        return fail_open(out, MCV_ERR_HEADER_CRC);

    if (h->header_size != MCV_HEADER_SIZE)
        return fail_open(out, MCV_ERR_INVALID_HEADER);
    if (h->format_version != MCV_FORMAT_VERSION)
        return fail_open(out, MCV_ERR_VERSION);
    // MCV v1 defines no required feature bits.
    if (h->required_flags != 0)
        return fail_open(out, MCV_ERR_UNSUPPORTED_FEATURE);
    // optional_flags may carry unknown compatible bits: ignore them.

    for (size_t i = 68; i < MCV_HEADER_SIZE - 4; i++) {
        if (header[i] != 0)
            return fail_open(out, MCV_ERR_INVALID_HEADER);
    }

    if (h->tile_width < 1 || h->tile_width > SCREEN_MAX_WIDTH ||
        h->tile_height < 1 || h->tile_height > SCREEN_MAX_HEIGHT ||
        h->pixel_width != (uint16_t)(h->tile_width * SCREEN_TILE_SIZE) ||
        h->pixel_height != (uint16_t)(h->tile_height * SCREEN_TILE_SIZE)) {
        return fail_open(out, MCV_ERR_INVALID_DIMENSIONS);
    }
    if (h->pixel_format != MCV_PIXFMT_ABGR8888)
        return fail_open(out, MCV_ERR_INVALID_DIMENSIONS);
    if (h->fps_num == 0 || h->fps_den == 0 ||
        (uint32_t)h->fps_num > 20u * (uint32_t)h->fps_den)
        return fail_open(out, MCV_ERR_INVALID_DIMENSIONS);
    if (h->codec != MCV_CODEC_NONE && h->codec != MCV_CODEC_ZLIB)
        return fail_open(out, MCV_ERR_INVALID_CODEC);
    if (h->frame_count == 0 || h->frame_count > MCV_MAX_FRAME_COUNT)
        return fail_open(out, MCV_ERR_FRAME_COUNT_OVERFLOW);

    uint64_t raw_pixels = 0;
    if (!mcv_u64_mul_checked(h->pixel_width, h->pixel_height, &raw_pixels) ||
        !mcv_u64_mul_checked(raw_pixels, 4, &out->raw_frame_size) ||
        out->raw_frame_size == 0 ||
        out->raw_frame_size > MCV_MAX_FRAME_SIZE ||
        out->raw_frame_size > SIZE_MAX) {
        return fail_open(out, MCV_ERR_OVERFLOW);
    }

    if (h->frame_data_offset != MCV_HEADER_SIZE ||
        h->frame_index_entry_size != MCV_FRAME_INDEX_ENTRY_SIZE)
        return fail_open(out, MCV_ERR_INVALID_HEADER);

    // Validate the complete frame-data region.
    uint64_t exact_uncompressed = 0;
    if (!mcv_u64_mul_checked(h->frame_count, out->raw_frame_size,
                             &exact_uncompressed))
        return fail_open(out, MCV_ERR_OVERFLOW);
    if (h->codec == MCV_CODEC_NONE) {
        if (h->frame_data_size != exact_uncompressed)
            return fail_open(out, MCV_ERR_FRAME_SIZE_MISMATCH);
    } else {
        uint64_t max_stored_frame =
            out->raw_frame_size + MCV_MAX_STORED_OVERHEAD;
        uint64_t max_data = 0;
        if (!mcv_u64_mul_checked(h->frame_count, max_stored_frame, &max_data))
            return fail_open(out, MCV_ERR_OVERFLOW);
        if (h->frame_data_size < h->frame_count ||
            h->frame_data_size > max_data)
            return fail_open(out, MCV_ERR_FRAME_SIZE_MISMATCH);
    }

    uint64_t expected_index_offset = 0;
    if (!mcv_u64_add_checked(h->frame_data_offset, h->frame_data_size,
                             &expected_index_offset))
        return fail_open(out, MCV_ERR_OVERFLOW);
    if (h->frame_index_offset != expected_index_offset)
        return fail_open(out, MCV_ERR_INVALID_HEADER);

    uint64_t index_size = 0;
    uint64_t index_end = 0;
    if (!mcv_u64_mul_checked(h->frame_count, h->frame_index_entry_size,
                             &index_size) ||
        !mcv_u64_add_checked(h->frame_index_offset, index_size, &index_end))
        return fail_open(out, MCV_ERR_OVERFLOW);
    if (out->file_size < h->frame_index_offset)
        return fail_open(out, MCV_ERR_DATA_TRUNCATED);
    // The frame index must end exactly at EOF.
    if (index_end != out->file_size)
        return fail_open(out, MCV_ERR_INDEX_TRUNCATED);

    out->cache_first = 0;
    out->cache_count = 0;
    return MCV_OK;
}

// Seeks only when the stream is not already at the requested position.
static enum mcv_error stream_seek(struct mcv_file *f, uint64_t position)
{
    if (f->stream_pos == position)
        return MCV_OK;
    if (seek_u64(f->fp, position, SEEK_SET) != 0) {
        f->stream_pos = MCV_STREAM_POS_UNKNOWN;
        return MCV_ERR_IO;
    }
    f->stream_pos = position;
    return MCV_OK;
}

// Reads bytes and keeps stream_pos synchronized.
static int stream_read(struct mcv_file *f, void *buf, size_t size)
{
    if (fread(buf, 1, size, f->fp) != size) {
        f->stream_pos = MCV_STREAM_POS_UNKNOWN;
        return 0;
    }
    if (f->stream_pos != MCV_STREAM_POS_UNKNOWN)
        f->stream_pos += size;
    return 1;
}

// Loads an index entry through the bounded cache window.
static enum mcv_error load_frame_ref(struct mcv_file *f, uint64_t frame_idx,
                                     const struct mcv_frame_ref **out_ref)
{
    if (frame_idx < f->cache_first ||
        frame_idx >= f->cache_first + f->cache_count) {
        uint64_t remaining = f->header.frame_count - frame_idx;
        uint32_t count = MCV_INDEX_CACHE_ENTRIES;
        if (remaining < count) count = (uint32_t)remaining;

        uint64_t entry_pos = 0;
        uint64_t entry_off = 0;
        if (!mcv_u64_mul_checked(frame_idx, MCV_FRAME_INDEX_ENTRY_SIZE,
                                 &entry_off) ||
            !mcv_u64_add_checked(f->header.frame_index_offset, entry_off,
                                 &entry_pos))
            return MCV_ERR_OVERFLOW;
        enum mcv_error seek_error = stream_seek(f, entry_pos);
        if (seek_error != MCV_OK)
            return seek_error;

        uint8_t encoded[MCV_INDEX_CACHE_ENTRIES * MCV_FRAME_INDEX_ENTRY_SIZE];
        size_t want = (size_t)count * MCV_FRAME_INDEX_ENTRY_SIZE;
        if (!stream_read(f, encoded, want))
            return MCV_ERR_INDEX_TRUNCATED;
        for (uint32_t i = 0; i < count; i++) {
            mcv_decode_frame_ref(encoded + (size_t)i *
                                     MCV_FRAME_INDEX_ENTRY_SIZE,
                                 &f->cache[i]);
        }
        f->cache_first = frame_idx;
        f->cache_count = count;
    }
    *out_ref = &f->cache[frame_idx - f->cache_first];
    return MCV_OK;
}

enum mcv_error mcv_read_frame(struct mcv_file *f, uint32_t frame_idx,
                              uint8_t *buf, size_t buf_size)
{
    if (!f || !f->fp || !buf || frame_idx >= f->header.frame_count)
        return MCV_ERR_DATA_TRUNCATED;
    if (f->raw_frame_size == 0 || f->raw_frame_size > MCV_MAX_FRAME_SIZE ||
        f->raw_frame_size > SIZE_MAX || buf_size < (size_t)f->raw_frame_size)
        return MCV_ERR_FRAME_SIZE_MISMATCH;

    const struct mcv_frame_ref *ref = nullptr;
    enum mcv_error err = load_frame_ref(f, frame_idx, &ref);
    if (err != MCV_OK)
        return err;

    // MCV v1 frames are independently decodable.
    if (ref->flags != MCV_FRAME_FLAG_INDEPENDENT || ref->reserved != 0)
        return MCV_ERR_FRAME_FLAGS;

    uint64_t relative_end = 0;
    uint64_t absolute_position = 0;
    uint64_t absolute_end = 0;
    if (!mcv_u64_add_checked(ref->offset, ref->size, &relative_end) ||
        !mcv_u64_add_checked(f->header.frame_data_offset, ref->offset,
                             &absolute_position) ||
        !mcv_u64_add_checked(absolute_position, ref->size, &absolute_end))
        return MCV_ERR_OVERFLOW;
    if (relative_end > f->header.frame_data_size ||
        absolute_end > f->header.frame_index_offset ||
        absolute_position > INT64_MAX)
        return MCV_ERR_DATA_TRUNCATED;
    if (ref->size == 0)
        return MCV_ERR_FRAME_SIZE_MISMATCH;

    if (f->header.codec == MCV_CODEC_NONE) {
        // Enforce the fixed uncompressed layout.
        if (ref->size != f->raw_frame_size ||
            ref->offset != (uint64_t)frame_idx * f->raw_frame_size)
            return MCV_ERR_FRAME_SIZE_MISMATCH;
        enum mcv_error seek_error = stream_seek(f, absolute_position);
        if (seek_error != MCV_OK)
            return seek_error;
        size_t size = (size_t)f->raw_frame_size;
        if (!stream_read(f, buf, size))
            return MCV_ERR_DATA_TRUNCATED;
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, buf, size);
        if (crc != ref->crc32)
            return MCV_ERR_FRAME_CRC;
        return MCV_OK;
    }

    if (ref->size > f->raw_frame_size + MCV_MAX_STORED_OVERHEAD)
        return MCV_ERR_FRAME_SIZE_MISMATCH;
    enum mcv_error seek_error = stream_seek(f, absolute_position);
    if (seek_error != MCV_OK)
        return seek_error;

    size_t stored_size = (size_t)ref->size;
    if (f->stored_scratch_size < stored_size) {
        uint8_t *grown = realloc(f->stored_scratch, stored_size);
        if (!grown) return MCV_ERR_ALLOC;
        f->stored_scratch = grown;
        f->stored_scratch_size = stored_size;
    }
    uint8_t *stored = f->stored_scratch;
    if (!stream_read(f, stored, stored_size))
        return MCV_ERR_DATA_TRUNCATED;

    // mz_uncompress validates the zlib stream checksum.
    mz_ulong destination_size = (mz_ulong)f->raw_frame_size;
    int zlib_result = mz_uncompress(buf, &destination_size, stored,
                                    (mz_ulong)stored_size);
    if (zlib_result != MZ_OK || destination_size != f->raw_frame_size)
        return MCV_ERR_DECOMPRESS;
    return MCV_OK;
}

double mcv_frame_duration_ms(const struct mcv_file *f)
{
    if (!f || f->header.fps_num == 0) return 50.0;
    return (double)f->header.fps_den / (double)f->header.fps_num * 1000.0;
}

double mcv_total_duration_ms(const struct mcv_file *f)
{
    if (!f) return 0.0;
    return mcv_frame_duration_ms(f) * (double)f->header.frame_count;
}

void mcv_close(struct mcv_file *f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f->stored_scratch);
    memset(f, 0, sizeof(*f));
}

int mcv_dimensions_match(const struct mcv_file *f, int tile_w, int tile_h)
{
    return f && f->header.tile_width == (uint16_t)tile_w &&
           f->header.tile_height == (uint16_t)tile_h;
}

const char *mcv_error_name(enum mcv_error error)
{
    switch (error) {
    case MCV_OK: return "ok";
    case MCV_ERR_IO: return "I/O error";
    case MCV_ERR_MAGIC: return "invalid MCV magic";
    case MCV_ERR_VERSION: return "unsupported MCV version";
    case MCV_ERR_HEADER_TRUNCATED: return "truncated MCV header";
    case MCV_ERR_HEADER_CRC: return "MCV header CRC mismatch";
    case MCV_ERR_UNSUPPORTED_FEATURE:
        return "MCV file requires an unsupported feature";
    case MCV_ERR_INVALID_DIMENSIONS: return "invalid MCV dimensions or FPS";
    case MCV_ERR_FRAME_COUNT_OVERFLOW: return "MCV frame-count limit exceeded";
    case MCV_ERR_INDEX_TRUNCATED: return "truncated MCV frame index";
    case MCV_ERR_DATA_TRUNCATED: return "truncated or invalid MCV frame data";
    case MCV_ERR_FRAME_SIZE_MISMATCH: return "MCV frame size mismatch";
    case MCV_ERR_FRAME_CRC: return "MCV frame CRC mismatch";
    case MCV_ERR_FRAME_FLAGS: return "invalid MCV frame flags";
    case MCV_ERR_DECOMPRESS: return "MCV frame decompression failed";
    case MCV_ERR_ALLOC: return "MCV allocation failed";
    case MCV_ERR_INVALID_HEADER: return "invalid MCV v1 layout";
    case MCV_ERR_INVALID_CODEC: return "unknown MCV codec";
    case MCV_ERR_OVERFLOW: return "MCV offset or size overflow";
    }
    return "unknown MCV error";
}

#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "mediaplayer/image/mps_format.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <sys/types.h>
static_assert(sizeof(off_t) >= 8, "MPS reader requires 64-bit off_t");
#endif

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

int mps_u64_add_checked(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || a > UINT64_MAX - b)
        return 0;
    *out = a + b;
    return 1;
}

int mps_u64_mul_checked(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || (a != 0 && b > UINT64_MAX / a))
        return 0;
    *out = a * b;
    return 1;
}

static int seek_u64(FILE *fp, uint64_t position, int origin)
{
    if (!fp || position > (uint64_t)INT64_MAX)
        return -1;
#if defined(_WIN32)
    return _fseeki64(fp, (int64_t)position, origin);
#else
    return fseeko(fp, (off_t)position, origin);
#endif
}

static int tell_u64(FILE *fp, uint64_t *position)
{
    if (!fp || !position)
        return -1;
#if defined(_WIN32)
    int64_t value = _ftelli64(fp);
#else
    off_t value = ftello(fp);
#endif
    if (value < 0)
        return -1;
    *position = (uint64_t)value;
    return 0;
}

static int file_size_u64(FILE *fp, uint64_t *size)
{
    if (seek_u64(fp, 0, SEEK_END) != 0 || tell_u64(fp, size) != 0 ||
        seek_u64(fp, 0, SEEK_SET) != 0)
        return -1;
    return 0;
}

static enum mps_error fail_open(struct mps_file *file, enum mps_error error)
{
    mps_close(file);
    return error;
}

static FILE *open_read_utf8(const char *path)
{
#if defined(_WIN32)
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                    nullptr, 0);
    if (count <= 0)
        return nullptr;
    wchar_t *wide = calloc((size_t)count, sizeof(*wide));
    if (!wide)
        return nullptr;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            count) != count) {
        free(wide);
        return nullptr;
    }
    FILE *fp = _wfopen(wide, L"rb");
    free(wide);
    return fp;
#else
    return fopen(path, "rb");
#endif
}

static int stream_seek(struct mps_file *file, uint64_t position)
{
    if (file->stream_pos == position)
        return 0;
    if (seek_u64(file->fp, position, SEEK_SET) != 0) {
        file->stream_pos = MPS_STREAM_POS_UNKNOWN;
        return -1;
    }
    file->stream_pos = position;
    return 0;
}

static int stream_read(struct mps_file *file, void *buffer, size_t size)
{
    if (size > file->stats.max_read_bytes)
        file->stats.max_read_bytes = size;
    if (fread(buffer, 1, size, file->fp) != size) {
        file->stream_pos = MPS_STREAM_POS_UNKNOWN;
        return -1;
    }
    if (file->stream_pos != MPS_STREAM_POS_UNKNOWN)
        file->stream_pos += size;
    return 0;
}

static uint32_t zero_tile_crc(void)
{
    uint8_t zeros[256] = {0};
    mz_ulong crc = MZ_CRC32_INIT;
    size_t remaining = MPS_TILE_BYTES;
    while (remaining != 0) {
        size_t count = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
        crc = mz_crc32(crc, zeros, count);
        remaining -= count;
    }
    return (uint32_t)crc;
}

static void decode_index_entry(const uint8_t encoded[MPS_INDEX_ENTRY_SIZE],
                               struct mps_tile_ref *out)
{
    out->data_offset = read_u64(encoded);
    out->stored_size = read_u32(encoded + 8);
    out->raw_size = read_u32(encoded + 12);
    out->decoded_crc32 = read_u32(encoded + 16);
    out->stored_crc32 = read_u32(encoded + 20);
    out->codec = read_u16(encoded + 24);
    out->flags = read_u16(encoded + 26);
    out->reserved = read_u32(encoded + 28);
}

static enum mps_error load_index_entry(struct mps_file *file,
                                       uint64_t tile_index,
                                       const struct mps_tile_ref **out)
{
    if (tile_index < file->cache_first ||
        tile_index >= file->cache_first + file->cache_count) {
        uint64_t remaining = file->header.tile_count - tile_index;
        uint32_t count = MPS_INDEX_CACHE_ENTRIES;
        if (remaining < count)
            count = (uint32_t)remaining;

        uint64_t relative_offset = 0;
        uint64_t entry_position = 0;
        if (!mps_u64_mul_checked(tile_index, MPS_INDEX_ENTRY_SIZE,
                                 &relative_offset) ||
            !mps_u64_add_checked(file->header.index_offset, relative_offset,
                                 &entry_position))
            return MPS_ERR_OVERFLOW;
        if (stream_seek(file, entry_position) != 0)
            return MPS_ERR_IO;

        uint8_t encoded[MPS_INDEX_CACHE_ENTRIES * MPS_INDEX_ENTRY_SIZE];
        size_t read_size = (size_t)count * MPS_INDEX_ENTRY_SIZE;
        if (stream_read(file, encoded, read_size) != 0)
            return MPS_ERR_INDEX_TRUNCATED;
        for (uint32_t i = 0; i < count; i++) {
            decode_index_entry(encoded + (size_t)i * MPS_INDEX_ENTRY_SIZE,
                               &file->index_cache[i]);
        }
        file->cache_first = tile_index;
        file->cache_count = count;
        file->stats.index_refills++;
    }
    *out = &file->index_cache[tile_index - file->cache_first];
    return MPS_OK;
}

static enum mps_error validate_tile_ref(const struct mps_file *file,
                                        const struct mps_tile_ref *ref,
                                        uint64_t *data_end)
{
    if (ref->flags != 0)
        return MPS_ERR_FLAGS;
    if (ref->reserved != 0)
        return MPS_ERR_RESERVED;
    if (ref->raw_size != MPS_TILE_BYTES)
        return MPS_ERR_RAW_SIZE;

    uint64_t max_zlib_size = (uint64_t)mz_compressBound(MPS_TILE_BYTES);
    switch (ref->codec) {
    case MPS_CODEC_RAW:
        if (ref->stored_size != MPS_TILE_BYTES)
            return MPS_ERR_STORED_SIZE;
        break;
    case MPS_CODEC_ZLIB:
        if (ref->stored_size == 0 ||
            (uint64_t)ref->stored_size > max_zlib_size)
            return MPS_ERR_STORED_SIZE;
        break;
    case MPS_CODEC_ZERO:
        if (ref->data_offset != 0 || ref->stored_size != 0 ||
            ref->stored_crc32 != 0 ||
            ref->decoded_crc32 != zero_tile_crc())
            return MPS_ERR_INDEX_ENTRY;
        if (data_end)
            *data_end = 0;
        return MPS_OK;
    default:
        return MPS_ERR_CODEC;
    }

    if (ref->data_offset < file->header.data_offset)
        return MPS_ERR_DATA_RANGE;
    if (!mps_u64_add_checked(ref->data_offset, ref->stored_size,
                             data_end))
        return MPS_ERR_OVERFLOW;
    if (*data_end > file->header.file_size)
        return MPS_ERR_DATA_RANGE;
    return MPS_OK;
}

static enum mps_error read_at(struct mps_file *file, uint64_t position,
                              void *buffer, size_t size)
{
    if (stream_seek(file, position) != 0)
        return MPS_ERR_IO;
    if (stream_read(file, buffer, size) != 0)
        return MPS_ERR_DATA_TRUNCATED;
    return MPS_OK;
}

enum mps_error mps_open(const char *path, struct mps_file *out)
{
    if (!out)
        return MPS_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->stream_pos = MPS_STREAM_POS_UNKNOWN;
    if (!path || !path[0])
        return MPS_ERR_INVALID_ARGUMENT;

    out->fp = open_read_utf8(path);
    if (!out->fp)
        return MPS_ERR_IO;
    if (file_size_u64(out->fp, &out->actual_file_size) != 0)
        return fail_open(out, MPS_ERR_IO);

    uint8_t encoded[MPS_HEADER_SIZE];
    if (stream_read(out, encoded, sizeof(encoded)) != 0)
        return fail_open(out, MPS_ERR_HEADER_TRUNCATED);
    if (memcmp(encoded, MPS_MAGIC, MPS_MAGIC_LEN) != 0)
        return fail_open(out, MPS_ERR_MAGIC);

    struct mps_header *header = &out->header;
    header->version = read_u16(encoded + 4);
    header->header_size = read_u16(encoded + 6);
    header->required_flags = read_u32(encoded + 8);
    header->optional_flags = read_u32(encoded + 12);
    header->tile_width = read_u32(encoded + 16);
    header->tile_height = read_u32(encoded + 20);
    header->tile_pixel_size = read_u16(encoded + 24);
    header->pixel_format = read_u16(encoded + 26);
    header->index_entry_size = read_u16(encoded + 28);
    header->reserved = read_u16(encoded + 30);
    header->tile_count = read_u64(encoded + 32);
    header->index_offset = read_u64(encoded + 40);
    header->data_offset = read_u64(encoded + 48);
    header->file_size = read_u64(encoded + 56);
    header->header_crc32 = read_u32(encoded + 124);

    uint32_t computed_crc =
        (uint32_t)mz_crc32(MZ_CRC32_INIT, encoded, MPS_HEADER_SIZE - 4);
    if (computed_crc != header->header_crc32)
        return fail_open(out, MPS_ERR_HEADER_CRC);
    if (header->version != MPS_FORMAT_VERSION)
        return fail_open(out, MPS_ERR_VERSION);
    if (header->header_size != MPS_HEADER_SIZE)
        return fail_open(out, MPS_ERR_HEADER_SIZE);
    if (header->required_flags != 0 || header->optional_flags != 0)
        return fail_open(out, MPS_ERR_UNSUPPORTED_FLAGS);
    if (header->tile_pixel_size != MPS_TILE_PIXEL_SIZE ||
        header->pixel_format != MPS_PIXFMT_ABGR8888 ||
        header->index_entry_size != MPS_INDEX_ENTRY_SIZE ||
        header->reserved != 0)
        return fail_open(out, MPS_ERR_INVALID_HEADER);
    for (size_t i = 64; i < MPS_HEADER_SIZE - 4; i++) {
        if (encoded[i] != 0)
            return fail_open(out, MPS_ERR_RESERVED);
    }
    if (header->tile_width < 1 ||
        header->tile_width > MPS_MAX_TILE_DIMENSION ||
        header->tile_height < 1 ||
        header->tile_height > MPS_MAX_TILE_DIMENSION)
        return fail_open(out, MPS_ERR_INVALID_DIMENSIONS);

    uint64_t expected_tile_count = 0;
    uint64_t index_bytes = 0;
    uint64_t expected_data_offset = 0;
    if (!mps_u64_mul_checked(header->tile_count, MPS_INDEX_ENTRY_SIZE,
                             &index_bytes) ||
        !mps_u64_add_checked(MPS_HEADER_SIZE, index_bytes,
                             &expected_data_offset) ||
        !mps_u64_mul_checked(header->tile_width, header->tile_height,
                             &expected_tile_count))
        return fail_open(out, MPS_ERR_OVERFLOW);
    if (header->tile_count != expected_tile_count)
        return fail_open(out, MPS_ERR_TILE_COUNT);
    if (header->index_offset != MPS_HEADER_SIZE ||
        header->data_offset != expected_data_offset)
        return fail_open(out, MPS_ERR_INVALID_HEADER);
    if (header->file_size != out->actual_file_size ||
        header->file_size < header->data_offset)
        return fail_open(out, MPS_ERR_FILE_SIZE);

    out->cache_first = 0;
    out->cache_count = 0;
    return MPS_OK;
}

enum mps_error mps_read_tile(struct mps_file *file, uint64_t tile_index,
                             uint8_t *buf, size_t buf_size)
{
    if (!file || !file->fp || !buf)
        return MPS_ERR_INVALID_ARGUMENT;
    if (tile_index >= file->header.tile_count)
        return MPS_ERR_INVALID_ARGUMENT;
    if (buf_size < MPS_TILE_BYTES)
        return MPS_ERR_INVALID_ARGUMENT;

    const struct mps_tile_ref *ref = nullptr;
    enum mps_error error = load_index_entry(file, tile_index, &ref);
    if (error != MPS_OK)
        return error;

    uint64_t data_end = 0;
    error = validate_tile_ref(file, ref, &data_end);
    if (error != MPS_OK)
        return error;

    if (ref->codec == MPS_CODEC_ZERO) {
        memset(buf, 0, MPS_TILE_BYTES);
        return MPS_OK;
    }

    if (ref->codec == MPS_CODEC_RAW) {
        error = read_at(file, ref->data_offset, buf, MPS_TILE_BYTES);
        if (error != MPS_OK)
            return error;
        uint32_t stored_crc =
            (uint32_t)mz_crc32(MZ_CRC32_INIT, buf, MPS_TILE_BYTES);
        if (stored_crc != ref->stored_crc32)
            return MPS_ERR_STORED_CRC;
        uint32_t decoded_crc =
            (uint32_t)mz_crc32(MZ_CRC32_INIT, buf, MPS_TILE_BYTES);
        if (decoded_crc != ref->decoded_crc32)
            return MPS_ERR_DECODED_CRC;
        return MPS_OK;
    }

    size_t stored_size = (size_t)ref->stored_size;
    if (file->stored_buffer_size < stored_size) {
        uint8_t *grown = realloc(file->stored_buffer, stored_size);
        if (!grown)
            return MPS_ERR_ALLOC;
        file->stored_buffer = grown;
        file->stored_buffer_size = stored_size;
        if (stored_size > file->stats.max_allocated_bytes)
            file->stats.max_allocated_bytes = stored_size;
    }
    error = read_at(file, ref->data_offset, file->stored_buffer, stored_size);
    if (error != MPS_OK)
        return error;
    uint32_t stored_crc = (uint32_t)mz_crc32(
        MZ_CRC32_INIT, file->stored_buffer, stored_size);
    if (stored_crc != ref->stored_crc32)
        return MPS_ERR_STORED_CRC;

    mz_ulong decoded_size = MPS_TILE_BYTES;
    int result = mz_uncompress(buf, &decoded_size, file->stored_buffer,
                               (mz_ulong)stored_size);
    if (result != MZ_OK || decoded_size != MPS_TILE_BYTES)
        return MPS_ERR_DECOMPRESS;
    uint32_t decoded_crc =
        (uint32_t)mz_crc32(MZ_CRC32_INIT, buf, MPS_TILE_BYTES);
    if (decoded_crc != ref->decoded_crc32)
        return MPS_ERR_DECODED_CRC;
    (void)data_end;
    return MPS_OK;
}

enum mps_error mps_read_tile_xy(struct mps_file *file, uint32_t tile_x,
                                uint32_t tile_y, uint8_t *buf,
                                size_t buf_size)
{
    if (!file || tile_x >= file->header.tile_width ||
        tile_y >= file->header.tile_height)
        return MPS_ERR_INVALID_ARGUMENT;
    uint64_t row_offset = 0;
    uint64_t tile_index = 0;
    if (!mps_u64_mul_checked(tile_y, file->header.tile_width, &row_offset) ||
        !mps_u64_add_checked(row_offset, tile_x, &tile_index))
        return MPS_ERR_OVERFLOW;
    return mps_read_tile(file, tile_index, buf, buf_size);
}

enum mps_error mps_get_metadata(const struct mps_file *file,
                                struct mps_header *out)
{
    if (!file || !file->fp || !out)
        return MPS_ERR_INVALID_ARGUMENT;
    *out = file->header;
    return MPS_OK;
}

enum mps_error mps_get_stats(const struct mps_file *file,
                             struct mps_stats *out)
{
    if (!file || !out)
        return MPS_ERR_INVALID_ARGUMENT;
    *out = file->stats;
    return MPS_OK;
}

void mps_close(struct mps_file *file)
{
    if (!file)
        return;
    if (file->fp)
        fclose(file->fp);
    free(file->stored_buffer);
    memset(file, 0, sizeof(*file));
}

const char *mps_error_name(enum mps_error error)
{
    switch (error) {
    case MPS_OK: return "ok";
    case MPS_ERR_INVALID_ARGUMENT: return "invalid argument";
    case MPS_ERR_IO: return "I/O error";
    case MPS_ERR_HEADER_TRUNCATED: return "truncated MPS header";
    case MPS_ERR_MAGIC: return "invalid MPS magic";
    case MPS_ERR_VERSION: return "unsupported MPS version";
    case MPS_ERR_HEADER_SIZE: return "invalid MPS header size";
    case MPS_ERR_UNSUPPORTED_FLAGS: return "unsupported MPS flags";
    case MPS_ERR_HEADER_CRC: return "MPS header CRC mismatch";
    case MPS_ERR_RESERVED: return "non-zero MPS reserved field";
    case MPS_ERR_INVALID_HEADER: return "invalid MPS v1 layout";
    case MPS_ERR_INVALID_DIMENSIONS: return "invalid MPS dimensions";
    case MPS_ERR_TILE_COUNT: return "invalid MPS tile count";
    case MPS_ERR_OVERFLOW: return "MPS offset or size overflow";
    case MPS_ERR_FILE_SIZE: return "invalid MPS file size";
    case MPS_ERR_INDEX_ENTRY: return "invalid MPS index entry";
    case MPS_ERR_INDEX_TRUNCATED: return "truncated MPS index";
    case MPS_ERR_DATA_TRUNCATED: return "truncated MPS tile data";
    case MPS_ERR_CODEC: return "unknown MPS codec";
    case MPS_ERR_FLAGS: return "invalid MPS tile flags";
    case MPS_ERR_RAW_SIZE: return "invalid MPS raw tile size";
    case MPS_ERR_STORED_SIZE: return "invalid MPS stored tile size";
    case MPS_ERR_DATA_RANGE: return "MPS tile data is out of range";
    case MPS_ERR_STORED_CRC: return "MPS stored tile CRC mismatch";
    case MPS_ERR_DECOMPRESS: return "MPS tile decompression failed";
    case MPS_ERR_DECODED_CRC: return "MPS decoded tile CRC mismatch";
    case MPS_ERR_ALLOC: return "MPS allocation failed";
    }
    return "unknown MPS error";
}

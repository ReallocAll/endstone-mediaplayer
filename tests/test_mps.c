#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "mediaplayer/image/mps_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "miniz.h"

static int g_failures;
static unsigned g_path_counter;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        g_failures++; \
    } \
} while (0)

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put_u64(uint8_t *p, uint64_t value)
{
    put_u32(p, (uint32_t)value);
    put_u32(p + 4, (uint32_t)(value >> 32));
}

static int seek_u64_test(FILE *fp, uint64_t position)
{
#if defined(_WIN32)
    return _fseeki64(fp, (int64_t)position, SEEK_SET);
#else
    return fseeko(fp, (off_t)position, SEEK_SET);
#endif
}

static void make_path(char path[128], const char *tag)
{
#if defined(_WIN32)
    int process_id = _getpid();
#else
    int process_id = (int)getpid();
#endif
    snprintf(path, 128, "mps_test_%s_%d_%u.mps", tag, process_id,
             g_path_counter++);
}

static void make_header(uint8_t header[MPS_HEADER_SIZE], uint32_t tile_width,
                        uint32_t tile_height, uint64_t tile_count,
                        uint64_t file_size)
{
    uint64_t index_bytes = tile_count * MPS_INDEX_ENTRY_SIZE;
    uint64_t data_offset = MPS_HEADER_SIZE + index_bytes;
    memset(header, 0, MPS_HEADER_SIZE);
    memcpy(header, MPS_MAGIC, MPS_MAGIC_LEN);
    put_u16(header + 4, MPS_FORMAT_VERSION);
    put_u16(header + 6, MPS_HEADER_SIZE);
    put_u32(header + 8, 0);
    put_u32(header + 12, 0);
    put_u32(header + 16, tile_width);
    put_u32(header + 20, tile_height);
    put_u16(header + 24, MPS_TILE_PIXEL_SIZE);
    put_u16(header + 26, MPS_PIXFMT_ABGR8888);
    put_u16(header + 28, MPS_INDEX_ENTRY_SIZE);
    put_u16(header + 30, 0);
    put_u64(header + 32, tile_count);
    put_u64(header + 40, MPS_HEADER_SIZE);
    put_u64(header + 48, data_offset);
    put_u64(header + 56, file_size);
    put_u32(header + 124,
            (uint32_t)mz_crc32(MZ_CRC32_INIT, header, MPS_HEADER_SIZE - 4));
}

static void make_entry(uint8_t entry[MPS_INDEX_ENTRY_SIZE],
                       uint64_t data_offset, uint32_t stored_size,
                       uint32_t raw_size, uint32_t decoded_crc,
                       uint32_t stored_crc, uint16_t codec)
{
    memset(entry, 0, MPS_INDEX_ENTRY_SIZE);
    put_u64(entry, data_offset);
    put_u32(entry + 8, stored_size);
    put_u32(entry + 12, raw_size);
    put_u32(entry + 16, decoded_crc);
    put_u32(entry + 20, stored_crc);
    put_u16(entry + 24, codec);
}

static int write_demo(const char *path)
{
    uint8_t *raw0 = calloc(1, MPS_TILE_BYTES);
    uint8_t *raw1 = calloc(1, MPS_TILE_BYTES);
    uint8_t *raw3 = calloc(1, MPS_TILE_BYTES);
    size_t bound = (size_t)mz_compressBound(MPS_TILE_BYTES);
    uint8_t *compressed = malloc(bound);
    if (!raw0 || !raw1 || !raw3 || !compressed) {
        free(raw0);
        free(raw1);
        free(raw3);
        free(compressed);
        return 0;
    }
    for (size_t i = 0; i < MPS_TILE_BYTES; i++) {
        raw0[i] = (uint8_t)(i * 13u + 3u);
        raw1[i] = (uint8_t)(i & 7u);
        raw3[i] = (uint8_t)(255u - (i & 255u));
    }
    mz_ulong compressed_size = (mz_ulong)bound;
    if (mz_compress(compressed, &compressed_size, raw1, MPS_TILE_BYTES) !=
            MZ_OK) {
        free(raw0);
        free(raw1);
        free(raw3);
        free(compressed);
        return 0;
    }

    uint64_t data_offset = MPS_HEADER_SIZE + 4u * MPS_INDEX_ENTRY_SIZE;
    uint64_t tile3_offset = data_offset + MPS_TILE_BYTES + compressed_size;
    uint64_t file_size = tile3_offset + MPS_TILE_BYTES;
    uint8_t header[MPS_HEADER_SIZE];
    make_header(header, 2, 2, 4, file_size);
    uint8_t entries[4][MPS_INDEX_ENTRY_SIZE];
    make_entry(entries[0], data_offset, MPS_TILE_BYTES, MPS_TILE_BYTES,
               (uint32_t)mz_crc32(MZ_CRC32_INIT, raw0, MPS_TILE_BYTES),
               (uint32_t)mz_crc32(MZ_CRC32_INIT, raw0, MPS_TILE_BYTES),
               MPS_CODEC_RAW);
    make_entry(entries[1], data_offset + MPS_TILE_BYTES,
               (uint32_t)compressed_size, MPS_TILE_BYTES,
               (uint32_t)mz_crc32(MZ_CRC32_INIT, raw1, MPS_TILE_BYTES),
               (uint32_t)mz_crc32(MZ_CRC32_INIT, compressed, compressed_size),
               MPS_CODEC_ZLIB);
    uint8_t zeros[256] = {0};
    mz_ulong zero_crc = MZ_CRC32_INIT;
    for (size_t left = MPS_TILE_BYTES; left != 0; ) {
        size_t count = left < sizeof(zeros) ? left : sizeof(zeros);
        zero_crc = mz_crc32(zero_crc, zeros, count);
        left -= count;
    }
    make_entry(entries[2], 0, 0, MPS_TILE_BYTES, (uint32_t)zero_crc, 0,
               MPS_CODEC_ZERO);
    make_entry(entries[3], tile3_offset, MPS_TILE_BYTES, MPS_TILE_BYTES,
               (uint32_t)mz_crc32(MZ_CRC32_INIT, raw3, MPS_TILE_BYTES),
               (uint32_t)mz_crc32(MZ_CRC32_INIT, raw3, MPS_TILE_BYTES),
               MPS_CODEC_RAW);

    FILE *fp = fopen(path, "wb");
    int ok = fp != nullptr;
    if (ok && fwrite(header, 1, sizeof(header), fp) != sizeof(header)) ok = 0;
    for (size_t i = 0; ok && i < 4; i++) {
        if (fwrite(entries[i], 1, MPS_INDEX_ENTRY_SIZE, fp) !=
                MPS_INDEX_ENTRY_SIZE)
            ok = 0;
    }
    if (ok && fwrite(raw0, 1, MPS_TILE_BYTES, fp) != MPS_TILE_BYTES) ok = 0;
    if (ok && fwrite(compressed, 1, compressed_size, fp) != compressed_size)
        ok = 0;
    if (ok && fwrite(raw3, 1, MPS_TILE_BYTES, fp) != MPS_TILE_BYTES) ok = 0;
    if (fp) fclose(fp);
    free(raw0);
    free(raw1);
    free(raw3);
    free(compressed);
    return ok;
}

static int write_one_raw(const char *path)
{
    uint8_t *raw = calloc(1, MPS_TILE_BYTES);
    if (!raw) return 0;
    for (size_t i = 0; i < MPS_TILE_BYTES; i++) raw[i] = (uint8_t)i;
    const uint64_t data_offset = MPS_HEADER_SIZE + MPS_INDEX_ENTRY_SIZE;
    const uint64_t file_size = data_offset + MPS_TILE_BYTES;
    uint8_t header[MPS_HEADER_SIZE];
    uint8_t entry[MPS_INDEX_ENTRY_SIZE];
    make_header(header, 1, 1, 1, file_size);
    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, raw, MPS_TILE_BYTES);
    make_entry(entry, data_offset, MPS_TILE_BYTES, MPS_TILE_BYTES, crc, crc,
               MPS_CODEC_RAW);
    FILE *fp = fopen(path, "wb");
    int ok = fp != nullptr;
    if (ok && fwrite(header, 1, sizeof(header), fp) != sizeof(header)) ok = 0;
    if (ok && fwrite(entry, 1, sizeof(entry), fp) != sizeof(entry)) ok = 0;
    if (ok && fwrite(raw, 1, MPS_TILE_BYTES, fp) != MPS_TILE_BYTES) ok = 0;
    if (fp) fclose(fp);
    free(raw);
    return ok;
}

static int write_sparse_index(const char *path, uint32_t width,
                              uint32_t height)
{
    uint64_t count = (uint64_t)width * height;
    uint64_t data_offset = MPS_HEADER_SIZE + count * MPS_INDEX_ENTRY_SIZE;
    uint8_t header[MPS_HEADER_SIZE];
    make_header(header, width, height, count, data_offset);
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    int ok = fwrite(header, 1, sizeof(header), fp) == sizeof(header);
    if (ok && seek_u64_test(fp, data_offset - 1) == 0 && fputc(0, fp) != EOF)
        ok = 1;
    else if (ok)
        ok = 0;
    fclose(fp);
    return ok;
}

static int read_all(const char *path, uint8_t **out, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (seek_u64_test(fp, 0) != 0 || fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long end = ftell(fp);
    if (end < 0 || (uint64_t)end > SIZE_MAX ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    uint8_t *data = malloc((size_t)end);
    if (!data || fread(data, 1, (size_t)end, fp) != (size_t)end) {
        free(data);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *out = data;
    *size = (size_t)end;
    return 1;
}

static int write_all(const char *path, const uint8_t *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    int ok = fwrite(data, 1, size, fp) == size;
    fclose(fp);
    return ok;
}

enum header_mutation {
    MUT_MAGIC,
    MUT_VERSION,
    MUT_HEADER_SIZE,
    MUT_FLAGS,
    MUT_DIMS,
    MUT_COUNT,
    MUT_OFFSET,
    MUT_FILE_SIZE,
    MUT_RESERVED,
    MUT_CRC,
    MUT_ARITHMETIC,
};

static enum mps_error open_header_mutation(enum header_mutation mutation)
{
    char path[128];
    make_path(path, "header");
    EXPECT(write_one_raw(path), "write one-tile fixture");
    uint8_t *data = nullptr;
    size_t size = 0;
    EXPECT(read_all(path, &data, &size), "read one-tile fixture");
    if (!data) {
        remove(path);
        return MPS_ERR_IO;
    }
    int recalc = 1;
    switch (mutation) {
    case MUT_MAGIC: data[0] = 'X'; recalc = 0; break;
    case MUT_VERSION: put_u16(data + 4, 9); break;
    case MUT_HEADER_SIZE: put_u16(data + 6, 64); break;
    case MUT_FLAGS: put_u32(data + 8, 1); break;
    case MUT_DIMS: put_u32(data + 16, 0); break;
    case MUT_COUNT: put_u64(data + 32, 2); break;
    case MUT_OFFSET: put_u64(data + 48, 1); break;
    case MUT_FILE_SIZE: put_u64(data + 56, get_u64(data + 56) - 1); break;
    case MUT_RESERVED: data[64] = 1; break;
    case MUT_CRC: data[124] ^= 0x80; recalc = 0; break;
    case MUT_ARITHMETIC: put_u64(data + 32, UINT64_MAX); break;
    }
    if (recalc) {
        put_u32(data + 124,
                (uint32_t)mz_crc32(MZ_CRC32_INIT, data, MPS_HEADER_SIZE - 4));
    }
    EXPECT(write_all(path, data, size), "write mutated header fixture");
    free(data);
    struct mps_file file;
    enum mps_error result = mps_open(path, &file);
    if (result == MPS_OK) mps_close(&file);
    remove(path);
    return result;
}

static void test_header_validation(void)
{
    EXPECT(open_header_mutation(MUT_MAGIC) == MPS_ERR_MAGIC, "bad magic");
    EXPECT(open_header_mutation(MUT_VERSION) == MPS_ERR_VERSION, "bad version");
    EXPECT(open_header_mutation(MUT_HEADER_SIZE) == MPS_ERR_HEADER_SIZE,
           "bad header size");
    EXPECT(open_header_mutation(MUT_FLAGS) == MPS_ERR_UNSUPPORTED_FLAGS,
           "bad flags");
    EXPECT(open_header_mutation(MUT_DIMS) == MPS_ERR_INVALID_DIMENSIONS,
           "bad dimensions");
    EXPECT(open_header_mutation(MUT_COUNT) == MPS_ERR_TILE_COUNT,
           "bad tile count");
    EXPECT(open_header_mutation(MUT_OFFSET) == MPS_ERR_INVALID_HEADER,
           "bad data offset");
    EXPECT(open_header_mutation(MUT_FILE_SIZE) == MPS_ERR_FILE_SIZE,
           "bad file size");
    EXPECT(open_header_mutation(MUT_RESERVED) == MPS_ERR_RESERVED,
           "bad reserved bytes");
    EXPECT(open_header_mutation(MUT_CRC) == MPS_ERR_HEADER_CRC,
           "bad header crc");
    EXPECT(open_header_mutation(MUT_ARITHMETIC) == MPS_ERR_OVERFLOW,
           "tile count arithmetic overflow");
}

static enum mps_error open_entry_mutation(uint16_t field, uint32_t value,
                                          int payload_corruption)
{
    char path[128];
    make_path(path, "entry");
    EXPECT(write_one_raw(path), "write entry fixture");
    uint8_t *data = nullptr;
    size_t size = 0;
    EXPECT(read_all(path, &data, &size), "read entry fixture");
    if (!data) {
        remove(path);
        return MPS_ERR_IO;
    }
    uint8_t *entry = data + MPS_HEADER_SIZE;
    if (field == 0) put_u64(entry, value);
    else if (field == 8) put_u32(entry + 8, value);
    else if (field == 12) put_u32(entry + 12, value);
    else if (field == 16) put_u32(entry + 16, value);
    else if (field == 20) put_u32(entry + 20, value);
    else if (field == 24) put_u16(entry + 24, (uint16_t)value);
    else if (field == 26) put_u16(entry + 26, (uint16_t)value);
    else if (field == 28) put_u32(entry + 28, value);
    if (payload_corruption) {
        size -= 1;
        put_u64(data + 56, size);
        put_u32(data + 124,
                (uint32_t)mz_crc32(MZ_CRC32_INIT, data, MPS_HEADER_SIZE - 4));
    }
    EXPECT(write_all(path, data, size), "write entry mutation");
    free(data);
    struct mps_file file = {0};
    enum mps_error result = mps_open(path, &file);
    uint8_t tile[MPS_TILE_BYTES];
    if (result == MPS_OK) {
        result = mps_read_tile(&file, 0, tile, sizeof(tile));
        mps_close(&file);
    }
    remove(path);
    return result;
}

static void test_entry_validation(void)
{
    EXPECT(open_entry_mutation(24, 9, 0) == MPS_ERR_CODEC, "bad codec");
    EXPECT(open_entry_mutation(26, 1, 0) == MPS_ERR_FLAGS, "bad entry flags");
    EXPECT(open_entry_mutation(28, 1, 0) == MPS_ERR_RESERVED,
           "bad entry reserved");
    EXPECT(open_entry_mutation(12, MPS_TILE_BYTES - 1, 0) == MPS_ERR_RAW_SIZE,
           "bad raw size");
    EXPECT(open_entry_mutation(8, MPS_TILE_BYTES - 1, 0) == MPS_ERR_STORED_SIZE,
           "bad stored size");
    EXPECT(open_entry_mutation(0, 0, 0) == MPS_ERR_DATA_RANGE,
           "bad data range");
    EXPECT(open_entry_mutation(20, 1, 0) == MPS_ERR_STORED_CRC,
           "bad stored crc");
    EXPECT(open_entry_mutation(16, 1, 0) == MPS_ERR_DECODED_CRC,
           "bad decoded crc");
    EXPECT(open_entry_mutation(0, 0, 1) == MPS_ERR_DATA_RANGE,
           "truncated data range");
}

static void test_demo_tiles(void)
{
    char path[128];
    make_path(path, "demo");
    EXPECT(write_demo(path), "write demo fixture");
    struct mps_file file;
    EXPECT(mps_open(path, &file) == MPS_OK, "open demo fixture");
    uint8_t *tile = malloc(MPS_TILE_BYTES + 16);
    EXPECT(tile != nullptr, "allocate tile output");
    if (tile && file.fp) {
        uint8_t expected = 0;
        EXPECT(mps_read_tile(&file, 0, tile, MPS_TILE_BYTES + 16) == MPS_OK,
               "read raw tile");
        EXPECT(tile[0] == 3 && tile[1] == 16, "raw tile pixels");
        EXPECT(mps_read_tile_xy(&file, 1, 0, tile, MPS_TILE_BYTES) == MPS_OK,
               "read zlib tile");
        EXPECT(tile[0] == 0 && tile[1] == 1 && tile[8] == 0,
               "zlib tile pixels");
        memset(tile, 0xFF, MPS_TILE_BYTES);
        EXPECT(mps_read_tile(&file, 2, tile, MPS_TILE_BYTES) == MPS_OK,
               "read implicit zero tile");
        for (size_t i = 0; i < MPS_TILE_BYTES; i++) {
            if (tile[i] != 0) expected = 1;
        }
        EXPECT(expected == 0, "implicit zero pixels");
        EXPECT(mps_read_tile(&file, 3, tile, MPS_TILE_BYTES) == MPS_OK,
               "read random raw tile");
        EXPECT(tile[0] == 255 && tile[1] == 254, "random raw pixels");
        struct mps_stats stats;
        EXPECT(mps_get_stats(&file, &stats) == MPS_OK &&
                   stats.index_refills >= 1 &&
                   stats.max_allocated_bytes <=
                       (size_t)mz_compressBound(MPS_TILE_BYTES) &&
                   stats.max_read_bytes <=
                       (size_t)mz_compressBound(MPS_TILE_BYTES),
               "bounded reader statistics");
    }
    if (file.fp) mps_close(&file);
    free(tile);
    remove(path);
}

static void test_index_window_and_sparse_open(void)
{
    char path[128];
    make_path(path, "window");
    EXPECT(write_sparse_index(path, 15, 20), "write 300-entry sparse fixture");
    struct mps_file file;
    EXPECT(mps_open(path, &file) == MPS_OK, "open sparse index fixture");
    struct mps_stats stats;
    EXPECT(mps_get_stats(&file, &stats) == MPS_OK && stats.index_refills == 0 &&
               stats.max_allocated_bytes == 0 && stats.max_read_bytes == MPS_HEADER_SIZE,
           "open does not scan sparse index");
    uint8_t tile[MPS_TILE_BYTES];
    EXPECT(mps_read_tile(&file, 0, tile, sizeof(tile)) == MPS_ERR_RAW_SIZE,
           "invalid sparse entry detected on demand");
    EXPECT(mps_read_tile(&file, 256, tile, sizeof(tile)) == MPS_ERR_RAW_SIZE,
           "second index window loaded on demand");
    EXPECT(mps_get_stats(&file, &stats) == MPS_OK && stats.index_refills == 2 &&
               stats.max_read_bytes <= MPS_INDEX_CACHE_ENTRIES * MPS_INDEX_ENTRY_SIZE,
           "index cache is bounded");
    mps_close(&file);
    remove(path);

    make_path(path, "large");
    EXPECT(write_sparse_index(path, 1024, 1024), "write large sparse fixture");
    EXPECT(mps_open(path, &file) == MPS_OK, "open 1024x1024 sparse fixture");
    EXPECT(mps_get_stats(&file, &stats) == MPS_OK && stats.index_refills == 0 &&
               stats.max_allocated_bytes == 0 &&
               stats.max_read_bytes == MPS_HEADER_SIZE,
           "large open remains bounded");
    mps_close(&file);
    remove(path);
}

int main(void)
{
    test_header_validation();
    test_entry_validation();
    test_demo_tiles();
    test_index_window_and_sparse_open();
    EXPECT(mps_u64_add_checked(UINT64_MAX, 1, nullptr) == 0,
           "checked add overflow");
    EXPECT(mps_u64_mul_checked(UINT64_MAX, 2, nullptr) == 0,
           "checked multiply overflow");
    if (g_failures != 0)
        fprintf(stderr, "%d MPS test failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

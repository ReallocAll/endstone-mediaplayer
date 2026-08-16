#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "mediaplayer/screen/screen_persistence.h"
#include "cJSON.h"
#include "platform.h"
#include "endstone_abi.h"
#include "abi_helpers.h"
#include "miniz.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ES_PLATFORM_WINDOWS)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// v2 sidecar layout (all integer fields are little-endian):
//   0x00  8 bytes  magic ("ESMPID2\0")
//   0x08  2 bytes  sidecar format version
//   0x0a  2 bytes  header size (currently 32)
//   0x0c  8 bytes  payload byte length
//   0x14  4 bytes  CRC-32 of the payload
//   0x18  4 bytes  reserved, must be zero
//   0x1c  4 bytes  reserved, must be zero
// The payload is exactly payload byte length bytes and consists solely of
// signed int64 map IDs encoded as their two's-complement little-endian bits.
//
// The immutable sidecar basename is:
//   screens.mapids.<payload-bytes>.<fnv1a64>.<crc32>.bin
// The identity is FNV-1a-64 over the payload, using the standard offset basis
// and prime.  The CRC is the standard miniz CRC-32 over the same bytes.  Thus
// a changed payload gets a different name without consulting filesystem time.

#define SCREEN_SIDECAR_TEMP_SUFFIX ".tmp"
#define SCREEN_MANIFEST_TEMP_SUFFIX ".tmp"
#define SCREEN_MANIFEST_MAX_BYTES (10U * 1024U * 1024U)
#define SCREEN_IO_BUFFER_SIZE (8U * 1024U)

static void *g_screen_log_plugin = nullptr;

struct sidecar_ref {
    uint64_t offset;
    uint64_t count;
    uint32_t crc32;
    int present;
};

struct sidecar_write_state {
    uint64_t payload_bytes;
    uint64_t identity;
    uint32_t crc32;
    struct sidecar_ref refs[SCREEN_REGISTRY_MAX];
    int ref_count;
};

static void screen_log(const char *fmt, ...)
{
    if (!g_screen_log_plugin)
        return;

    char buffer[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    PLUGIN_LOG(g_screen_log_plugin, ES_LOG_INFO, buffer);
}

void screen_persistence_set_log_plugin(void *plugin)
{
    g_screen_log_plugin = plugin;
}

static FILE *fopen_utf8_local(const char *path, const char *mode)
{
#if defined(ES_PLATFORM_WINDOWS)
    if (!path || !mode)
        return nullptr;

    int path_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    int mode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    if (path_len <= 0 || mode_len <= 0)
        return nullptr;

    wchar_t *wpath = calloc((size_t)path_len, sizeof(*wpath));
    wchar_t *wmode = calloc((size_t)mode_len, sizeof(*wmode));
    if (!wpath || !wmode) {
        free(wpath);
        free(wmode);
        return nullptr;
    }

    int path_ok = MultiByteToWideChar(CP_UTF8, 0, path, -1,
                                      wpath, path_len);
    int mode_ok = MultiByteToWideChar(CP_UTF8, 0, mode, -1,
                                      wmode, mode_len);
    FILE *fp = nullptr;
    if (path_ok > 0 && mode_ok > 0)
        fp = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return fp;
#else
    return fopen(path, mode);
#endif
}

static int checked_add_size(size_t left, size_t right, size_t *out)
{
    if (left > SIZE_MAX - right)
        return -1;
    *out = left + right;
    return 0;
}

static int checked_mul_u64(uint64_t left, uint64_t right, uint64_t *out)
{
    if (left != 0 && right > UINT64_MAX / left)
        return -1;
    *out = left * right;
    return 0;
}

static int path_with_suffix(const char *path, const char *suffix, char **out)
{
    if (!path || !suffix || !out)
        return -1;

    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    size_t total = 0;
    if (checked_add_size(path_len, suffix_len, &total) != 0 ||
        checked_add_size(total, 1, &total) != 0)
        return -1;

    char *result = calloc(total, 1);
    if (!result)
        return -1;
    memcpy(result, path, path_len);
    memcpy(result + path_len, suffix, suffix_len);
    *out = result;
    return 0;
}

static int path_directory(const char *path, char **out)
{
    if (!path || !out)
        return -1;

    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *separator = last_slash;
    if (last_backslash && (!separator || last_backslash > separator))
        separator = last_backslash;

    if (!separator) {
        char *dot = calloc(2, 1);
        if (!dot)
            return -1;
        dot[0] = '.';
        *out = dot;
        return 0;
    }

    size_t len = (size_t)(separator - path);
    if (len == 0)
        len = 1;
#if defined(ES_PLATFORM_WINDOWS)
    if (len == 2 && path[1] == ':')
        len = 3;
#endif

    size_t allocation_size = 0;
    if (checked_add_size(len, 1, &allocation_size) != 0)
        return -1;
    char *directory = calloc(allocation_size, 1);
    if (!directory)
        return -1;
    memcpy(directory, path, len);
    directory[len] = '\0';
    *out = directory;
    return 0;
}

static int path_join_basename(const char *directory, const char *basename,
                              char **out)
{
    if (!directory || !basename || !out || !basename[0])
        return -1;

    size_t dir_len = strlen(directory);
    size_t base_len = strlen(basename);
    int needs_separator = dir_len > 0 && directory[dir_len - 1] != '/' &&
                          directory[dir_len - 1] != '\\';
    size_t total = dir_len;
    if (needs_separator && checked_add_size(total, 1, &total) != 0)
        return -1;
    if (checked_add_size(total, base_len, &total) != 0 ||
        checked_add_size(total, 1, &total) != 0)
        return -1;

    char *result = calloc(total, 1);
    if (!result)
        return -1;
    memcpy(result, directory, dir_len);
    size_t pos = dir_len;
    if (needs_separator)
        result[pos++] =
#if defined(ES_PLATFORM_WINDOWS)
            '\\';
#else
            '/';
#endif
    memcpy(result + pos, basename, base_len);
    *out = result;
    return 0;
}

static int sidecar_name(uint64_t payload_bytes, uint64_t identity,
                        uint32_t crc32, char **out)
{
    char text[128];
    int written = snprintf(text, sizeof(text),
                           "screens.mapids.%" PRIu64 ".%016" PRIx64
                           ".%08" PRIx32 ".bin",
                           payload_bytes, identity, crc32);
    if (written < 0 || (size_t)written >= sizeof(text))
        return -1;

    char *name = calloc((size_t)written + 1, 1);
    if (!name)
        return -1;
    memcpy(name, text, (size_t)written);
    *out = name;
    return 0;
}

static uint16_t read_u16_le(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read_u32_le(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read_u64_le(const unsigned char *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static void write_u16_le(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void write_u32_le(unsigned char *p, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

static void write_u64_le(unsigned char *p, uint64_t value)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

static void encode_i64_le(unsigned char *p, int64_t value)
{
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    write_u64_le(p, bits);
}

static int64_t decode_i64_le(const unsigned char *p)
{
    uint64_t bits = read_u64_le(p);
    int64_t value = 0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void make_sidecar_header(unsigned char header[SCREEN_SIDECAR_HEADER_SIZE],
                                uint64_t payload_bytes, uint32_t crc32)
{
    memset(header, 0, SCREEN_SIDECAR_HEADER_SIZE);
    memcpy(header, SCREEN_SIDECAR_MAGIC, 8);
    write_u16_le(header + 8, SCREEN_SIDECAR_VERSION);
    write_u16_le(header + 10, SCREEN_SIDECAR_HEADER_SIZE);
    write_u64_le(header + 12, payload_bytes);
    write_u32_le(header + 20, crc32);
}

static int read_exact(FILE *fp, void *buffer, size_t size)
{
    if (size == 0)
        return 0;
    return fread(buffer, 1, size, fp) == size ? 0 : -1;
}

static int file_size(FILE *fp, uint64_t *out)
{
    if (!fp || !out)
        return -1;
#if defined(ES_PLATFORM_WINDOWS)
    if (_fseeki64(fp, 0, SEEK_END) != 0)
        return -1;
    __int64 end = _ftelli64(fp);
    if (end < 0 || _fseeki64(fp, 0, SEEK_SET) != 0)
        return -1;
    *out = (uint64_t)end;
#else
    if (fseeko(fp, 0, SEEK_END) != 0)
        return -1;
    off_t end = ftello(fp);
    if (end < 0 || fseeko(fp, 0, SEEK_SET) != 0)
        return -1;
    *out = (uint64_t)end;
#endif
    return 0;
}

static int seek_u64(FILE *fp, uint64_t position)
{
#if defined(ES_PLATFORM_WINDOWS)
    if (position > (uint64_t)INT64_MAX)
        return -1;
    return _fseeki64(fp, (__int64)position, SEEK_SET) == 0 ? 0 : -1;
#else
    if (position > (uint64_t)INT64_MAX)
        return -1;
    return fseeko(fp, (off_t)position, SEEK_SET) == 0 ? 0 : -1;
#endif
}

static int flush_and_commit(FILE *fp)
{
    if (!fp || fflush(fp) != 0)
        return -1;
#if defined(ES_PLATFORM_WINDOWS)
    if (_commit(_fileno(fp)) != 0)
        return -1;
#else
    if (fsync(fileno(fp)) != 0)
        return -1;
#endif
    return 0;
}

static void fsync_parent_directory(const char *path)
{
#if defined(ES_PLATFORM_WINDOWS)
    (void)path;
#else
    char *directory = nullptr;
    if (path_directory(path, &directory) != 0)
        return;
    int fd = open(directory, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
    free(directory);
#endif
}

static int atomic_install(const char *temporary, const char *destination)
{
#if defined(ES_PLATFORM_WINDOWS)
    if (!MoveFileExA(temporary, destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return -1;
#else
    if (rename(temporary, destination) != 0)
        return -1;
    fsync_parent_directory(destination);
#endif
    return 0;
}

static int files_equal(const char *left_path, const char *right_path)
{
    FILE *left = fopen_utf8_local(left_path, "rb");
    FILE *right = fopen_utf8_local(right_path, "rb");
    if (!left || !right) {
        if (left)
            fclose(left);
        if (right)
            fclose(right);
        return 0;
    }
    uint64_t left_size = 0;
    uint64_t right_size = 0;
    int equal = file_size(left, &left_size) == 0 &&
                file_size(right, &right_size) == 0 &&
                left_size == right_size;
    unsigned char left_buffer[4096];
    unsigned char right_buffer[4096];
    while (equal && left_size > 0) {
        size_t bytes = left_size > sizeof(left_buffer)
                           ? sizeof(left_buffer)
                           : (size_t)left_size;
        if (read_exact(left, left_buffer, bytes) != 0 ||
            read_exact(right, right_buffer, bytes) != 0 ||
            memcmp(left_buffer, right_buffer, bytes) != 0) {
            equal = 0;
            break;
        }
        left_size -= bytes;
    }
    fclose(left);
    fclose(right);
    return equal;
}

static int install_immutable_sidecar(const char *temporary,
                                     const char *destination)
{
#if defined(ES_PLATFORM_WINDOWS)
    if (MoveFileExA(temporary, destination, MOVEFILE_WRITE_THROUGH))
        return 0;
    DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
        return -1;
#else
    if (link(temporary, destination) == 0) {
        if (unlink(temporary) != 0)
            return -1;
        fsync_parent_directory(destination);
        return 0;
    }
    if (errno != EEXIST)
        return -1;
#endif
    if (!files_equal(temporary, destination))
        return -1;
    if (remove(temporary) != 0)
        return -1;
    return 0;
}

static int copy_file_to_first_bad(const char *path)
{
    if (!path)
        return -1;

    FILE *source = fopen_utf8_local(path, "rb");
    if (!source)
        return -1;

    for (uint64_t suffix = 0; suffix < UINT64_MAX; suffix++) {
        char suffix_text[64];
        int suffix_len = suffix == 0
                             ? snprintf(suffix_text, sizeof(suffix_text),
                                        ".bad")
                             : snprintf(suffix_text, sizeof(suffix_text),
                                         ".bad.%" PRIu64, suffix);
        if (suffix_len < 0 || (size_t)suffix_len >= sizeof(suffix_text))
            break;

        char *candidate = nullptr;
        if (path_with_suffix(path, suffix_text, &candidate) != 0)
            break;

        errno = 0;
        FILE *copy = fopen_utf8_local(candidate, "wbx");
        if (!copy) {
            int open_error = errno;
            free(candidate);
            if (open_error == EEXIST)
                continue;
            break;
        }

        unsigned char buffer[SCREEN_IO_BUFFER_SIZE];
        int ok = 1;
        for (;;) {
            size_t read_count = fread(buffer, 1, sizeof(buffer), source);
            if (read_count > 0 && fwrite(buffer, 1, read_count, copy) !=
                                      read_count) {
                ok = 0;
                break;
            }
            if (read_count < sizeof(buffer)) {
                if (ferror(source))
                    ok = 0;
                break;
            }
        }
        if (ok && flush_and_commit(copy) != 0)
            ok = 0;
        if (fclose(copy) != 0)
            ok = 0;
        if (!ok) {
            remove(candidate);
            free(candidate);
            fclose(source);
            return -1;
        }

        fsync_parent_directory(candidate);
        screen_log("preserved unreadable screens file as %s", candidate);
        free(candidate);
        fclose(source);
        return 0;
    }

    fclose(source);
    screen_log("could not copy %s aside; the original was left untouched", path);
    return -1;
}

static int load_reject(const char *path, int *warnings, const char *reason)
{
    screen_log("cannot load %s: %s", path, reason);
    (void)copy_file_to_first_bad(path);
    if (warnings)
        *warnings = 1;
    return -1;
}

static void replace_registry(struct screen_registry *destination,
                              struct screen_registry *loaded)
{
    screen_registry_cleanup(destination);
    *destination = *loaded;
    screen_registry_init(loaded);
}

static int cjson_number_integral(const cJSON *item, double *value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble)
        return -1;
    if (value)
        *value = item->valuedouble;
    return 0;
}

static int parse_i64_text(const char *text, int64_t *out)
{
    if (!text || !text[0] || !out)
        return -1;
    if (text[0] == '+')
        return -1;
    const char *digits = text[0] == '-' ? text + 1 : text;
    if (!digits[0])
        return -1;
    for (const char *p = digits; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    char *end = nullptr;
    long long value = strtoll(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0')
        return -1;
    *out = (int64_t)value;
    return 0;
}

static int parse_u64_text(const char *text, uint64_t *out)
{
    if (!text || !text[0] || !out)
        return -1;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    char *end = nullptr;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0')
        return -1;
    *out = (uint64_t)value;
    return 0;
}

static int parse_i64_json(const cJSON *item, int64_t *out)
{
    if (cJSON_IsString(item))
        return parse_i64_text(item->valuestring, out);

    double value = 0;
    if (cjson_number_integral(item, &value) != 0 ||
        value < (double)INT64_MIN || value > (double)INT64_MAX ||
        fabs(value) > 9007199254740992.0)
        return -1;
    *out = (int64_t)value;
    return 0;
}

static int parse_u64_json(const cJSON *item, uint64_t *out)
{
    if (cJSON_IsString(item))
        return parse_u64_text(item->valuestring, out);

    double value = 0;
    if (cjson_number_integral(item, &value) != 0 || value < 0 ||
        value > 9007199254740992.0)
        return -1;
    *out = (uint64_t)value;
    return 0;
}

static int parse_u32_json(const cJSON *item, uint32_t *out)
{
    uint64_t value = 0;
    if (parse_u64_json(item, &value) != 0 || value > UINT32_MAX)
        return -1;
    *out = (uint32_t)value;
    return 0;
}

static int parse_int_json(const cJSON *item, int *out)
{
    double value = 0;
    if (cjson_number_integral(item, &value) != 0 ||
        value < (double)INT_MIN || value > (double)INT_MAX ||
        fabs(value) > 9007199254740992.0)
        return -1;
    *out = (int)value;
    return 0;
}

static int parse_pos(const cJSON *obj, struct screen_pos *pos)
{
    if (!cJSON_IsObject(obj) || !pos)
        return -1;
    cJSON *x = cJSON_GetObjectItemCaseSensitive(obj, "x");
    cJSON *y = cJSON_GetObjectItemCaseSensitive(obj, "y");
    cJSON *z = cJSON_GetObjectItemCaseSensitive(obj, "z");
    if (parse_int_json(x, &pos->x) != 0 || parse_int_json(y, &pos->y) != 0 ||
        parse_int_json(z, &pos->z) != 0)
        return -1;
    return 0;
}

static int valid_sidecar_basename(const char *name)
{
    if (!name || !name[0] || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':')
            return 0;
    }
    return 1;
}

static int open_and_validate_sidecar(const char *path, FILE **out,
                                     uint64_t *payload_bytes)
{
    if (!path || !out || !payload_bytes)
        return -1;

    FILE *fp = fopen_utf8_local(path, "rb");
    if (!fp)
        return -1;

    uint64_t actual_size = 0;
    unsigned char header[SCREEN_SIDECAR_HEADER_SIZE];
    if (file_size(fp, &actual_size) != 0 ||
        actual_size < SCREEN_SIDECAR_HEADER_SIZE ||
        read_exact(fp, header, sizeof(header)) != 0 ||
        memcmp(header, SCREEN_SIDECAR_MAGIC, 8) != 0 ||
        read_u16_le(header + 8) != SCREEN_SIDECAR_VERSION ||
        read_u16_le(header + 10) != SCREEN_SIDECAR_HEADER_SIZE ||
        read_u32_le(header + 24) != 0 || read_u32_le(header + 28) != 0) {
        fclose(fp);
        return -1;
    }

    uint64_t bytes = read_u64_le(header + 12);
    if (bytes > UINT64_MAX - SCREEN_SIDECAR_HEADER_SIZE ||
        actual_size != (uint64_t)SCREEN_SIDECAR_HEADER_SIZE + bytes ||
        bytes % sizeof(int64_t) != 0) {
        fclose(fp);
        return -1;
    }

    if (seek_u64(fp, SCREEN_SIDECAR_HEADER_SIZE) != 0) {
        fclose(fp);
        return -1;
    }
    unsigned char buffer[SCREEN_IO_BUFFER_SIZE];
    uint64_t remaining = bytes;
    mz_ulong crc = MZ_CRC32_INIT;
    while (remaining > 0) {
        size_t want = remaining > sizeof(buffer) ? sizeof(buffer) :
                                                   (size_t)remaining;
        if (read_exact(fp, buffer, want) != 0) {
            fclose(fp);
            return -1;
        }
        crc = mz_crc32(crc, buffer, want);
        remaining -= want;
    }
    if ((uint32_t)crc != read_u32_le(header + 20) ||
        seek_u64(fp, SCREEN_SIDECAR_HEADER_SIZE) != 0) {
        fclose(fp);
        return -1;
    }

    *payload_bytes = bytes;
    *out = fp;
    return 0;
}

static int parse_v2_ref(const cJSON *screen, struct sidecar_ref *ref)
{
    if (!screen || !ref)
        return -1;
    cJSON *map_ids = cJSON_GetObjectItemCaseSensitive(screen, "map_ids");
    if (!map_ids) {
        memset(ref, 0, sizeof(*ref));
        return 0;
    }
    if (!cJSON_IsObject(map_ids))
        return -1;

    cJSON *offset = cJSON_GetObjectItemCaseSensitive(map_ids, "offset");
    cJSON *count = cJSON_GetObjectItemCaseSensitive(map_ids, "count");
    cJSON *crc32 = cJSON_GetObjectItemCaseSensitive(map_ids, "crc32");
    if (parse_u64_json(offset, &ref->offset) != 0 ||
        parse_u64_json(count, &ref->count) != 0 ||
        parse_u32_json(crc32, &ref->crc32) != 0)
        return -1;
    ref->present = 1;
    return 0;
}

static int read_sidecar_ids(FILE *fp, uint64_t payload_bytes,
                            const struct sidecar_ref *ref,
                            struct screen_entry *entry)
{
    if (!fp || !ref || !entry || !ref->present || ref->count == 0 ||
        ref->count > INT_MAX || ref->offset % sizeof(int64_t) != 0 ||
        ref->offset > payload_bytes ||
        ref->count > (payload_bytes - ref->offset) / sizeof(int64_t))
        return -1;

    int tile_count = screen_geom_tile_count(&entry->geom);
    if (tile_count <= 0 || ref->count != (uint64_t)tile_count)
        return -1;
    if (screen_entry_materialize_tiles(entry) != SCREEN_OK)
        return -1;

    uint64_t byte_offset = (uint64_t)SCREEN_SIDECAR_HEADER_SIZE;
    if (byte_offset > UINT64_MAX - ref->offset)
        return -1;
    byte_offset += ref->offset;
    if (seek_u64(fp, byte_offset) != 0)
        return -1;

    unsigned char encoded[sizeof(int64_t)];
    mz_ulong crc = MZ_CRC32_INIT;
    for (uint64_t i = 0; i < ref->count; i++) {
        if (read_exact(fp, encoded, sizeof(encoded)) != 0)
            return -1;
        crc = mz_crc32(crc, encoded, sizeof(encoded));
        entry->tiles[i].map_id = decode_i64_le(encoded);
        entry->tiles[i].map_id_valid = 1;
    }
    return (uint32_t)crc == ref->crc32 ? 0 : -1;
}

static int persist_ids_for_entry(const struct screen_entry *entry)
{
    if (!entry || !entry->plugin_managed || !entry->tiles)
        return 0;
    int count = screen_geom_tile_count(&entry->geom);
    if (count <= 0 || entry->tiles_capacity < (size_t)count)
        return -1;
    for (int i = 0; i < count; i++) {
        if (!entry->tiles[i].map_id_valid)
            return -1;
    }
    return 1;
}

static int write_sidecar_payload(FILE *fp, const struct screen_registry *reg,
                                 struct sidecar_write_state *state)
{
    if (!fp || !reg || !state)
        return -1;

    state->payload_bytes = 0;
    state->identity = UINT64_C(14695981039346656037);
    state->crc32 = MZ_CRC32_INIT;
    state->ref_count = 0;

    unsigned char header[SCREEN_SIDECAR_HEADER_SIZE] = {0};
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header))
        return -1;

    for (int i = 0; i < reg->count; i++) {
        const struct screen_entry *entry = reg->screens[i];
        int persist_ids = persist_ids_for_entry(entry);
        if (persist_ids < 0)
            return -1;
        if (persist_ids == 0)
            continue;

        int tile_count = screen_geom_tile_count(&entry->geom);
        uint64_t bytes = 0;
        if (checked_mul_u64((uint64_t)tile_count, sizeof(int64_t), &bytes) !=
                0 ||
            state->payload_bytes > UINT64_MAX - bytes ||
            state->ref_count >= SCREEN_REGISTRY_MAX)
            return -1;

        struct sidecar_ref *ref = &state->refs[i];
        ref->offset = state->payload_bytes;
        ref->count = (uint64_t)tile_count;
        ref->crc32 = MZ_CRC32_INIT;
        ref->present = 1;
        state->ref_count++;

        for (int tile = 0; tile < tile_count; tile++) {
            unsigned char encoded[sizeof(int64_t)];
            encode_i64_le(encoded, entry->tiles[tile].map_id);
            if (fwrite(encoded, 1, sizeof(encoded), fp) != sizeof(encoded))
                return -1;
            state->crc32 = mz_crc32(state->crc32, encoded, sizeof(encoded));
            ref->crc32 = mz_crc32(ref->crc32, encoded, sizeof(encoded));
            for (size_t byte = 0; byte < sizeof(encoded); byte++) {
                state->identity ^= encoded[byte];
                state->identity *= UINT64_C(1099511628211);
            }
        }
        state->payload_bytes += bytes;
    }

    if (state->ref_count == 0)
        return 0;
    if (seek_u64(fp, 0) != 0)
        return -1;
    make_sidecar_header(header, state->payload_bytes, (uint32_t)state->crc32);
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header))
        return -1;
    return 0;
}

static int add_string(cJSON *object, const char *key, const char *value)
{
    return cJSON_AddStringToObject(object, key, value ? value : "") ? 0 : -1;
}

static int add_number(cJSON *object, const char *key, double value)
{
    return cJSON_AddNumberToObject(object, key, value) ? 0 : -1;
}

static int add_bool(cJSON *object, const char *key, int value)
{
    return cJSON_AddBoolToObject(object, key, value != 0) ? 0 : -1;
}

static int add_playback(cJSON *screen,
                        const struct screen_playback_checkpoint *playback)
{
    if (!screen || !playback)
        return -1;
    if (playback->state == SCREEN_PLAYBACK_STOPPED)
        return 0;
    if ((playback->state != SCREEN_PLAYBACK_PLAYING &&
         playback->state != SCREEN_PLAYBACK_PAUSED) ||
        !playback->video_name[0] ||
        playback->loop_total < -1 || playback->loop_total == 0 ||
        playback->loop_current < 1 ||
        (playback->loop_total > 0 &&
         playback->loop_current > playback->loop_total))
        return -1;

    cJSON *value = cJSON_CreateObject();
    if (!value)
        return -1;
    const char *state = playback->state == SCREEN_PLAYBACK_PAUSED
                            ? "paused"
                            : "playing";
    if (add_string(value, "state", state) != 0 ||
        add_string(value, "video", playback->video_name) != 0 ||
        add_number(value, "current_frame", playback->current_frame) != 0 ||
        add_number(value, "loop_total", playback->loop_total) != 0 ||
        add_number(value, "loop_current", playback->loop_current) != 0 ||
        !cJSON_AddItemToObject(screen, "playback", value)) {
        cJSON_Delete(value);
        return -1;
    }
    return 0;
}

static int add_i64_string(cJSON *object, const char *key, int64_t value)
{
    char text[32];
    int written = snprintf(text, sizeof(text), "%" PRId64, value);
    if (written < 0 || (size_t)written >= sizeof(text))
        return -1;
    return add_string(object, key, text);
}

static int add_u64_string(cJSON *object, const char *key, uint64_t value)
{
    char text[32];
    int written = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (written < 0 || (size_t)written >= sizeof(text))
        return -1;
    return add_string(object, key, text);
}

static int build_manifest(const struct screen_registry *reg,
                          const char *sidecar_basename,
                          const struct sidecar_write_state *state,
                          char **json_out)
{
    if (!reg || !state || !json_out)
        return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return -1;
    if (add_number(root, "format_version", SCREEN_SAVE_VERSION) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    if (state->ref_count > 0 &&
        add_string(root, SCREEN_SIDECAR_FIELD, sidecar_basename) != 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *screens = cJSON_AddArrayToObject(root, "screens");
    if (!screens) {
        cJSON_Delete(root);
        return -1;
    }

    for (int i = 0; i < reg->count; i++) {
        const struct screen_entry *entry = reg->screens[i];
        if (!entry) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON *screen = cJSON_CreateObject();
        if (!screen || add_string(screen, "name", entry->name) != 0 ||
            add_string(screen, "owner_uuid", entry->owner_uuid) != 0 ||
            add_string(screen, "dimension", entry->geom.dimension) != 0 ||
            add_number(screen, "facing", entry->geom.facing) != 0 ||
            add_number(screen, "width", entry->geom.width) != 0 ||
            add_number(screen, "height", entry->geom.height) != 0 ||
            add_bool(screen, "plugin_managed", entry->plugin_managed) != 0) {
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }

        cJSON *corner1 = cJSON_CreateObject();
        cJSON *corner2 = cJSON_CreateObject();
        if (!corner1 || !corner2 ||
            add_number(corner1, "x", entry->geom.corner1.x) != 0 ||
            add_number(corner1, "y", entry->geom.corner1.y) != 0 ||
            add_number(corner1, "z", entry->geom.corner1.z) != 0 ||
            add_number(corner2, "x", entry->geom.corner2.x) != 0 ||
            add_number(corner2, "y", entry->geom.corner2.y) != 0 ||
            add_number(corner2, "z", entry->geom.corner2.z) != 0) {
            cJSON_Delete(corner1);
            cJSON_Delete(corner2);
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }
        if (!cJSON_AddItemToObject(screen, "corner1", corner1)) {
            cJSON_Delete(corner1);
            cJSON_Delete(corner2);
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }
        corner1 = nullptr;
        if (!cJSON_AddItemToObject(screen, "corner2", corner2)) {
            cJSON_Delete(corner2);
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }
        corner2 = nullptr;
        if (add_i64_string(screen, "created_at", entry->created_at) != 0) {
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }
        if (add_playback(screen, &entry->playback) != 0) {
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }

        if (state->refs[i].present) {
            cJSON *ref = cJSON_CreateObject();
            if (!ref || add_u64_string(ref, "offset", state->refs[i].offset) !=
                              0 ||
                add_u64_string(ref, "count", state->refs[i].count) != 0 ||
                add_u64_string(ref, "crc32", state->refs[i].crc32) != 0) {
                cJSON_Delete(ref);
                cJSON_Delete(screen);
                cJSON_Delete(root);
                return -1;
            }
            if (!cJSON_AddItemToObject(screen, "map_ids", ref)) {
                cJSON_Delete(ref);
                cJSON_Delete(screen);
                cJSON_Delete(root);
                return -1;
            }
            ref = nullptr;
        }

        if (cJSON_AddItemToArray(screens, screen) == 0) {
            cJSON_Delete(screen);
            cJSON_Delete(root);
            return -1;
        }
        screen = nullptr;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return -1;
    *json_out = json;
    return 0;
}

int screen_persistence_save(const struct screen_registry *reg, const char *path)
{
    if (!reg || !path || !path[0] || reg->count < 0 ||
        reg->count > SCREEN_REGISTRY_MAX)
        return -1;

    struct sidecar_write_state state = {0};
    char *sidecar_temp = nullptr;
    char *sidecar_basename = nullptr;
    char *sidecar_final = nullptr;
    char *manifest_temp = nullptr;
    char *json = nullptr;

    if (path_with_suffix(path, ".maps" SCREEN_SIDECAR_TEMP_SUFFIX,
                         &sidecar_temp) != 0)
        goto fail;

    FILE *sidecar_fp = fopen_utf8_local(sidecar_temp, "wb+");
    if (!sidecar_fp)
        goto fail;
    int sidecar_ok = write_sidecar_payload(sidecar_fp, reg, &state) == 0 &&
                     flush_and_commit(sidecar_fp) == 0;
    if (fclose(sidecar_fp) != 0)
        sidecar_ok = 0;
    if (!sidecar_ok) {
        remove(sidecar_temp);
        goto fail;
    }

    if (state.ref_count > 0) {
        if (sidecar_name(state.payload_bytes, state.identity,
                         (uint32_t)state.crc32, &sidecar_basename) != 0)
            goto fail;
        char *directory = nullptr;
        if (path_directory(path, &directory) != 0 ||
            path_join_basename(directory, sidecar_basename, &sidecar_final) !=
                0) {
            free(directory);
            goto fail;
        }
        free(directory);
        if (install_immutable_sidecar(sidecar_temp, sidecar_final) != 0)
            goto fail;
    } else {
        remove(sidecar_temp);
    }

    if (build_manifest(reg, sidecar_basename, &state, &json) != 0)
        goto fail;
    if (path_with_suffix(path, SCREEN_MANIFEST_TEMP_SUFFIX,
                         &manifest_temp) != 0)
        goto fail;

    FILE *manifest_fp = fopen_utf8_local(manifest_temp, "wb");
    if (!manifest_fp)
        goto fail;
    size_t json_len = strlen(json);
    int manifest_ok = fwrite(json, 1, json_len, manifest_fp) == json_len &&
                      flush_and_commit(manifest_fp) == 0;
    if (fclose(manifest_fp) != 0)
        manifest_ok = 0;
    if (!manifest_ok) {
        remove(manifest_temp);
        goto fail;
    }
    if (atomic_install(manifest_temp, path) != 0) {
        remove(manifest_temp);
        goto fail;
    }

    free(sidecar_temp);
    free(sidecar_basename);
    free(sidecar_final);
    free(manifest_temp);
    free(json);
    return 0;

fail:
    if (sidecar_temp)
        remove(sidecar_temp);
    if (manifest_temp)
        remove(manifest_temp);
    free(sidecar_temp);
    free(sidecar_basename);
    free(sidecar_final);
    free(manifest_temp);
    free(json);
    return -1;
}

static int parse_screen_metadata(const cJSON *screen, struct screen_geom *geom,
                                 const char **name, const char **owner)
{
    if (!cJSON_IsObject(screen) || !geom || !name || !owner)
        return -1;

    cJSON *jname = cJSON_GetObjectItemCaseSensitive(screen, "name");
    if (!cJSON_IsString(jname) || !screen_name_valid(jname->valuestring))
        return -1;

    cJSON *dimension = cJSON_GetObjectItemCaseSensitive(screen, "dimension");
    if (!cJSON_IsString(dimension) || !dimension->valuestring[0] ||
        strlen(dimension->valuestring) >= sizeof(geom->dimension))
        return -1;

    cJSON *facing = cJSON_GetObjectItemCaseSensitive(screen, "facing");
    cJSON *width = cJSON_GetObjectItemCaseSensitive(screen, "width");
    cJSON *height = cJSON_GetObjectItemCaseSensitive(screen, "height");
    int facing_value = 0;
    int persisted_width = 0;
    int persisted_height = 0;
    if (parse_int_json(facing, &facing_value) != 0 ||
        facing_value < SCREEN_FACE_SOUTH || facing_value > SCREEN_FACE_WEST ||
        parse_int_json(width, &persisted_width) != 0 ||
        parse_int_json(height, &persisted_height) != 0 ||
        screen_geom_validate_dimensions(persisted_width, persisted_height) !=
            SCREEN_GEOM_OK)
        return -1;

    cJSON *corner1 = cJSON_GetObjectItemCaseSensitive(screen, "corner1");
    cJSON *corner2 = cJSON_GetObjectItemCaseSensitive(screen, "corner2");
    struct screen_pos position1;
    struct screen_pos position2;
    if (parse_pos(corner1, &position1) != 0 ||
        parse_pos(corner2, &position2) != 0 ||
        screen_geom_validate(position1, position2, dimension->valuestring,
                             dimension->valuestring,
                             (enum screen_facing)facing_value, geom) !=
            SCREEN_GEOM_OK ||
        geom->width != persisted_width || geom->height != persisted_height)
        return -1;

    *name = jname->valuestring;
    cJSON *jowner = cJSON_GetObjectItemCaseSensitive(screen, "owner_uuid");
    *owner = cJSON_IsString(jowner) ? jowner->valuestring : "";
    return 0;
}

static int load_legacy_ids(const cJSON *screen, struct screen_entry *entry,
                           int *warning)
{
    if (!screen || !entry || !warning)
        return -1;
    cJSON *managed = cJSON_GetObjectItemCaseSensitive(screen, "plugin_managed");
    if (!cJSON_IsTrue(managed))
        return 0;
    entry->plugin_managed = 1;

    cJSON *map_ids = cJSON_GetObjectItemCaseSensitive(screen, "map_ids");
    int tile_count = screen_geom_tile_count(&entry->geom);
    if (!cJSON_IsArray(map_ids) || cJSON_GetArraySize(map_ids) != tile_count) {
        if (map_ids)
            *warning = 1;
        return 0;
    }
    if (screen_entry_materialize_tiles(entry) != SCREEN_OK) {
        *warning = 1;
        return 0;
    }

    int valid = 1;
    for (int tile = 0; tile < tile_count; tile++) {
        cJSON *id = cJSON_GetArrayItem(map_ids, tile);
        int64_t value = 0;
        if (!cJSON_IsString(id) ||
            parse_i64_text(id->valuestring, &value) != 0) {
            valid = 0;
            break;
        }
        entry->tiles[tile].map_id = value;
        entry->tiles[tile].map_id_valid = 1;
    }
    if (!valid) {
        for (int tile = 0; tile < tile_count; tile++) {
            entry->tiles[tile].map_id = -1;
            entry->tiles[tile].map_id_valid = 0;
        }
        *warning = 1;
    }
    return 0;
}

static int get_v2_sidecar_name(const cJSON *root, const char **name)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(root, SCREEN_SIDECAR_FIELD);
    if (!field)
        field = cJSON_GetObjectItemCaseSensitive(root, "sidecar");
    if (!field) {
        *name = nullptr;
        return 0;
    }
    if (!cJSON_IsString(field) || !valid_sidecar_basename(field->valuestring))
        return -1;
    *name = field->valuestring;
    return 0;
}

static int parse_playback(const cJSON *screen,
                          struct screen_playback_checkpoint *out)
{
    if (!screen || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    cJSON *value = cJSON_GetObjectItemCaseSensitive(screen, "playback");
    if (!value)
        return 0;
    if (!cJSON_IsObject(value))
        return -1;

    cJSON *state = cJSON_GetObjectItemCaseSensitive(value, "state");
    cJSON *video = cJSON_GetObjectItemCaseSensitive(value, "video");
    if (!video)
        video = cJSON_GetObjectItemCaseSensitive(value, "video_name");
    cJSON *frame = cJSON_GetObjectItemCaseSensitive(value, "current_frame");
    cJSON *loop_total = cJSON_GetObjectItemCaseSensitive(value, "loop_total");
    cJSON *loop_current =
        cJSON_GetObjectItemCaseSensitive(value, "loop_current");
    if (!cJSON_IsString(state) || !cJSON_IsString(video) ||
        !video->valuestring[0] ||
        strlen(video->valuestring) >= SCREEN_PLAYBACK_VIDEO_NAME_MAX ||
        parse_u32_json(frame, &out->current_frame) != 0 ||
        parse_int_json(loop_total, &out->loop_total) != 0 ||
        parse_int_json(loop_current, &out->loop_current) != 0 ||
        out->loop_current < 1 || out->loop_total < -1 ||
        out->loop_total == 0 ||
        (out->loop_total > 0 && out->loop_current > out->loop_total))
        return -1;
    if (strcmp(state->valuestring, "playing") == 0)
        out->state = SCREEN_PLAYBACK_PLAYING;
    else if (strcmp(state->valuestring, "paused") == 0)
        out->state = SCREEN_PLAYBACK_PAUSED;
    else
        return -1;
    memcpy(out->video_name, video->valuestring, strlen(video->valuestring) + 1);
    return 0;
}

int screen_persistence_load(struct screen_registry *reg, const char *path,
                            int *warnings)
{
    if (warnings)
        *warnings = 0;
    if (!reg || !path || !path[0])
        return -1;

    FILE *fp = fopen_utf8_local(path, "rb");
    if (!fp) {
        if (errno == ENOENT)
            return 0;
        return load_reject(path, warnings, "could not open manifest");
    }

    uint64_t manifest_size = 0;
    if (file_size(fp, &manifest_size) != 0 || manifest_size == 0 ||
        manifest_size > SCREEN_MANIFEST_MAX_BYTES ||
        manifest_size > SIZE_MAX - 1) {
        fclose(fp);
        return load_reject(path, warnings, "manifest size out of range");
    }
    size_t size = (size_t)manifest_size;
    char *data = calloc(size + 1, 1);
    if (!data) {
        fclose(fp);
        return load_reject(path, warnings, "out of memory");
    }
    int read_ok = fread(data, 1, size, fp) == size;
    fclose(fp);
    if (!read_ok) {
        free(data);
        return load_reject(path, warnings, "manifest read was incomplete");
    }

    cJSON *root = cJSON_ParseWithLengthOpts(data, size + 1, nullptr, 1);
    free(data);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return load_reject(path, warnings, "not valid JSON");
    }

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "format_version");
    int version_value = 0;
    if (parse_int_json(version, &version_value) != 0 || version_value < 1 ||
        version_value > SCREEN_SAVE_VERSION) {
        cJSON_Delete(root);
        return load_reject(path, warnings, "unsupported format_version");
    }
    cJSON *screens = cJSON_GetObjectItemCaseSensitive(root, "screens");
    if (!cJSON_IsArray(screens)) {
        cJSON_Delete(root);
        return load_reject(path, warnings, "missing screens array");
    }

    struct screen_registry loaded;
    screen_registry_init(&loaded);
    int warn_count = 0;
    FILE *sidecar_fp = nullptr;
    uint64_t sidecar_bytes = 0;
    const char *sidecar_name_value = nullptr;
    char *sidecar_path = nullptr;

    if (version_value == 2) {
        if (get_v2_sidecar_name(root, &sidecar_name_value) != 0) {
            cJSON_Delete(root);
            return load_reject(path, warnings, "invalid sidecar name");
        }

        int has_reference = 0;
        uint64_t referenced_bytes = 0;
        int screen_count = cJSON_GetArraySize(screens);
        if (screen_count > SCREEN_REGISTRY_MAX) {
            cJSON_Delete(root);
            return load_reject(path, warnings, "too many v2 screens");
        }
        for (int i = 0; i < screen_count; i++) {
            cJSON *screen = cJSON_GetArrayItem(screens, i);
            struct screen_geom reference_geom;
            const char *reference_name = nullptr;
            const char *reference_owner = nullptr;
            struct sidecar_ref reference = {0};
            if (parse_screen_metadata(screen, &reference_geom,
                                      &reference_name, &reference_owner) != 0 ||
                parse_v2_ref(screen, &reference) != 0) {
                cJSON_Delete(root);
                return load_reject(path, warnings,
                                   "invalid v2 screen metadata or reference");
            }
            if (!reference.present)
                continue;
            cJSON *managed = cJSON_GetObjectItemCaseSensitive(
                screen, "plugin_managed");
            uint64_t reference_bytes = 0;
            if (!cJSON_IsTrue(managed) || reference.offset != referenced_bytes ||
                reference.count !=
                    (uint64_t)screen_geom_tile_count(&reference_geom) ||
                checked_mul_u64(reference.count, sizeof(int64_t),
                                &reference_bytes) != 0 ||
                referenced_bytes > UINT64_MAX - reference_bytes) {
                cJSON_Delete(root);
                return load_reject(path, warnings,
                                   "non-contiguous v2 map ID reference");
            }
            referenced_bytes += reference_bytes;
            has_reference = 1;
        }
        if (has_reference != (sidecar_name_value != nullptr)) {
            cJSON_Delete(root);
            return load_reject(path, warnings,
                               "v2 sidecar/reference mismatch");
        }
        if (has_reference) {
            if (!sidecar_name_value) {
                cJSON_Delete(root);
                return load_reject(path, warnings,
                                   "managed screen has no sidecar");
            }
            char *directory = nullptr;
            if (path_directory(path, &directory) != 0 ||
                path_join_basename(directory, sidecar_name_value,
                                   &sidecar_path) != 0) {
                free(directory);
                cJSON_Delete(root);
                return load_reject(path, warnings, "sidecar path is invalid");
            }
            free(directory);
            if (open_and_validate_sidecar(sidecar_path, &sidecar_fp,
                                          &sidecar_bytes) != 0) {
                free(sidecar_path);
                cJSON_Delete(root);
                return load_reject(path, warnings,
                                   "sidecar header, length, or checksum invalid");
            }
            if (sidecar_bytes != referenced_bytes) {
                fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                return load_reject(path, warnings,
                                   "v2 sidecar has unreferenced payload");
            }
        }
    }

    int array_size = cJSON_GetArraySize(screens);
    for (int i = 0; i < array_size; i++) {
        cJSON *screen = cJSON_GetArrayItem(screens, i);
        struct sidecar_ref ref = {0};
        if (version_value == 2) {
            if (parse_v2_ref(screen, &ref) != 0) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "invalid v2 map ID reference");
            }
            cJSON *managed = cJSON_GetObjectItemCaseSensitive(
                screen, "plugin_managed");
            if (ref.present && !cJSON_IsTrue(managed)) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "map ID reference is not plugin managed");
            }
        }

        if (loaded.count >= SCREEN_REGISTRY_MAX) {
            warn_count++;
            continue;
        }

        struct screen_geom geom;
        const char *name = nullptr;
        const char *owner = nullptr;
        if (parse_screen_metadata(screen, &geom, &name, &owner) != 0) {
            if (version_value == 2) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "invalid v2 screen metadata");
            }
            warn_count++;
            continue;
        }
        if (screen_registry_find(&loaded, name) >= 0) {
            if (version_value == 2) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "duplicate v2 screen name");
            }
            warn_count++;
            continue;
        }

        int index = -1;
        if (screen_registry_create(&loaded, name, owner, &geom, &index) !=
            SCREEN_OK) {
            if (version_value == 2) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "could not create v2 screen");
            }
            warn_count++;
            continue;
        }
        struct screen_entry *entry = loaded.screens[index];

        if (version_value == 2) {
            if (parse_playback(screen, &entry->playback) != 0) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "invalid v2 playback checkpoint");
            }
            entry->playing = entry->playback.state != SCREEN_PLAYBACK_STOPPED;
        }

        cJSON *created = cJSON_GetObjectItemCaseSensitive(screen, "created_at");
        if (created) {
            int64_t created_at = 0;
            if (parse_i64_json(created, &created_at) != 0) {
                screen_registry_delete(&loaded, name);
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "invalid created_at integer");
            }
            entry->created_at = created_at;
        }

        if (version_value == 1) {
            int entry_warning = 0;
            (void)load_legacy_ids(screen, entry, &entry_warning);
            warn_count += entry_warning;
        } else {
            cJSON *managed = cJSON_GetObjectItemCaseSensitive(
                screen, "plugin_managed");
            entry->plugin_managed = cJSON_IsTrue(managed) ? 1 : 0;
            if (ref.present &&
                read_sidecar_ids(sidecar_fp, sidecar_bytes, &ref, entry) != 0) {
                if (sidecar_fp)
                    fclose(sidecar_fp);
                free(sidecar_path);
                cJSON_Delete(root);
                screen_registry_cleanup(&loaded);
                return load_reject(path, warnings,
                                   "invalid v2 map ID reference range or CRC");
            }
        }
    }

    if (sidecar_fp)
        fclose(sidecar_fp);
    free(sidecar_path);
    cJSON_Delete(root);

    replace_registry(reg, &loaded);
    if (warn_count > 0)
        screen_log("loaded %d screens from %s; skipped %d invalid entries",
                   reg->count, path, warn_count);
    if (warnings)
        *warnings = warn_count;
    return 0;
}

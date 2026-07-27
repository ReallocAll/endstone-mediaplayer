#include "mediaplayer/screen/screen_persistence.h"
#include "cJSON.h"
#include "platform.h"
#include "endstone_abi.h"
#include "abi_helpers.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ES_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif

static void *g_screen_log_plugin = nullptr;

static void screen_log(const char *fmt, ...)
{
    if (!g_screen_log_plugin) {
        return;
    }
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
    wchar_t wpath[512], wmode[16];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16);
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

int screen_persistence_save(const struct screen_registry *reg, const char *path)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddNumberToObject(root, "format_version", SCREEN_SAVE_VERSION);

    cJSON *screens_arr = cJSON_AddArrayToObject(root, "screens");
    if (!screens_arr) { cJSON_Delete(root); return -1; }

    for (int i = 0; i < reg->count; i++) {
        const struct screen_entry *e = &reg->screens[i];
        cJSON *s = cJSON_CreateObject();
        if (!s) continue;

        cJSON_AddStringToObject(s, "name", e->name);
        cJSON_AddStringToObject(s, "owner_uuid", e->owner_uuid);
        cJSON_AddStringToObject(s, "dimension", e->geom.dimension);
        cJSON_AddNumberToObject(s, "facing", (double)e->geom.facing);
        cJSON_AddNumberToObject(s, "width", e->geom.width);
        cJSON_AddNumberToObject(s, "height", e->geom.height);
        cJSON_AddBoolToObject(s, "plugin_managed", e->plugin_managed != 0);

        if (e->plugin_managed) {
            cJSON *map_ids = cJSON_AddArrayToObject(s, "map_ids");
            int tile_count = screen_geom_tile_count(&e->geom);
            for (int tile = 0; tile < tile_count; tile++) {
                char id[32];
                snprintf(id, sizeof(id), "%lld", (long long)e->tiles[tile].map_id);
                cJSON_AddItemToArray(map_ids, cJSON_CreateString(id));
            }
        }

        cJSON *c1 = cJSON_CreateObject();
        cJSON_AddNumberToObject(c1, "x", e->geom.corner1.x);
        cJSON_AddNumberToObject(c1, "y", e->geom.corner1.y);
        cJSON_AddNumberToObject(c1, "z", e->geom.corner1.z);
        cJSON_AddItemToObject(s, "corner1", c1);

        cJSON *c2 = cJSON_CreateObject();
        cJSON_AddNumberToObject(c2, "x", e->geom.corner2.x);
        cJSON_AddNumberToObject(c2, "y", e->geom.corner2.y);
        cJSON_AddNumberToObject(c2, "z", e->geom.corner2.z);
        cJSON_AddItemToObject(s, "corner2", c2);

        cJSON_AddNumberToObject(s, "created_at", (double)e->created_at);
        cJSON_AddItemToArray(screens_arr, s);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    // Write to a temporary file before replacing the destination.
    char tmp_path[560];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen_utf8_local(tmp_path, "w");
    if (!fp) {
        free(json_str);
        return -1;
    }

    size_t len = strlen(json_str);
    if (fwrite(json_str, 1, len, fp) != len) {
        fclose(fp);
        free(json_str);
        remove(tmp_path);
        return -1;
    }
    fclose(fp);
    free(json_str);

#if defined(ES_PLATFORM_WINDOWS)
    if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp_path);
        return -1;
    }
#else
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }
#endif

    return 0;
}

static int parse_pos(cJSON *obj, struct screen_pos *pos)
{
    cJSON *x = cJSON_GetObjectItem(obj, "x");
    cJSON *y = cJSON_GetObjectItem(obj, "y");
    cJSON *z = cJSON_GetObjectItem(obj, "z");
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(z))
        return -1;
    pos->x = (int)x->valuedouble;
    pos->y = (int)y->valuedouble;
    pos->z = (int)z->valuedouble;
    return 0;
}

// Move a file that failed to load aside so a later save cannot destroy it.
static void preserve_bad_file(const char *path)
{
    char bad_path[560];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
#if defined(ES_PLATFORM_WINDOWS)
    int moved = MoveFileExA(path, bad_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int moved = rename(path, bad_path) == 0;
#endif
    if (moved)
        screen_log("preserved unreadable screens file as %s", bad_path);
    else
        screen_log("could not move %s aside; fix or delete it by hand", path);
}

static int load_reject(const char *path, int *warnings, const char *reason)
{
    screen_log("cannot load %s: %s", path, reason);
    preserve_bad_file(path);
    if (warnings) *warnings = 1;
    return -1;
}

int screen_persistence_load(struct screen_registry *reg, const char *path, int *warnings)
{
    int warn_count = 0;
    screen_registry_init(reg);

    FILE *fp = fopen_utf8_local(path, "rb");
    if (!fp) {
        // A missing save file is valid on first start.
        if (warnings) *warnings = 0;
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
        fclose(fp);
        return load_reject(path, warnings, "file size out of range");
    }

    char *data = malloc((size_t)fsize + 1);
    if (!data) {
        fclose(fp);
        return load_reject(path, warnings, "out of memory");
    }

    size_t rd = fread(data, 1, (size_t)fsize, fp);
    fclose(fp);
    data[rd] = '\0';

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root)
        return load_reject(path, warnings, "not valid JSON");

    cJSON *ver = cJSON_GetObjectItem(root, "format_version");
    if (!cJSON_IsNumber(ver)) {
        cJSON_Delete(root);
        return load_reject(path, warnings, "missing format_version");
    }
    if ((int)ver->valuedouble > SCREEN_SAVE_VERSION) {
        cJSON_Delete(root);
        return load_reject(path, warnings, "format_version newer than this plugin");
    }

    cJSON *screens_arr = cJSON_GetObjectItem(root, "screens");
    if (!cJSON_IsArray(screens_arr)) {
        cJSON_Delete(root);
        return load_reject(path, warnings, "missing screens array");
    }

    int arr_size = cJSON_GetArraySize(screens_arr);
    int i = 0;
    for (; i < arr_size && reg->count < SCREEN_REGISTRY_MAX; i++) {
        cJSON *s = cJSON_GetArrayItem(screens_arr, i);
        if (!cJSON_IsObject(s)) { warn_count++; continue; }

        cJSON *jname = cJSON_GetObjectItem(s, "name");
        if (!cJSON_IsString(jname) || !screen_name_valid(jname->valuestring)) {
            warn_count++;
            continue;
        }

        if (screen_registry_find(reg, jname->valuestring) >= 0) {
            warn_count++;
            continue;
        }

        struct screen_geom geom;
        memset(&geom, 0, sizeof(geom));

        cJSON *jdim = cJSON_GetObjectItem(s, "dimension");
        if (cJSON_IsString(jdim)) {
            size_t dl = strlen(jdim->valuestring);
            if (dl >= sizeof(geom.dimension)) dl = sizeof(geom.dimension) - 1;
            memcpy(geom.dimension, jdim->valuestring, dl);
            geom.dimension[dl] = '\0';
        }

        // Reject facings outside the geometry lookup table.
        cJSON *jfacing = cJSON_GetObjectItem(s, "facing");
        if (!cJSON_IsNumber(jfacing)) {
            warn_count++;
            continue;
        }
        int facing_value = (int)jfacing->valuedouble;
        if (facing_value < SCREEN_FACE_SOUTH ||
            facing_value > SCREEN_FACE_WEST) {
            warn_count++;
            continue;
        }
        geom.facing = (enum screen_facing)facing_value;

        cJSON *jw = cJSON_GetObjectItem(s, "width");
        cJSON *jh = cJSON_GetObjectItem(s, "height");
        if (cJSON_IsNumber(jw)) geom.width = (int)jw->valuedouble;
        if (cJSON_IsNumber(jh)) geom.height = (int)jh->valuedouble;

        if (geom.width < 1 || geom.width > SCREEN_MAX_WIDTH ||
            geom.height < 1 || geom.height > SCREEN_MAX_HEIGHT) {
            warn_count++;
            continue;
        }

        cJSON *jc1 = cJSON_GetObjectItem(s, "corner1");
        cJSON *jc2 = cJSON_GetObjectItem(s, "corner2");
        if (!cJSON_IsObject(jc1) || !cJSON_IsObject(jc2) ||
            parse_pos(jc1, &geom.corner1) != 0 ||
            parse_pos(jc2, &geom.corner2) != 0) {
            warn_count++;
            continue;
        }

        int idx = 0;
        cJSON *jowner = cJSON_GetObjectItem(s, "owner_uuid");
        const char *owner = cJSON_IsString(jowner) ? jowner->valuestring : "";

        enum screen_error err = screen_registry_create(reg, jname->valuestring, owner, &geom, &idx);
        if (err != SCREEN_OK) {
            warn_count++;
            continue;
        }

        // Unknown fields are ignored.
        cJSON *jcreated = cJSON_GetObjectItem(s, "created_at");
        if (cJSON_IsNumber(jcreated))
            reg->screens[idx].created_at = (int64_t)jcreated->valuedouble;

        cJSON *jmanaged = cJSON_GetObjectItem(s, "plugin_managed");
        cJSON *jmap_ids = cJSON_GetObjectItem(s, "map_ids");
        if (cJSON_IsTrue(jmanaged) && cJSON_IsArray(jmap_ids) &&
            cJSON_GetArraySize(jmap_ids) == screen_geom_tile_count(&geom)) {
            int ids_ok = 1;
            for (int tile = 0; tile < screen_geom_tile_count(&geom); tile++) {
                cJSON *jid = cJSON_GetArrayItem(jmap_ids, tile);
                if (!cJSON_IsString(jid) || !jid->valuestring[0]) {
                    ids_ok = 0;
                    break;
                }
                char *end = nullptr;
                long long id = strtoll(jid->valuestring, &end, 10);
                if (!end || *end != '\0') {
                    ids_ok = 0;
                    break;
                }
                reg->screens[idx].tiles[tile].map_id = (int64_t)id;
                reg->screens[idx].tiles[tile].map_id_valid = 1;
            }
            if (ids_ok) {
                reg->screens[idx].plugin_managed = 1;
            } else {
                for (int tile = 0; tile < screen_geom_tile_count(&geom); tile++) {
                    reg->screens[idx].tiles[tile].map_id = -1;
                    reg->screens[idx].tiles[tile].map_id_valid = 0;
                }
                warn_count++;
            }
        }
    }

    warn_count += arr_size - i; // entries beyond registry capacity

    cJSON_Delete(root);
    if (warn_count > 0)
        screen_log("loaded %d screens from %s; skipped %d invalid entries",
                   reg->count, path, warn_count);
    if (warnings) *warnings = warn_count;
    return 0;
}

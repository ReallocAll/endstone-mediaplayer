// Focused tests for the production UTF-8 file-opening helper.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mediaplayer/endstone_api.h"

void *g_plugin = nullptr;

static int test_valid_utf8_path(void)
{
    const char *path = "test_fopen_utf8_\xC3\xA9.tmp";
    FILE *file = fopen_utf8(path, "wb");
    if (file == nullptr) return 0;
    fclose(file);
#if defined(_WIN32)
    if (_wremove(L"test_fopen_utf8_\u00e9.tmp") != 0) return 0;
#else
    remove(path);
#endif
    return 1;
}

static int test_null_arguments(void)
{
    return fopen_utf8(nullptr, "rb") == nullptr &&
           fopen_utf8("test_fopen_utf8.tmp", nullptr) == nullptr;
}

#if defined(_WIN32)
static int test_invalid_utf8_is_rejected(void)
{
    const char invalid_utf8[] = "test_fopen_utf8_\xC3\x28.tmp";
    const unsigned char invalid_mode[] = {'r', 0xFF, 'b', '\0'};
    return fopen_utf8(invalid_utf8, "rb") == nullptr &&
           fopen_utf8("test_fopen_utf8.tmp", (const char *)invalid_mode) == nullptr;
}

static int test_fixed_buffers_are_not_truncated(void)
{
    char long_path[ENDSTONE_MEDIAPLAYER_PATH_MAX + 2];
    char long_mode[10];

    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    memset(long_mode, 'b', sizeof(long_mode) - 1);
    long_mode[sizeof(long_mode) - 1] = '\0';
    return fopen_utf8(long_path, "rb") == nullptr &&
           fopen_utf8("test_fopen_utf8.tmp", long_mode) == nullptr;
}
#endif

int main(void)
{
    if (!test_null_arguments() || !test_valid_utf8_path()) return 1;
#if defined(_WIN32)
    if (!test_invalid_utf8_is_rejected() ||
        !test_fixed_buffers_are_not_truncated()) return 1;
#endif
    return 0;
}

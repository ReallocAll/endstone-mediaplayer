#include "cppcompat/string.h"
#include "common/common.h"

/**
 * MSVC std::string ABI implementation.
 *
 * Memory layout (x64):
 *   [0-15]  char _Buf[16] | char *_Ptr   (union)
 *   [16-23] size_t _Mysize
 *   [24-31] size_t _Myres  (15 for SSO)
 *
 * Heap strings use plain malloc (< 4096 bytes) to match MSVC's allocator.
 */

static void string_construct_impl(char *obj, const char *str)
{
    size_t len = strlen(str);
    if (len > CPP_STRING_MAX) len = CPP_STRING_MAX;

    memset(obj, 0, CPP_STRING_SIZE);
    *(size_t *)(obj + 24) = CPP_STRING_SSO_CAP;

    if (len <= CPP_STRING_SSO_CAP) {
        memcpy(obj, str, len + 1);
        *(size_t *)(obj + 16) = len;
    } else {
        size_t cap = len | 0xF;
        if (cap < 22) cap = 22;

        char *buf = (char *)cpp_alloc(cap + 1);
        if (!buf) {
            memcpy(obj, str, CPP_STRING_SSO_CAP + 1);
            *(size_t *)(obj + 16) = CPP_STRING_SSO_CAP;
            return;
        }
        memcpy(buf, str, len + 1);
        *(char **)(obj) = buf;
        *(size_t *)(obj + 16) = len;
        *(size_t *)(obj + 24) = cap;
    }
}

void cpp_string_construct(void *dst, const char *str)
{
    if (!dst) return;
    string_construct_impl((char *)dst, str ? str : "");
}

void cpp_string_destroy(void *s)
{
    if (!s) return;
    char *obj = (char *)s;
    if (*(size_t *)(obj + 24) > CPP_STRING_SSO_CAP) {
        char *buf = *(char **)obj;
        if (buf) cpp_dealloc(buf);
    }
}

void *cpp_string_new(const char *str)
{
    char *obj = (char *)cpp_alloc(CPP_STRING_SIZE);
    if (!obj) return NULL;
    string_construct_impl(obj, str ? str : "");
    return obj;
}

void cpp_string_delete(void *s)
{
    if (!s) return;
    cpp_string_destroy(s);
    cpp_dealloc(s);
}

void cpp_string_copy(void *dst, const void *src)
{
    if (!dst || !src) return;
    cpp_string_construct(dst, cpp_string_str(src));
}

void cpp_string_move(void *dst, void *src)
{
    if (!dst || !src) return;
    memcpy(dst, src, CPP_STRING_SIZE);
    /* Reset src to empty SSO state */
    memset(src, 0, CPP_STRING_SIZE);
    *(size_t *)((char *)src + 24) = CPP_STRING_SSO_CAP;
}

const char *cpp_string_str(const void *s)
{
    if (!s) return "";
    const char *obj = (const char *)s;
    return *(const size_t *)(obj + 24) <= CPP_STRING_SSO_CAP
        ? obj
        : *(const char *const *)obj;
}

size_t cpp_string_size(const void *s)
{
    if (!s) return 0;
    return *(const size_t *)((const char *)s + 16);
}

#include <cppcompat/string.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *cppcompat_alloc(size_t size);
extern void cppcompat_free(void *ptr);

static int is_long(const unsigned char *str)
{
    return (str[0] & 1U) != 0;
}

size_t cpp_string_size(const void *str)
{
    const unsigned char *obj = (const unsigned char *)str;
    if (!obj) return 0;
    return is_long(obj) ? *(const size_t *)(obj + 8) : (size_t)(obj[0] >> 1);
}

const char *cpp_string_str(const void *str)
{
    const unsigned char *obj = (const unsigned char *)str;
    if (!obj) return "";
    return is_long(obj) ? *(const char *const *)(obj + 16) : (const char *)(obj + 1);
}

void cpp_string_construct(void *dst, const char *value)
{
    unsigned char *obj = (unsigned char *)dst;
    size_t length;
    if (!obj) return;
    if (!value) value = "";
    length = strlen(value);
    if (length > CPP_STRING_MAX) length = CPP_STRING_MAX;
    memset(obj, 0, CPP_STRING_SIZE);
    if (length <= CPP_STRING_SSO_CAP) {
        obj[0] = (unsigned char)(length << 1);
        memcpy(obj + 1, value, length);
        obj[length + 1] = 0;
        return;
    }

    size_t allocation = (length + 8U) & ~(size_t)7U;
    if (allocation < 26U) allocation = 26U;
    char *buffer = (char *)cppcompat_alloc(allocation);
    if (!buffer) return;
    memcpy(buffer, value, length);
    buffer[length] = 0;
    *(size_t *)(obj + 0) = allocation | 1U;
    *(size_t *)(obj + 8) = length;
    *(char **)(obj + 16) = buffer;
}

void cpp_string_destroy(void *str)
{
    unsigned char *obj = (unsigned char *)str;
    if (!obj) return;
    if (is_long(obj)) cppcompat_free(*(void **)(obj + 16));
    memset(obj, 0, CPP_STRING_SIZE);
}

void cpp_string_copy(void *dst, const void *src)
{
    cpp_string_construct(dst, cpp_string_str(src));
}

void cpp_string_move(void *dst, void *src)
{
    if (!dst || !src || dst == src) return;
    memcpy(dst, src, CPP_STRING_SIZE);
    memset(src, 0, CPP_STRING_SIZE);
}

void *cpp_string_new(const char *value)
{
    void *obj = cppcompat_alloc(CPP_STRING_SIZE);
    if (obj) cpp_string_construct(obj, value);
    return obj;
}

void cpp_string_delete(void *str)
{
    if (!str) return;
    cpp_string_destroy(str);
    cppcompat_free(str);
}

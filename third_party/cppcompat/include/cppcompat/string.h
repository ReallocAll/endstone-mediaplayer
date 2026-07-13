#ifndef CPPCOMPAT_STRING_H
#define CPPCOMPAT_STRING_H
#include <stddef.h>
#include "abi.h"
void cpp_string_construct(void *dst, const char *str);
void cpp_string_destroy(void *str);
void *cpp_string_new(const char *str);
void cpp_string_delete(void *str);
void cpp_string_copy(void *dst, const void *src);
void cpp_string_move(void *dst, void *src);
const char *cpp_string_str(const void *str);
size_t cpp_string_size(const void *str);
#endif

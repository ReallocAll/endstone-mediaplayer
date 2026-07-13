#ifndef CPPCOMPAT_VECTOR_H
#define CPPCOMPAT_VECTOR_H
#include <stddef.h>
#include "abi.h"
void cpp_vector_construct(void *dst, size_t elem_size,
                          void (*elem_destroy)(void *elem));
void cpp_vector_construct_with(void *dst, size_t elem_size,
                               const void *data, size_t count,
                               void (*elem_destroy)(void *elem));
void cpp_vector_construct_borrowed(void *dst, void *data, size_t count,
                                   size_t elem_size);
void cpp_vector_destroy(void *vector);
void *cpp_vector_new(size_t elem_size, void (*elem_destroy)(void *elem));
void cpp_vector_delete(void *vector);
void *cpp_vector_data(const void *vector);
size_t cpp_vector_size(const void *vector, size_t elem_size);
#endif

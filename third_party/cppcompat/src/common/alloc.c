#include <stddef.h>
#include <stdlib.h>

void *cppcompat_alloc(size_t size)
{
    return malloc(size);
}

void cppcompat_free(void *ptr)
{
    free(ptr);
}

#include <cppcompat/vector.h>
#include <stdlib.h>
#include <string.h>

extern void *cppcompat_alloc(size_t size);
extern void cppcompat_free(void *ptr);

struct vector_owner {
    void *vector;
    void *allocation;
    size_t elem_size;
    void (*elem_destroy)(void *);
    struct vector_owner *next;
};

static struct vector_owner *owners;

static int owner_add(void *vector, void *allocation, size_t elem_size,
                     void (*elem_destroy)(void *))
{
    struct vector_owner *owner = cppcompat_alloc(sizeof(*owner));
    if (!owner) return 0;
    *owner = (struct vector_owner){
        vector, allocation, elem_size, elem_destroy, owners
    };
    owners = owner;
    return 1;
}

static struct vector_owner *owner_take(const void *vector)
{
    struct vector_owner **link = &owners;
    while (*link) {
        if ((*link)->vector == vector) {
            struct vector_owner *owner = *link;
            *link = owner->next;
            return owner;
        }
        link = &(*link)->next;
    }
    return NULL;
}

void cpp_vector_construct(void *dst, size_t elem_size,
                          void (*elem_destroy)(void *))
{
    (void)elem_size;
    (void)elem_destroy;
    if (dst) memset(dst, 0, CPP_VECTOR_SIZE);
}

void cpp_vector_construct_borrowed(void *dst, void *data, size_t count,
                                   size_t elem_size)
{
    void **ptrs = (void **)dst;
    if (!dst) return;
    ptrs[0] = data;
    ptrs[1] = (char *)data + count * elem_size;
    ptrs[2] = ptrs[1];
}

void cpp_vector_construct_with(void *dst, size_t elem_size,
                               const void *data, size_t count,
                               void (*elem_destroy)(void *))
{
    size_t bytes;
    void *allocation;
    cpp_vector_construct(dst, elem_size, elem_destroy);
    if (!dst || !data || !count || !elem_size) return;
    bytes = count * elem_size;
    allocation = cppcompat_alloc(bytes);
    if (!allocation) return;
    memcpy(allocation, data, bytes);
    if (!owner_add(dst, allocation, elem_size, elem_destroy)) {
        cppcompat_free(allocation);
        return;
    }
    cpp_vector_construct_borrowed(dst, allocation, count, elem_size);
}

void cpp_vector_destroy(void *vector)
{
    struct vector_owner *owner;
    char *current;
    char *end;
    if (!vector) return;
    owner = owner_take(vector);
    if (owner) {
        current = (char *)((void **)vector)[0];
        end = (char *)((void **)vector)[1];
        if (owner->elem_destroy)
            for (; current < end; current += owner->elem_size)
                owner->elem_destroy(current);
        cppcompat_free(owner->allocation);
        cppcompat_free(owner);
    }
    memset(vector, 0, CPP_VECTOR_SIZE);
}

void *cpp_vector_new(size_t elem_size, void (*elem_destroy)(void *))
{
    void *vector = cppcompat_alloc(CPP_VECTOR_SIZE);
    if (vector) cpp_vector_construct(vector, elem_size, elem_destroy);
    return vector;
}

void cpp_vector_delete(void *vector)
{
    if (!vector) return;
    cpp_vector_destroy(vector);
    cppcompat_free(vector);
}

void *cpp_vector_data(const void *vector)
{
    return vector ? *(void *const *)vector : NULL;
}

size_t cpp_vector_size(const void *vector, size_t elem_size)
{
    char *const *ptrs = (char *const *)vector;
    if (!vector || !elem_size || !ptrs[0]) return 0;
    return (size_t)(ptrs[1] - ptrs[0]) / elem_size;
}

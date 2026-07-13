#include "abi_helpers.h"
#include <stdlib.h>
#include <string.h>

#if ES_PLATFORM_LINUX

static void function_complete_destructor(void *self) { (void)self; }
static void function_deleting_destructor(void *self) { free(self); }

static void *function_clone_heap(const void *self)
{
    void *copy = malloc(24);
    if (copy) memcpy(copy, self, 24);
    return copy;
}

static void function_clone_inline(const void *self, void *destination)
{
    memcpy(destination, self, 24);
}

static void function_destroy(void *self) { (void)self; }
static void function_destroy_deallocate(void *self) { free(self); }

static void function_invoke_void(void *self)
{
    void (*handler)(void) = *(void (**)(void))((char *)self + 8);
    if (handler) handler();
}

static void function_invoke_event(void *self, void *event)
{
    void (*handler)(void *) = *(void (**)(void *))((char *)self + 8);
    if (handler) handler(event);
}

static void *function_target(void *self, const void *type)
{
    (void)self;
    (void)type;
    return NULL;
}

static const void *function_target_type(void *self)
{
    (void)self;
    return NULL;
}

/* Itanium vtable: two prefix words followed by D1, D0, clone, clone_into,
 * destroy, destroy_deallocate, invoke, target and target_type. */
static void *void_vtable[] = {
    NULL, NULL,
    (void *)function_complete_destructor,
    (void *)function_deleting_destructor,
    (void *)function_clone_heap,
    (void *)function_clone_inline,
    (void *)function_destroy,
    (void *)function_destroy_deallocate,
    (void *)function_invoke_void,
    (void *)function_target,
    (void *)function_target_type
};

static void *event_vtable[] = {
    NULL, NULL,
    (void *)function_complete_destructor,
    (void *)function_deleting_destructor,
    (void *)function_clone_heap,
    (void *)function_clone_inline,
    (void *)function_destroy,
    (void *)function_destroy_deallocate,
    (void *)function_invoke_event,
    (void *)function_target,
    (void *)function_target_type
};

#define SFUNC_POOL_SIZE 8
static func_impl_t descriptors[SFUNC_POOL_SIZE];
static unsigned descriptor_count;

func_impl_t *sfunc_alloc(void *handler, bool is_void)
{
    func_impl_t *descriptor;
    if (descriptor_count >= SFUNC_POOL_SIZE) return NULL;
    descriptor = &descriptors[descriptor_count++];
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->vptr = is_void ? &void_vtable[2] : &event_vtable[2];
    descriptor->func = handler;
    descriptor->is_void = is_void;
    return descriptor;
}

void sfunc_build(void *buffer, const func_impl_t *descriptor)
{
    memset(buffer, 0, ES_STD_FUNCTION_SIZE);
    *(void **)((char *)buffer + 0) = descriptor->vptr;
    *(void **)((char *)buffer + 8) = descriptor->func;
    *(void **)((char *)buffer + 16) = descriptor->instance;
    *(void **)((char *)buffer + 32) = buffer;
}

#else

static void *sfunc_copy(void *self, void *dst)
{
    memcpy(dst, self, 24);
    return dst;
}
static void *sfunc_target_type(void *self) { (void)self; return NULL; }
static void *sfunc_get(void *self) { (void)self; return NULL; }
static void sfunc_delete_this(void *self, int deleting) { (void)self; (void)deleting; }
static void event_trampoline(void *instance, void *event)
{
    ((void (*)(void *))instance)(event);
}
static void event_call(void *self, void *event)
{
    void (*fn)(void *, void *) = *(void (**)(void *, void *))((char *)self + 8);
    fn(*(void **)((char *)self + 16), event);
}
static void void_trampoline(void *instance) { ((void (*)(void))instance)(); }
static void void_call(void *self)
{
    void (*fn)(void *) = *(void (**)(void *))((char *)self + 8);
    fn(*(void **)((char *)self + 16));
}
static void *event_vtable[6] = {sfunc_copy, sfunc_copy, event_call,
    sfunc_target_type, sfunc_delete_this, sfunc_get};
static void *void_vtable[6] = {sfunc_copy, sfunc_copy, void_call,
    sfunc_target_type, sfunc_delete_this, sfunc_get};
static func_impl_t descriptors[8];
static unsigned descriptor_count;

func_impl_t *sfunc_alloc(void *handler, bool is_void)
{
    func_impl_t *descriptor;
    if (descriptor_count >= 8) return NULL;
    descriptor = &descriptors[descriptor_count++];
    descriptor->vptr = is_void ? void_vtable : event_vtable;
    descriptor->func = is_void ? (void *)void_trampoline : (void *)event_trampoline;
    descriptor->instance = handler;
    descriptor->is_void = is_void;
    return descriptor;
}

void sfunc_build(void *buffer, const func_impl_t *descriptor)
{
    memset(buffer, 0, ES_STD_FUNCTION_SIZE);
    memcpy(buffer, descriptor, 3 * sizeof(void *));
    *(void **)((char *)buffer + 0x30) = buffer;
    *(void **)((char *)buffer + 0x38) = buffer;
}

#endif

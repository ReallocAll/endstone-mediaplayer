#ifndef ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H
#define ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H

#include "endstone_abi.h"
#include <cppcompat/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VTABLE(obj) (*(void ***)(obj))
#define VCALL0(obj, slot, ret) ((ret (*)(void *))VTABLE(obj)[slot])(obj)
#define VCALL1(obj, slot, ret, T1, a1) \
    ((ret (*)(void *, T1))VTABLE(obj)[slot])((obj), (a1))
#define VCALL2(obj, slot, ret, T1, a1, T2, a2) \
    ((ret (*)(void *, T1, T2))VTABLE(obj)[slot])((obj), (a1), (a2))
#define VCALL3(obj, slot, ret, T1, a1, T2, a2, T3, a3) \
    ((ret (*)(void *, T1, T2, T3))VTABLE(obj)[slot])((obj), (a1), (a2), (a3))
#define VCALL4(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4) \
    ((ret (*)(void *, T1, T2, T3, T4))VTABLE(obj)[slot])((obj), (a1), (a2), (a3), (a4))

#if ES_PLATFORM_LINUX
static inline void es_shared_ptr_release(void *storage)
{
    void *control = ((void **)storage)[1];
    if (!control) return;
    long previous = __atomic_fetch_sub((long *)((char *)control + 8), 1,
                                       __ATOMIC_ACQ_REL);
    if (previous == 0) {
        void **vtable = *(void ***)control;
        ((void (*)(void *))vtable[2])(control);
        previous = __atomic_fetch_sub((long *)((char *)control + 16), 1,
                                      __ATOMIC_ACQ_REL);
        if (previous == 0)
            ((void (*)(void *))vtable[4])(control);
    }
    ((void **)storage)[0] = NULL;
    ((void **)storage)[1] = NULL;
}

static inline void *es_vcall5_linux(void *obj, size_t slot,
                                    uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                    uintptr_t a4, uintptr_t a5)
{
    void *fn = VTABLE(obj)[slot];
    if (slot == ES_SCHEDULER_SLOT_RUN_TIMER) {
        ((void (*)(void *, void *, void *, void *, uint64_t, uint64_t))fn)(
            (void *)a1, obj, (void *)a2, (void *)a3, (uint64_t)a4, (uint64_t)a5);
        es_shared_ptr_release((void *)a1);
        return (void *)a1;
    }
    ((void (*)(void *, void *, void *, int, void *, bool))fn)(
        obj, (void *)a1, (void *)a2, (int)a3, (void *)a4, (bool)a5);
    return NULL;
}
#define VCALL5(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5) \
    ((ret)es_vcall5_linux((obj), (slot), (uintptr_t)(a1), (uintptr_t)(a2), \
                          (uintptr_t)(a3), (uintptr_t)(a4), (uintptr_t)(a5)))
#else
#define VCALL5(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5) \
    ((ret (*)(void *, T1, T2, T3, T4, T5))VTABLE(obj)[slot])((obj), (a1), (a2), (a3), (a4), (a5))
#endif

#define MC_AQUA "\xc2\xa7" "b"
#define MC_RED "\xc2\xa7" "c"
#define MC_GOLD "\xc2\xa7" "6"
#define MC_GREEN "\xc2\xa7" "a"
#define MC_YELLOW "\xc2\xa7" "e"
#define MC_GRAY "\xc2\xa7" "7"

#define VEC_INIT(vec, data, count, elem_size) do { \
    void **_v = (void **)(vec); \
    _v[0] = (data); \
    _v[1] = (char *)(data) + (count) * (elem_size); \
    _v[2] = (char *)(data) + (count) * (elem_size); \
} while (0)

typedef struct func_impl {
    void **vptr;
    void *func;
    void *instance;
    bool is_void;
} func_impl_t;

func_impl_t *sfunc_alloc(void *handler, bool is_void);
void sfunc_build(void *buffer, const func_impl_t *impl);
#define SFUNC_BUILD(buf, impl) sfunc_build((buf), (impl))

#if ES_PLATFORM_LINUX
#define STR_GUARD(_str, _call) do { (_call); memset((_str), 0, ES_STRING_SIZE); } while (0)
#else
static inline bool es_string_is_heap(const void *s)
{
    return *(const size_t *)((const char *)s + 24) > 15;
}
#define STR_GUARD(_str, _call) do { \
    bool _heap = es_string_is_heap(_str); \
    (_call); \
    if (_heap) memset((_str), 0, ES_STRING_SIZE); \
    else cpp_string_destroy((_str)); \
} while (0)
#endif

#if ES_PLATFORM_LINUX
struct es_string_view { const char *data; size_t size; };
#define PLUGIN_LOG(self, level, msg) do { \
    void *_logger = *(void **)((char *)(self) + ES_PLUGIN_OFF_LOGGER); \
    if (_logger) { \
        struct es_string_view _view = {(msg), strlen(msg)}; \
        ((void (*)(void *, unsigned char, struct es_string_view)) \
            VTABLE(_logger)[ES_LOGGER_SLOT_LOG])(_logger, (unsigned char)(level), _view); \
    } \
} while (0)
#else
#define PLUGIN_LOG(self, level, msg) do { \
    void *_logger = *(void **)((char *)(self) + ES_PLUGIN_OFF_LOGGER); \
    if (_logger) { \
        struct { const char *data; size_t size; } _view = {(msg), strlen(msg)}; \
        VCALL2(_logger, ES_LOGGER_SLOT_LOG, void, unsigned char, \
               (unsigned char)(level), const void *, &_view); \
    } \
} while (0)
#endif

#define PLUGIN_SERVER(self) (*(void **)((char *)(self) + ES_PLUGIN_OFF_SERVER))

#endif

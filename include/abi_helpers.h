#ifndef ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H
#define ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H

#include "endstone_abi.h"
#include <cppcompat/string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ES_PLATFORM_WINDOWS
#include <intrin.h>
#endif

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

static inline void *es_pointer_at(const void *storage, size_t offset)
{
    void *value = nullptr;
    memcpy(&value, (const unsigned char *)storage + offset, sizeof(value));
    return value;
}

static inline void es_store_pointer(void *storage, size_t offset, void *value)
{
    memcpy((unsigned char *)storage + offset, &value, sizeof(value));
}

static inline int64_t es_atomic_fetch_add(void *field, int64_t delta)
{
#if ES_PLATFORM_WINDOWS
    if (ES_REFCOUNT_COUNTER_SIZE == sizeof(long)) {
        return (int64_t)_InterlockedExchangeAdd(
            (volatile long *)field, (long)delta);
    }
    return (int64_t)_InterlockedExchangeAdd64(
        (volatile __int64 *)field, (__int64)delta);
#else
    if (ES_REFCOUNT_COUNTER_SIZE == sizeof(int32_t)) {
        return (int64_t)__atomic_fetch_add(
            (int32_t *)field, (int32_t)delta, __ATOMIC_ACQ_REL);
    }
    return __atomic_fetch_add(
        (int64_t *)field, delta, __ATOMIC_ACQ_REL);
#endif
}

static inline void es_counter_store(void *field, int64_t value)
{
    if (ES_REFCOUNT_COUNTER_SIZE == sizeof(int32_t)) {
        int32_t narrowed = (int32_t)value;
        memcpy(field, &narrowed, sizeof(narrowed));
    }
    else {
        memcpy(field, &value, sizeof(value));
    }
}

static inline int64_t es_counter_load(const void *field)
{
    if (ES_REFCOUNT_COUNTER_SIZE == sizeof(int32_t)) {
        int32_t value = 0;
        memcpy(&value, field, sizeof(value));
        return value;
    }
    int64_t value = 0;
    memcpy(&value, field, sizeof(value));
    return value;
}

static inline void *es_shared_object(const void *storage)
{
    return es_pointer_at(storage, ES_SHARED_PTR_OFF_OBJECT);
}

static inline void *es_shared_control(const void *storage)
{
    return es_pointer_at(storage, ES_SHARED_PTR_OFF_CONTROL);
}

static inline void es_shared_init(void *storage, void *object, void *control)
{
    memset(storage, 0, ES_SHARED_PTR_SIZE);
    es_store_pointer(storage, ES_SHARED_PTR_OFF_OBJECT, object);
    es_store_pointer(storage, ES_SHARED_PTR_OFF_CONTROL, control);
}

static inline void es_shared_add_ref(void *storage)
{
    void *control = es_shared_control(storage);
    if (control) {
        (void)es_atomic_fetch_add(
            (unsigned char *)control + ES_REFCOUNT_OFF_USES, 1);
    }
}

static inline void es_shared_copy(void *destination, const void *source)
{
    memcpy(destination, source, ES_SHARED_PTR_SIZE);
    es_shared_add_ref(destination);
}

static inline void es_shared_release(void *storage)
{
    void *control = es_shared_control(storage);
    if (control) {
        int64_t previous = es_atomic_fetch_add(
            (unsigned char *)control + ES_REFCOUNT_OFF_USES, -1);
        if (previous == ES_REFCOUNT_ONE_OWNER_VALUE) {
            void **vtable = *(void ***)control;
            ((void (*)(void *))vtable[
                ES_REFCOUNT_SLOT_DESTROY_RESOURCE])(control);
            previous = es_atomic_fetch_add(
                (unsigned char *)control + ES_REFCOUNT_OFF_WEAKS, -1);
            if (previous == ES_REFCOUNT_IMPLICIT_WEAK_VALUE) {
                ((void (*)(void *))vtable[
                    ES_REFCOUNT_SLOT_DELETE_THIS])(control);
            }
        }
    }
    es_store_pointer(storage, ES_SHARED_PTR_OFF_OBJECT, nullptr);
    es_store_pointer(storage, ES_SHARED_PTR_OFF_CONTROL, nullptr);
}

static inline void es_weak_release(void *storage)
{
    void *control = es_pointer_at(storage, ES_WEAK_PTR_OFF_CONTROL);
    if (control) {
        int64_t previous = es_atomic_fetch_add(
            (unsigned char *)control + ES_REFCOUNT_OFF_WEAKS, -1);
        if (previous == ES_REFCOUNT_IMPLICIT_WEAK_VALUE) {
            void **vtable = *(void ***)control;
            ((void (*)(void *))vtable[
                ES_REFCOUNT_SLOT_DELETE_THIS])(control);
        }
    }
    es_store_pointer(storage, ES_WEAK_PTR_OFF_OBJECT, nullptr);
    es_store_pointer(storage, ES_WEAK_PTR_OFF_CONTROL, nullptr);
}

static inline void es_location_release(struct es_location *location)
{
    if (location) {
        es_weak_release(location->bytes + ES_LOCATION_OFF_DIMENSION);
    }
}

static inline float es_location_float(const struct es_location *location,
                                      size_t offset)
{
    float value = 0.0f;
    memcpy(&value, location->bytes + offset, sizeof(value));
    return value;
}

static inline const char *es_string_view_data(const void *storage)
{
    return es_pointer_at(storage, ES_STRING_VIEW_OFF_DATA);
}

static inline size_t es_string_view_size(const void *storage)
{
    size_t value = 0;
    memcpy(&value, (const unsigned char *)storage + ES_STRING_VIEW_OFF_SIZE,
           sizeof(value));
    return value;
}

static inline void es_string_view_init(void *storage, const char *data,
                                       size_t size)
{
    memset(storage, 0, ES_STRING_VIEW_SIZE);
    es_store_pointer(storage, ES_STRING_VIEW_OFF_DATA, (void *)data);
    memcpy((unsigned char *)storage + ES_STRING_VIEW_OFF_SIZE, &size,
           sizeof(size));
}

static inline void es_identifier_init(struct es_identifier *identifier,
                                      const char *text)
{
    static const char default_namespace[] = "minecraft";
    const char *namespace_data = default_namespace;
    size_t namespace_size = sizeof(default_namespace) - 1;
    const char *key_data = text ? text : "";
    size_t key_size = strlen(key_data);
    if (text) {
        const char *separator = strrchr(text, ':');
        if (separator) {
            namespace_data = text;
            namespace_size = (size_t)(separator - text);
            key_data = separator + 1;
            key_size = strlen(key_data);
        }
    }
    memset(identifier, 0, sizeof(*identifier));
    es_string_view_init(identifier->bytes + ES_IDENTIFIER_OFF_NAMESPACE,
                        namespace_data, namespace_size);
    es_string_view_init(identifier->bytes + ES_IDENTIFIER_OFF_KEY,
                        key_data, key_size);
}

static inline bool es_identifier_copy_text(const struct es_identifier *identifier,
                                           char *output, size_t output_size)
{
    const void *namespace_view =
        identifier->bytes + ES_IDENTIFIER_OFF_NAMESPACE;
    const void *key_view = identifier->bytes + ES_IDENTIFIER_OFF_KEY;
    const char *namespace_data = es_string_view_data(namespace_view);
    const char *key_data = es_string_view_data(key_view);
    size_t namespace_size = es_string_view_size(namespace_view);
    size_t key_size = es_string_view_size(key_view);
    size_t total = namespace_size + (key_size ? 1 : 0) + key_size;
    if (!output || output_size == 0 || total >= output_size ||
        (namespace_size && !namespace_data) || (key_size && !key_data)) {
        if (output && output_size) output[0] = '\0';
        return false;
    }
    if (namespace_size) memcpy(output, namespace_data, namespace_size);
    size_t cursor = namespace_size;
    if (key_size) output[cursor++] = ':';
    if (key_size) memcpy(output + cursor, key_data, key_size);
    output[total] = '\0';
    return true;
}

#define VCALL5(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5) \
    ((ret (*)(void *, T1, T2, T3, T4, T5))VTABLE(obj)[slot])((obj), (a1), (a2), (a3), (a4), (a5))

#define MC_AQUA "\xc2\xa7" "b"
#define MC_RED "\xc2\xa7" "c"
#define MC_GOLD "\xc2\xa7" "6"
#define MC_GREEN "\xc2\xa7" "a"
#define MC_YELLOW "\xc2\xa7" "e"
#define MC_GRAY "\xc2\xa7" "7"

#define VEC_INIT(vec, data, count, elem_size) do { \
    es_store_pointer((vec), ES_VECTOR_OFF_BEGIN, (data)); \
    es_store_pointer((vec), ES_VECTOR_OFF_END, \
        (char *)(data) + (count) * (elem_size)); \
    es_store_pointer((vec), ES_VECTOR_OFF_CAPACITY, \
        (char *)(data) + (count) * (elem_size)); \
} while (0)

// sfunc_build copies exactly the leading three pointers into the
// std::function buffer, so no field may be added before `instance`.
struct func_impl {
    void **vptr;
    void *func;
    void *instance;
};

struct func_impl *sfunc_alloc(void *handler, bool is_void);
void sfunc_build(void *buffer, const struct func_impl *impl);
void sfunc_after_by_value(void *buffer);
#define SFUNC_BUILD(buf, impl) sfunc_build((buf), (impl))

static inline bool es_string_is_heap(const void *s)
{
#if ES_PLATFORM_WINDOWS
    return *(const size_t *)((const char *)s + 24) > 15;
#else
    (void)s;
    return false;
#endif
}
#define STR_GUARD(_str, _call) do { \
    bool _callee_destroys = ES_C_ABI_STRING_CALLEE_DESTROYS != 0; \
    bool _heap = _callee_destroys && es_string_is_heap(_str); \
    (_call); \
    if (_heap) memset((_str), 0, ES_STRING_SIZE); \
    else cpp_string_destroy((_str)); \
} while (0)

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

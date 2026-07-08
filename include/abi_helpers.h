/**
 * abi_helpers.h — Macro wrappers for MSVC C++ ABI operations.
 *
 * Eliminates repetitive boilerplate in pure-C Endstone plugin code.
 * All macros are designed to be used as expressions or statements.
 *
 * Naming conventions:
 *   VTABLE(obj)          — dereference vtable pointer
 *   VCALLn(obj, slot, ret, types...) — call virtual method with n args
 *   STR_GUARD(str, call) — guard string destruction after pass-by-value call
 *   SEND_MSG(sender, color, fmt, ...) — send colored message
 */

#ifndef ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H
#define ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H
#include "endstone_abi.h"
#include <cppcompat/string.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── VTable dispatch ─────────────────────────────────────────────── */

#define VTABLE(obj)    (*(void ***)(obj))

/* Zero-arg virtual call: VCALL0(obj, slot, ret_type) */
#define VCALL0(obj, slot, ret) \
    ((ret (*)(void *))VTABLE(obj)[slot])(obj)

/* One-arg: VCALL1(obj, slot, ret, T1, a1) */
#define VCALL1(obj, slot, ret, T1, a1) \
    ((ret (*)(void *, T1))VTABLE(obj)[slot])(obj, a1)

/* Two-arg: VCALL2(obj, slot, ret, T1, a1, T2, a2) */
#define VCALL2(obj, slot, ret, T1, a1, T2, a2) \
    ((ret (*)(void *, T1, T2))VTABLE(obj)[slot])(obj, a1, a2)

/* Three-arg: VCALL3(obj, slot, ret, T1, a1, T2, a2, T3, a3) */
#define VCALL3(obj, slot, ret, T1, a1, T2, a2, T3, a3) \
    ((ret (*)(void *, T1, T2, T3))VTABLE(obj)[slot])(obj, a1, a2, a3)

/* Four-arg: VCALL4(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4) */
#define VCALL4(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4) \
    ((ret (*)(void *, T1, T2, T3, T4))VTABLE(obj)[slot])(obj, a1, a2, a3, a4)

/* Five-arg: VCALL5(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5) */
#define VCALL5(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5) \
    ((ret (*)(void *, T1, T2, T3, T4, T5))VTABLE(obj)[slot])(obj, a1, a2, a3, a4, a5)

/* Six-arg: VCALL6(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5, T6, a6) */
#define VCALL6(obj, slot, ret, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5, T6, a6) \
    ((ret (*)(void *, T1, T2, T3, T4, T5, T6))VTABLE(obj)[slot])(obj, a1, a2, a3, a4, a5, a6)

/* ── std::string pass-by-value guard ──────────────────────────────── */
/*
 * Many Endstone APIs take std::string BY VALUE.  The C++ callee destroys
 * the string at function exit (operator delete on heap buffers).  For SSO
 * strings (≤15 chars) this is harmless; for heap strings (>15 chars) we
 * must reset the string to SSO-empty BEFORE cpp_string_destroy to avoid a
 * double-free (cppcompat uses malloc/free, C++ uses operator new/delete).
 *
 * Usage:
 *   char str[32];
 *   cpp_string_construct(str, "hello");
 *   STR_GUARD(str, VCALL2(obj, SLOT, void, void*, str, float, 1.0f));
 *   // str is now safe to leave on stack — already destroyed or reset.
 */

static inline bool str_is_heap(const void *s) {
    return *(const size_t *)((const char *)s + 24) > 15;
}

static inline void str_reset_ssio(void *s) {
    memset(s, 0, ES_STRING_SIZE);
    *(size_t *)((char *)s + 24) = 15;
}

#define STR_GUARD(_str, _call) do {              \
    bool _sg_was_heap = str_is_heap(_str);        \
    _call;                                        \
    if (!_sg_was_heap) {                          \
        cpp_string_destroy(_str);                 \
    } else {                                      \
        str_reset_ssio(_str);                     \
    }                                             \
} while(0)

/* ── Colored message helpers ──────────────────────────────────────── */
/*
 * Minecraft color codes using § (0xc2 0xa7 in UTF-8).
 *   §6 = gold, §a = green, §b = aqua, §c = red, §e = yellow
 */

#define MC_AQUA    "\xc2\xa7" "b"
#define MC_RED     "\xc2\xa7" "c"
#define MC_GOLD    "\xc2\xa7" "6"
#define MC_GREEN   "\xc2\xa7" "a"
#define MC_YELLOW  "\xc2\xa7" "e"
#define MC_GRAY    "\xc2\xa7" "7"

/* ── std::vector construction helpers ─────────────────────────────── */

/* Initialize a std::vector<T> to contain `count` elements of `elem_size`
 * starting at `data`.  The vector is stored at `vec` (24 bytes). */
#define VEC_INIT(vec, data, count, elem_size) do {           \
    void **_v_first = (void **)(vec);                         \
    void **_v_last  = (void **)((char *)(vec) + 8);           \
    void **_v_end   = (void **)((char *)(vec) + 16);          \
    *_v_first = (data);                                       \
    *_v_last  = (char *)(data) + (count) * (elem_size);       \
    *_v_end   = (char *)(data) + (count) * (elem_size);       \
} while(0)

/* ── std::function construction ───────────────────────────────────── */

/* std::function<void(Event&)> and std::function<void()> share the
 * same _Func_impl layout.  Use sfunc_alloc(handler, is_void) and SFUNC_BUILD(buf, impl). */
#define SFUNC_SLOTS 6

typedef struct {
    void **vptr;
    void  *func;
    void  *instance;
} func_impl_t;

/* Allocate a func_impl_t from the static pool (defined in src/sfunc.c).
 * handler: function pointer to the actual handler
 * is_void: true for std::function<void()>, false for std::function<void(Event&)> */
func_impl_t *sfunc_alloc(void *handler, bool is_void);

/* Build the std::function buffer (64 bytes) from a func_impl_t */
#define SFUNC_BUILD(buf, impl) do {                           \
    memset((buf), 0, ES_STD_FUNCTION_SIZE);                   \
    *(void **)((char *)(buf) + 0x00) = (impl)->vptr;           \
    *(void **)((char *)(buf) + 0x08) = (impl)->func;           \
    *(void **)((char *)(buf) + 0x10) = (impl)->instance;       \
    *(void **)((char *)(buf) + 0x30) = (buf);                  \
    *(void **)((char *)(buf) + 0x38) = (buf);                  \
} while(0)

/* ── Logger ───────────────────────────────────────────────────────── */

#define PLUGIN_LOG(self, level, msg) do {                     \
    void *_pl_logger = *(void **)((char *)(self) + ES_PLUGIN_OFF_LOGGER); \
    if (_pl_logger) {                                          \
        struct { const char *data; size_t size; } _pl_sv = {msg, strlen(msg)}; \
        VCALL2(_pl_logger, ES_LOGGER_SLOT_LOG, void,          \
               unsigned char, (unsigned char)(level),          \
               const void *, &_pl_sv);                         \
    }                                                          \
} while(0)

/* ── Server access ────────────────────────────────────────────────── */

#define PLUGIN_SERVER(self) \
    (*(void **)((char *)(self) + ES_PLUGIN_OFF_SERVER))
#endif /* ENDSTONE_MEDIAPLAYER_ABI_HELPERS_H */

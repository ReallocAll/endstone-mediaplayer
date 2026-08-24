#ifndef ENDSTONE_MEDIAPLAYER_ENDSTONE_ABI_H
#define ENDSTONE_MEDIAPLAYER_ENDSTONE_ABI_H

#include <stddef.h>
#include <stdint.h>
#include "platform.h"
#if ES_PLATFORM_WINDOWS
#include "abi/windows_x86_64.h"
#else
#include "abi/linux_x86_64.h"
#endif

struct es_shared_handle {
    _Alignas(ES_SHARED_PTR_ALIGN) unsigned char bytes[ES_SHARED_PTR_SIZE];
};

struct es_weak_handle {
    _Alignas(ES_WEAK_PTR_ALIGN) unsigned char bytes[ES_WEAK_PTR_SIZE];
};

struct es_location {
    _Alignas(ES_LOCATION_ALIGN) unsigned char bytes[ES_LOCATION_SIZE];
};

struct es_identifier {
    _Alignas(ES_IDENTIFIER_ALIGN) unsigned char bytes[ES_IDENTIFIER_SIZE];
};

static_assert(sizeof(struct es_shared_handle) == ES_SHARED_PTR_SIZE);
static_assert(_Alignof(struct es_shared_handle) == ES_SHARED_PTR_ALIGN);
static_assert(sizeof(struct es_weak_handle) == ES_WEAK_PTR_SIZE);
static_assert(_Alignof(struct es_weak_handle) == ES_WEAK_PTR_ALIGN);
static_assert(sizeof(struct es_location) == ES_LOCATION_SIZE);
static_assert(_Alignof(struct es_location) == ES_LOCATION_ALIGN);
static_assert(sizeof(struct es_identifier) == ES_IDENTIFIER_SIZE);
static_assert(_Alignof(struct es_identifier) == ES_IDENTIFIER_ALIGN);

#endif

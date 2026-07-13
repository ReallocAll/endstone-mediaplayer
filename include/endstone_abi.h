#ifndef ENDSTONE_MEDIAPLAYER_ENDSTONE_ABI_H
#define ENDSTONE_MEDIAPLAYER_ENDSTONE_ABI_H

#include <stdint.h>
#include "platform.h"
#if ES_PLATFORM_WINDOWS
#include "abi/windows_x86_64.h"
#else
#include "abi/linux_x86_64.h"
#endif

struct es_location {
    void *dimension;
    float x;
    float y;
    float z;
    float pitch;
    float yaw;
    uint32_t padding;
};

#endif

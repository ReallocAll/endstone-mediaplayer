#include "mediaplayer/endstone_api.h"

#include <stdint.h>

void *endstone_expected_image_base(void)
{
    return (void *)(uintptr_t)0x10000000;
}

bool endstone_expected_image_matches(void *base)
{
    return base == endstone_expected_image_base();
}

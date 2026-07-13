#ifndef CPPCOMPAT_ABI_H
#define CPPCOMPAT_ABI_H
#include "platform.h"
#if ES_PLATFORM_WINDOWS
#define CPP_STRING_SIZE 32
#define CPP_STRING_SSO_CAP 15
#else
#define CPP_STRING_SIZE 24
#define CPP_STRING_SSO_CAP 22
#endif
#define CPP_VECTOR_SIZE 24
#define CPP_STRING_MAX 4095
#endif

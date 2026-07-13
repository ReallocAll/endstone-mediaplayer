#ifndef ENDSTONE_MEDIAPLAYER_PLATFORM_H
#define ENDSTONE_MEDIAPLAYER_PLATFORM_H

#if defined(_WIN32)
#define ES_EXPORT __declspec(dllexport)
#define ES_PLATFORM_WINDOWS 1
#elif defined(__linux__) && defined(__x86_64__)
#define ES_EXPORT __attribute__((visibility("default")))
#define ES_PLATFORM_LINUX 1
#else
#error Unsupported Endstone MediaPlayer platform
#endif

#endif

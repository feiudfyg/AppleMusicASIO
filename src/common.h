#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <propsys.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <intrin.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "propsys.lib")

#ifdef AMB_BRIDGE_EXPORTS
#define AMB_API __declspec(dllexport)
#else
#define AMB_API __declspec(dllimport)
#endif

#define AMB_VERSION_MAJOR 0
#define AMB_VERSION_MINOR 6
#define AMB_VERSION_PATCH 1

#define AMB_MAX_STREAMS 16
#define AMB_MAX_VTABLES 6
#define AMB_MAX_CHANNELS 32
#define AMB_MAX_ASIO_OUT 32

#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT 0x00000001
#define SPEAKER_FRONT_RIGHT 0x00000002
#define SPEAKER_FRONT_CENTER 0x00000004
#endif

typedef enum AmbMode {
    AMB_MODE_AUTO = 0,
    AMB_MODE_ASIO,
    AMB_MODE_EXCLUSIVE,
    AMB_MODE_MONITOR,
    AMB_MODE_OFF
} AmbMode;

static __inline uint32_t Amb_NextPow2(uint32_t v)
{
    if (v < 2) return 2;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

static __inline uint32_t Amb_MinU32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

#define AMB_UNUSED(x) ((void)(x))

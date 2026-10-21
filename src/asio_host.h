#pragma once

#include "common.h"
#include "ring.h"

typedef struct AsioDriverInfo {
    wchar_t key[128];
    wchar_t description[256];
    wchar_t clsid[64];
} AsioDriverInfo;

typedef struct AsioFormatInfo {
    double rate;
    long channels;
    long buffer_frames;
    long sample_type;
    char driver[64];
} AsioFormatInfo;

typedef struct AsioConsumer {
    AmbRing* ring;
    UINT32 frame_bytes;
    UINT32 channels;
    HANDLE* event;
    volatile LONG64* frames_played;
    volatile float* gain;
    volatile BOOL* mute;
    BOOL is_float;
    BOOL volume_enabled;
} AsioConsumer;

int Asio_EnumDrivers(AsioDriverInfo* out, int max_count);
BOOL Asio_FindDriver(const wchar_t* wanted, AsioDriverInfo* out);
BOOL Asio_Available(const wchar_t* wanted);

HRESULT AsioHost_Probe(double rate, int channels, int buffer_frames, AsioFormatInfo* out);
HRESULT AsioHost_OpenStream(double rate, int channels, int buffer_frames, AsioFormatInfo* out);
void AsioHost_SetConsumer(const AsioConsumer* consumer);
HRESULT AsioHost_Start(void);
void AsioHost_Stop(void);
void AsioHost_CloseStream(void);
void AsioHost_Close(void);
void AsioHost_CloseIfIdle(void);
void AsioHost_GetStats(LONG64* frames, LONG* underruns);
const char* Asio_SampleTypeName(long type);

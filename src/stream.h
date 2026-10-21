#pragma once

#include "common.h"
#include "format.h"
#include "ring.h"

typedef struct Stream Stream;

typedef struct RealClientFns {
    HRESULT (STDMETHODCALLTYPE* Initialize)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD,
                                            REFERENCE_TIME, REFERENCE_TIME,
                                            const WAVEFORMATEX*, LPCGUID);
    HRESULT (STDMETHODCALLTYPE* GetBufferSize)(IAudioClient*, UINT32*);
    HRESULT (STDMETHODCALLTYPE* GetStreamLatency)(IAudioClient*, REFERENCE_TIME*);
    HRESULT (STDMETHODCALLTYPE* GetCurrentPadding)(IAudioClient*, UINT32*);
    HRESULT (STDMETHODCALLTYPE* IsFormatSupported)(IAudioClient*, AUDCLNT_SHAREMODE,
                                                   const WAVEFORMATEX*, WAVEFORMATEX**);
    HRESULT (STDMETHODCALLTYPE* GetMixFormat)(IAudioClient*, WAVEFORMATEX**);
    HRESULT (STDMETHODCALLTYPE* GetDevicePeriod)(IAudioClient*, REFERENCE_TIME*, REFERENCE_TIME*);
    HRESULT (STDMETHODCALLTYPE* Start)(IAudioClient*);
    HRESULT (STDMETHODCALLTYPE* Stop)(IAudioClient*);
    HRESULT (STDMETHODCALLTYPE* Reset)(IAudioClient*);
    HRESULT (STDMETHODCALLTYPE* SetEventHandle)(IAudioClient*, HANDLE);
    HRESULT (STDMETHODCALLTYPE* GetService)(IAudioClient*, REFIID, void**);
} RealClientFns;

typedef struct FakeRender FakeRender;
typedef struct FakeClock FakeClock;
typedef struct FakeVolume FakeVolume;
typedef struct FakeStreamVolume FakeStreamVolume;

struct Stream {
    LONG refs;
    CRITICAL_SECTION lock;
    IAudioClient* client;

    AmbAudioFormat app_fmt;
    AmbAudioFormat plan_fmt;
    BOOL have_plan;

    struct AmbSink* sink;
    AmbRing ring;
    uint8_t* staging;
    uint32_t period_frames;

    HANDLE app_event;

    BOOL fake;
    BOOL initialized;
    BOOL started;
    BOOL bitperfect;
    int mode;

    volatile LONG64 frames_played;
    volatile float gain;
    volatile BOOL mute;
};

Stream* Stream_Get(IAudioClient* client, BOOL create);
void Stream_Remove(IAudioClient* client);
void Stream_AddRef(Stream* s);
void Stream_Release(Stream* s);
void Stream_EndFake(Stream* s);
BOOL Stream_BeginFake(Stream* s, const AmbAudioFormat* fmt, int mode,
                      const RealClientFns* real_fns);
void Stream_Signal(Stream* s);
void Stream_GetStats(Stream* s, LONG64* frames);

IAudioRenderClient* Stream_GetRenderClient(Stream* s);
IAudioClock* Stream_GetClock(Stream* s);
ISimpleAudioVolume* Stream_GetVolume(Stream* s);
IAudioStreamVolume* Stream_GetStreamVolume(Stream* s);

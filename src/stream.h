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
    RealClientFns real_fns;
    BOOL has_real_fns;
    AmbRing ring;
    uint8_t* staging;
    uint32_t app_period_frames;

    HANDLE app_event;
    HANDLE pace_event;

    BOOL fake;
    BOOL capture;
    BOOL initialized;
    BOOL started;
    BOOL sink_started;
    BOOL suspended;
    BOOL bitperfect;
    int mode;
    AmbAudioFormat capture_fmt;
    volatile LONG64 capture_frames;
    volatile LONG sink_starting;
    volatile LONG capture_epoch;

    volatile LONG64 app_submitted;
    LARGE_INTEGER app_clock_start;
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
BOOL Stream_BeginCapture(Stream* s, const AmbAudioFormat* app_fmt,
                         const AmbAudioFormat* capture_fmt);
BOOL Stream_ResumeCapture(Stream* s, const AmbAudioFormat* app_fmt,
                          const AmbAudioFormat* capture_fmt);
void Stream_SuspendCapture(Stream* s);
void Stream_SuspendFake(Stream* s);
void Stream_ScheduleClose(Stream* s, DWORD delay_ms);
BOOL Stream_EnsureCaptureSink(Stream* s);
BOOL Stream_WriteCapture(Stream* s, const void* data, uint32_t frames);
void Stream_Signal(Stream* s);
void Stream_GetStats(Stream* s, LONG64* frames);
LONG64 Stream_GetAppFramesPlayed(Stream* s);

IAudioRenderClient* Stream_GetRenderClient(Stream* s);
IAudioClock* Stream_GetClock(Stream* s);
ISimpleAudioVolume* Stream_GetVolume(Stream* s);
IAudioStreamVolume* Stream_GetStreamVolume(Stream* s);
IAudioSessionControl2* Stream_GetSessionControl(Stream* s);

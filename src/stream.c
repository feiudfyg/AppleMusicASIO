#include "stream.h"
#include "sink.h"
#include "asio_host.h"
#include "config.h"
#include "log.h"
#include <math.h>

static SRWLOCK g_streams_lock = SRWLOCK_INIT;
static Stream* g_streams[AMB_MAX_STREAMS];

#define AMB_CAPTURE_SILENCE 0
#define AMB_CAPTURE_THROTTLE 0
#define AMB_APP_BUFFER_FRAMES 4096

/* ------------------------------------------------------------------ */
/* Fake IAudioRenderClient                                             */
/* ------------------------------------------------------------------ */

struct FakeRender {
    IAudioRenderClientVtbl* lpVtbl;
    LONG refs;
    Stream* stream;
    UINT32 requested;
};

static HRESULT STDMETHODCALLTYPE Render_QueryInterface(IAudioRenderClient* This, REFIID riid,
                                                       void** ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioRenderClient)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Render_AddRef(IAudioRenderClient* This)
{
    FakeRender* self = (FakeRender*)This;
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE Render_Release(IAudioRenderClient* This)
{
    FakeRender* self = (FakeRender*)This;
    LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        Stream* s = self->stream;
        free(self);
        Stream_Release(s);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE Render_GetBuffer(IAudioRenderClient* This,
                                                  UINT32 NumFramesRequested, BYTE** ppData)
{
    FakeRender* self = (FakeRender*)This;
    if (!ppData) return E_POINTER;
    Stream* s = self->stream;
    if (!s) return E_FAIL;

    for (int attempt = 0; attempt < 500; ++attempt) {
        EnterCriticalSection(&s->lock);
        if (!s->fake || !s->staging) {
            LeaveCriticalSection(&s->lock);
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        if (NumFramesRequested > s->ring.cap_frames) {
            LeaveCriticalSection(&s->lock);
            return E_INVALIDARG;
        }

        BOOL ready = TRUE;
        if (s->capture && s->started && NumFramesRequested <= s->app_period_frames) {
            LONG64 submitted = InterlockedCompareExchange64(&s->app_submitted, 0, 0);
            LONG64 played = Stream_GetAppFramesPlayed(s);
            LONG64 pending = submitted - played;
            if (pending < 0) pending = 0;
            ready = (pending + NumFramesRequested) <= s->app_period_frames;
        }
        if (ready) {
            self->requested = NumFramesRequested;
            *ppData = s->staging;
            LeaveCriticalSection(&s->lock);
            return S_OK;
        }
        LeaveCriticalSection(&s->lock);
        if (s->pace_event) {
            WaitForSingleObject(s->pace_event, 20);
        } else {
            Sleep(1);
        }
    }

    EnterCriticalSection(&s->lock);
    self->requested = NumFramesRequested;
    *ppData = s->staging;
    LeaveCriticalSection(&s->lock);
    return S_OK;
}

static ULONG g_render_dropped = 0;

static HRESULT STDMETHODCALLTYPE Render_ReleaseBuffer(IAudioRenderClient* This,
                                                      UINT32 NumFramesWritten, DWORD dwFlags)
{
    FakeRender* self = (FakeRender*)This;
    Stream* s = self->stream;
    if (!s) return E_FAIL;

    EnterCriticalSection(&s->lock);
    if (!s->fake || !s->staging) {
        LeaveCriticalSection(&s->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    UINT32 n = NumFramesWritten;
    if (n > self->requested) n = self->requested;
    if (n > s->ring.cap_frames) n = s->ring.cap_frames;

    if (s->capture) {
        /* The virtual endpoint clock starts at the first app render after
           the capture switch, not when the physical sink happens to open.
           Otherwise QPC can report a large positive position before Apple
           has submitted anything, and the app renders without pacing. */
        if (n && s->app_clock_start.QuadPart == 0) {
            QueryPerformanceCounter(&s->app_clock_start);
            s->started = TRUE;
        }
        InterlockedExchangeAdd64(&s->app_submitted, (LONG64)n);
        self->requested = 0;
        LeaveCriticalSection(&s->lock);
        return S_OK;
    }
    if (dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
        memset(s->staging, 0, (size_t)n * s->app_fmt.frame_bytes);
    }
    UINT32 written = Ring_Write(&s->ring, s->staging, n);
    if (written < n) {
        if ((InterlockedIncrement(&g_render_dropped) & 0x3F) == 1) {
            LOGW("render: ring full, dropped %lu frames (total drops %lu)",
                 (unsigned long)(n - written), (unsigned long)g_render_dropped);
        }
    }
    self->requested = 0;
    LeaveCriticalSection(&s->lock);
    return S_OK;
}

static IAudioRenderClientVtbl g_render_vtbl = {
    Render_QueryInterface,
    Render_AddRef,
    Render_Release,
    Render_GetBuffer,
    Render_ReleaseBuffer
};

/* ------------------------------------------------------------------ */
/* Fake IAudioClock                                                    */
/* ------------------------------------------------------------------ */

struct FakeClock {
    IAudioClockVtbl* lpVtbl;
    LONG refs;
    Stream* stream;
};

static HRESULT STDMETHODCALLTYPE Clock_QueryInterface(IAudioClock* This, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioClock)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Clock_AddRef(IAudioClock* This)
{
    FakeClock* self = (FakeClock*)This;
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE Clock_Release(IAudioClock* This)
{
    FakeClock* self = (FakeClock*)This;
    LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        Stream* s = self->stream;
        free(self);
        Stream_Release(s);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE Clock_GetFrequency(IAudioClock* This, UINT64* pFrequency)
{
    FakeClock* self = (FakeClock*)This;
    if (!pFrequency) return E_POINTER;
    *pFrequency = self->stream ? self->stream->app_fmt.rate : 48000;
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 8) {
        LOGI("clock frequency: call=%ld stream=%p capture=%d app=%.0f capture_rate=%.0f freq=%llu",
             (long)call, (void*)self->stream,
             self->stream ? self->stream->capture : -1,
             self->stream ? (double)self->stream->app_fmt.rate : 0.0,
             self->stream ? (double)self->stream->capture_fmt.rate : 0.0,
             (unsigned long long)*pFrequency);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Clock_GetPosition(IAudioClock* This, UINT64* pPosition,
                                                   UINT64* pQPCPosition)
{
    FakeClock* self = (FakeClock*)This;
    if (!pPosition) return E_POINTER;
    *pPosition = (UINT64)Stream_GetAppFramesPlayed(self->stream);
    if (pQPCPosition) *pQPCPosition = 0;
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 8 || (call % 200) == 0) {
        LOGI("clock position: call=%ld stream=%p capture=%d position=%llu submitted=%lld app=%.0f capture_rate=%.0f",
             (long)call, (void*)self->stream,
             self->stream ? self->stream->capture : -1,
             (unsigned long long)*pPosition,
             self->stream ? (long long)InterlockedCompareExchange64(&self->stream->app_submitted, 0, 0) : -1,
             self->stream ? (double)self->stream->app_fmt.rate : 0.0,
             self->stream ? (double)self->stream->capture_fmt.rate : 0.0);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Clock_GetCharacteristics(IAudioClock* This, DWORD* pCharacteristics)
{
    AMB_UNUSED(This);
    if (!pCharacteristics) return E_POINTER;
    *pCharacteristics = 0;
    return S_OK;
}

static IAudioClockVtbl g_clock_vtbl = {
    Clock_QueryInterface,
    Clock_AddRef,
    Clock_Release,
    Clock_GetFrequency,
    Clock_GetPosition,
    Clock_GetCharacteristics
};

/* ------------------------------------------------------------------ */
/* Fake ISimpleAudioVolume                                             */
/* ------------------------------------------------------------------ */

struct FakeVolume {
    ISimpleAudioVolumeVtbl* lpVtbl;
    LONG refs;
    Stream* stream;
};

static HRESULT STDMETHODCALLTYPE Volume_QueryInterface(ISimpleAudioVolume* This, REFIID riid,
                                                       void** ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISimpleAudioVolume)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Volume_AddRef(ISimpleAudioVolume* This)
{
    FakeVolume* self = (FakeVolume*)This;
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE Volume_Release(ISimpleAudioVolume* This)
{
    FakeVolume* self = (FakeVolume*)This;
    LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        Stream* s = self->stream;
        free(self);
        Stream_Release(s);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE Volume_SetMasterVolume(ISimpleAudioVolume* This, float level,
                                                        LPCGUID event_context)
{
    AMB_UNUSED(event_context);
    FakeVolume* self = (FakeVolume*)This;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    self->stream->gain = level;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Volume_GetMasterVolume(ISimpleAudioVolume* This, float* level)
{
    FakeVolume* self = (FakeVolume*)This;
    if (!level) return E_POINTER;
    *level = g_cfg.volume_control ? self->stream->gain : 1.0f;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Volume_SetMute(ISimpleAudioVolume* This, BOOL mute,
                                                LPCGUID event_context)
{
    AMB_UNUSED(event_context);
    FakeVolume* self = (FakeVolume*)This;
    self->stream->mute = mute ? TRUE : FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Volume_GetMute(ISimpleAudioVolume* This, BOOL* mute)
{
    FakeVolume* self = (FakeVolume*)This;
    if (!mute) return E_POINTER;
    *mute = self->stream->mute;
    return S_OK;
}

static ISimpleAudioVolumeVtbl g_volume_vtbl = {
    Volume_QueryInterface,
    Volume_AddRef,
    Volume_Release,
    Volume_SetMasterVolume,
    Volume_GetMasterVolume,
    Volume_SetMute,
    Volume_GetMute
};

/* ------------------------------------------------------------------ */
/* Fake IAudioStreamVolume (stored, not applied)                       */
/* ------------------------------------------------------------------ */

struct FakeStreamVolume {
    IAudioStreamVolumeVtbl* lpVtbl;
    LONG refs;
    Stream* stream;
    float volumes[AMB_MAX_CHANNELS];
};

static HRESULT STDMETHODCALLTYPE StreamVolume_QueryInterface(IAudioStreamVolume* This, REFIID riid,
                                                             void** ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IAudioStreamVolume)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE StreamVolume_AddRef(IAudioStreamVolume* This)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE StreamVolume_Release(IAudioStreamVolume* This)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        Stream* s = self->stream;
        free(self);
        Stream_Release(s);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE StreamVolume_GetChannelCount(IAudioStreamVolume* This,
                                                              UINT32* channels)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    if (!channels) return E_POINTER;
    *channels = self->stream->app_fmt.channels;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StreamVolume_SetChannelVolume(IAudioStreamVolume* This,
                                                               UINT32 index, const float volume)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    if (index >= AMB_MAX_CHANNELS) return E_INVALIDARG;
    self->volumes[index] = volume;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StreamVolume_GetChannelVolume(IAudioStreamVolume* This,
                                                               UINT32 index, float* volume)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    if (!volume || index >= AMB_MAX_CHANNELS) return E_INVALIDARG;
    *volume = self->volumes[index];
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StreamVolume_SetAllVolumes(IAudioStreamVolume* This,
                                                            UINT32 count, const float* volumes)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    if (!volumes) return E_INVALIDARG;
    for (UINT32 i = 0; i < count && i < AMB_MAX_CHANNELS; ++i) {
        self->volumes[i] = volumes[i];
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StreamVolume_GetAllVolumes(IAudioStreamVolume* This,
                                                            UINT32 count, float* volumes)
{
    FakeStreamVolume* self = (FakeStreamVolume*)This;
    if (!volumes) return E_INVALIDARG;
    for (UINT32 i = 0; i < count && i < AMB_MAX_CHANNELS; ++i) {
        volumes[i] = self->volumes[i];
    }
    return S_OK;
}

static IAudioStreamVolumeVtbl g_stream_volume_vtbl = {
    StreamVolume_QueryInterface,
    StreamVolume_AddRef,
    StreamVolume_Release,
    StreamVolume_GetChannelCount,
    StreamVolume_SetChannelVolume,
    StreamVolume_GetChannelVolume,
    StreamVolume_SetAllVolumes,
    StreamVolume_GetAllVolumes
};

/* ------------------------------------------------------------------ */
/* Fake IAudioSessionControl / IAudioSessionControl2                   */
/* ------------------------------------------------------------------ */

typedef struct FakeSession {
    IAudioSessionControl2Vtbl* lpVtbl;
    LONG refs;
    Stream* stream;
} FakeSession;

static HRESULT Session_QueryInterface(IAudioSessionControl2* This, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IAudioSessionControl) ||
        IsEqualIID(riid, &IID_IAudioSessionControl2)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Session_AddRef(IAudioSessionControl2* This)
{
    FakeSession* self = (FakeSession*)This;
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE Session_Release(IAudioSessionControl2* This)
{
    FakeSession* self = (FakeSession*)This;
    LONG refs = InterlockedDecrement(&self->refs);
    if (refs == 0) {
        Stream* s = self->stream;
        free(self);
        Stream_Release(s);
    }
    return (ULONG)refs;
}

static HRESULT CoEmptyString(LPWSTR* out)
{
    if (!out) return E_POINTER;
    wchar_t* p = (wchar_t*)CoTaskMemAlloc(sizeof(wchar_t));
    if (!p) return E_OUTOFMEMORY;
    p[0] = 0;
    *out = p;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_GetState(IAudioSessionControl2* This,
                                                  AudioSessionState* state)
{
    FakeSession* self = (FakeSession*)This;
    if (!state) return E_POINTER;
    *state = (self->stream && self->stream->started) ? AudioSessionStateActive
                                                     : AudioSessionStateInactive;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_GetDisplayName(IAudioSessionControl2* This,
                                                        LPWSTR* name)
{
    AMB_UNUSED(This);
    return CoEmptyString(name);
}

static HRESULT STDMETHODCALLTYPE Session_SetDisplayName(IAudioSessionControl2* This,
                                                        LPCWSTR name, LPCGUID ctx)
{
    AMB_UNUSED(This);
    AMB_UNUSED(name);
    AMB_UNUSED(ctx);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_GetIconPath(IAudioSessionControl2* This,
                                                     LPWSTR* path)
{
    AMB_UNUSED(This);
    return CoEmptyString(path);
}

static HRESULT STDMETHODCALLTYPE Session_SetIconPath(IAudioSessionControl2* This,
                                                     LPCWSTR path, LPCGUID ctx)
{
    AMB_UNUSED(This);
    AMB_UNUSED(path);
    AMB_UNUSED(ctx);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_GetGroupingParam(IAudioSessionControl2* This,
                                                          GUID* param)
{
    AMB_UNUSED(This);
    if (!param) return E_POINTER;
    ZeroMemory(param, sizeof(*param));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_SetGroupingParam(IAudioSessionControl2* This,
                                                          LPCGUID param, LPCGUID ctx)
{
    AMB_UNUSED(This);
    AMB_UNUSED(param);
    AMB_UNUSED(ctx);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_Register(IAudioSessionControl2* This,
                                                  IAudioSessionEvents* events)
{
    AMB_UNUSED(This);
    AMB_UNUSED(events);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_Unregister(IAudioSessionControl2* This,
                                                    IAudioSessionEvents* events)
{
    AMB_UNUSED(This);
    AMB_UNUSED(events);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_GetSessionIdentifier(IAudioSessionControl2* This,
                                                              LPWSTR* id)
{
    AMB_UNUSED(This);
    return CoEmptyString(id);
}

static HRESULT STDMETHODCALLTYPE Session_GetSessionInstanceIdentifier(
    IAudioSessionControl2* This, LPWSTR* id)
{
    AMB_UNUSED(This);
    return CoEmptyString(id);
}

static HRESULT STDMETHODCALLTYPE Session_GetProcessId(IAudioSessionControl2* This,
                                                      DWORD* pid)
{
    AMB_UNUSED(This);
    if (!pid) return E_POINTER;
    *pid = GetCurrentProcessId();
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Session_IsSystemSoundsSession(IAudioSessionControl2* This)
{
    AMB_UNUSED(This);
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE Session_SetDuckingPreference(IAudioSessionControl2* This,
                                                              BOOL opt_out)
{
    AMB_UNUSED(This);
    AMB_UNUSED(opt_out);
    return S_OK;
}

static IAudioSessionControl2Vtbl g_session_vtbl = {
    Session_QueryInterface,
    Session_AddRef,
    Session_Release,
    Session_GetState,
    Session_GetDisplayName,
    Session_SetDisplayName,
    Session_GetIconPath,
    Session_SetIconPath,
    Session_GetGroupingParam,
    Session_SetGroupingParam,
    Session_Register,
    Session_Unregister,
    Session_GetSessionIdentifier,
    Session_GetSessionInstanceIdentifier,
    Session_GetProcessId,
    Session_IsSystemSoundsSession,
    Session_SetDuckingPreference
};

/* ------------------------------------------------------------------ */
/* Stream table and lifetime                                           */
/* ------------------------------------------------------------------ */

static void Stream_Destroy(Stream* s);

Stream* Stream_Get(IAudioClient* client, BOOL create)
{
    if (!client) return NULL;
    AcquireSRWLockExclusive(&g_streams_lock);
    for (int i = 0; i < AMB_MAX_STREAMS; ++i) {
        if (g_streams[i] && g_streams[i]->client == client) {
            Stream* s = g_streams[i];
            ReleaseSRWLockExclusive(&g_streams_lock);
            return s;
        }
    }
    if (!create) {
        ReleaseSRWLockExclusive(&g_streams_lock);
        return NULL;
    }
    Stream* s = (Stream*)calloc(1, sizeof(*s));
    if (!s) {
        ReleaseSRWLockExclusive(&g_streams_lock);
        return NULL;
    }
    InitializeCriticalSection(&s->lock);
    s->pace_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->refs = 1;
    s->client = client;
    s->gain = 1.0f;
    s->mute = FALSE;
    for (int i = 0; i < AMB_MAX_STREAMS; ++i) {
        if (!g_streams[i]) {
            g_streams[i] = s;
            ReleaseSRWLockExclusive(&g_streams_lock);
            return s;
        }
    }
    ReleaseSRWLockExclusive(&g_streams_lock);
    DeleteCriticalSection(&s->lock);
    free(s);
    return NULL;
}

void Stream_Remove(IAudioClient* client)
{
    Stream* found = NULL;
    AcquireSRWLockExclusive(&g_streams_lock);
    for (int i = 0; i < AMB_MAX_STREAMS; ++i) {
        if (g_streams[i] && g_streams[i]->client == client) {
            found = g_streams[i];
            g_streams[i] = NULL;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_streams_lock);
    if (found) {
        Stream_EndFake(found);
        Stream_Release(found);
    }
}

void Stream_AddRef(Stream* s)
{
    if (s) InterlockedIncrement(&s->refs);
}

void Stream_Release(Stream* s)
{
    if (!s) return;
    if (InterlockedDecrement(&s->refs) == 0) Stream_Destroy(s);
}

static void Stream_CloseSinkLocked(Stream* s)
{
    if (s->sink) {
        s->suspended = TRUE;
        s->sink->Close(s->sink);
        free(s->sink);
        s->sink = NULL;
    }
    s->sink_started = FALSE;
    s->fake = FALSE;
    s->capture = FALSE;
    s->initialized = FALSE;
    s->started = FALSE;
    s->sink_started = FALSE;
    s->suspended = FALSE;
    s->app_period_frames = 0;
    s->app_submitted = 0;
    s->app_clock_start.QuadPart = 0;
}

void Stream_EndFake(Stream* s)
{
    if (!s) return;
    EnterCriticalSection(&s->lock);
    LOGI("stream end: %p capture=%d started=%d sink=%p",
         (void*)s, s->capture ? 1 : 0, s->started ? 1 : 0, (void*)s->sink);
    /* Stop must be preceded by a gate and a clear.  sink->Stop can take
       several hundred milliseconds, and the ASIO callback must already be
       emitting silence before the driver is torn down. */
    s->suspended = TRUE;
    s->started = FALSE;
    Ring_Clear(&s->ring);
    Stream_CloseSinkLocked(s);
    LeaveCriticalSection(&s->lock);
    AsioHost_CloseIfIdle();
}

static void Stream_Destroy(Stream* s)
{
    EnterCriticalSection(&s->lock);
    Stream_CloseSinkLocked(s);
    Ring_Free(&s->ring);
    free(s->staging);
    s->staging = NULL;
    LeaveCriticalSection(&s->lock);
    if (s->pace_event) CloseHandle(s->pace_event);
    DeleteCriticalSection(&s->lock);
    free(s);
}

void Stream_Signal(Stream* s)
{
    if (s && s->app_event) SetEvent(s->app_event);
}

LONG64 Stream_GetAppFramesPlayed(Stream* s)
{
    if (!s) return 0;
    LONG64 played = s->sink && s->sink->GetPlayed
                        ? s->sink->GetPlayed(s->sink)
                        : InterlockedCompareExchange64(&s->frames_played, 0, 0);
    if (!s->capture) return played;

    double app_rate = s->app_fmt.rate;
    double capture_rate = s->capture_fmt.rate;
    if (app_rate > 0.0 && capture_rate > 0.0) {
        played = (LONG64)((double)played * app_rate / capture_rate);
    }

    if (!s->sink && s->app_clock_start.QuadPart != 0 && app_rate > 0.0 &&
        (s->started || s->sink_started)) {
        LARGE_INTEGER now;
        LARGE_INTEGER freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart > 0) {
            double seconds = (double)(now.QuadPart - s->app_clock_start.QuadPart) /
                             (double)freq.QuadPart;
            LONG64 estimate = (LONG64)(seconds * app_rate);
            if (estimate > played) played = estimate;
        }
    }
    return played;
}

void Stream_GetStats(Stream* s, LONG64* frames)
{
    if (frames && s) *frames = InterlockedCompareExchange64(&s->frames_played, 0, 0);
}

BOOL Stream_BeginFake(Stream* s, const AmbAudioFormat* fmt, int mode,
                      const RealClientFns* real_fns)
{
    if (!s || !fmt) return FALSE;

    EnterCriticalSection(&s->lock);
    Stream_CloseSinkLocked(s);
    Ring_Free(&s->ring);
    free(s->staging);
    s->staging = NULL;

    s->app_fmt = *fmt;
    s->mode = mode;
    s->capture = FALSE;
    s->bitperfect = FALSE;
    if (real_fns) {
        s->real_fns = *real_fns;
        s->has_real_fns = TRUE;
    } else {
        s->has_real_fns = FALSE;
    }
    s->frames_played = 0;
    s->app_submitted = 0;
    s->app_clock_start.QuadPart = 0;
    s->gain = 1.0f;
    s->mute = FALSE;

    AmbSink* sink = NULL;
    BOOL ok;
    if (mode == AMB_MODE_ASIO) {
        ok = Sink_AsioCreate(s, fmt, &sink);
    } else {
        ok = Sink_WasapiCreate(s, fmt, real_fns, &sink);
    }
    if (!ok || !sink) {
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }

    uint32_t period = sink->period_frames ? sink->period_frames : 480;
    uint32_t cap = mode == AMB_MODE_ASIO ? period * 4 : period * 2;
    if (cap < 512) cap = 512;
    if (!Ring_Init(&s->ring, cap, fmt->frame_bytes)) {
        sink->Close(sink);
        free(sink);
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }

    s->staging = (uint8_t*)calloc(s->ring.cap_frames, fmt->frame_bytes);
    if (!s->staging) {
        sink->Close(sink);
        free(sink);
        Ring_Free(&s->ring);
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }

    sink->SetRing(sink, &s->ring);
    s->sink = sink;
    s->app_period_frames = period;
    s->fake = TRUE;
    s->initialized = TRUE;
    s->started = FALSE;
    s->sink_started = FALSE;
    s->bitperfect = sink->bitperfect;

    LOGI("bridge stream: %s, app buffer %lu frames (%lu bytes/frame), period %lu frames, %s",
         mode == AMB_MODE_ASIO ? "ASIO" : "WASAPI-exclusive",
         (unsigned long)s->ring.cap_frames, (unsigned long)fmt->frame_bytes,
         (unsigned long)period,
         s->bitperfect ? "bit-perfect" : "transparent conversion");
    LeaveCriticalSection(&s->lock);
    return TRUE;
}

BOOL Stream_BeginCapture(Stream* s, const AmbAudioFormat* app_fmt,
                         const AmbAudioFormat* capture_fmt)
{
    if (!s || !app_fmt || !capture_fmt) return FALSE;

    EnterCriticalSection(&s->lock);
    int capture_mode = s->mode;
    InterlockedIncrement(&s->capture_epoch);
    Stream_CloseSinkLocked(s);
    Ring_Free(&s->ring);
    free(s->staging);
    s->staging = NULL;

    s->app_fmt = *app_fmt;
    s->capture_fmt = *capture_fmt;
    s->mode = capture_mode;
    s->capture = TRUE;
    s->bitperfect = FALSE;
    s->frames_played = 0;
    s->capture_frames = 0;
    s->app_submitted = 0;
    s->app_clock_start.QuadPart = 0;
    s->sink_starting = 0;
    s->gain = 1.0f;
    s->mute = FALSE;

    uint32_t period = AMB_APP_BUFFER_FRAMES;
    uint32_t cap = 1u << 23;
    if (!Ring_Init(&s->ring, cap, capture_fmt->frame_bytes)) {
        s->capture = FALSE;
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }

    s->staging = (uint8_t*)calloc(s->ring.cap_frames, app_fmt->frame_bytes);
    if (!s->staging) {
        Ring_Free(&s->ring);
        s->capture = FALSE;
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }

    s->sink = NULL;
    s->app_period_frames = period;
    s->fake = TRUE;
    s->initialized = TRUE;
    s->started = FALSE;
    s->sink_started = FALSE;
    s->suspended = FALSE;
    s->bitperfect = FALSE;

    LOGI("capture stream pending: app %.0f Hz / capture %.0f Hz, %lu frames",
         (double)app_fmt->rate, (double)capture_fmt->rate,
         (unsigned long)s->ring.cap_frames);
    LeaveCriticalSection(&s->lock);
    return TRUE;
}

BOOL Stream_ResumeCapture(Stream* s, const AmbAudioFormat* app_fmt,
                          const AmbAudioFormat* capture_fmt)
{
    if (!s || !app_fmt || !capture_fmt) return FALSE;

    EnterCriticalSection(&s->lock);
    if (!s->capture || !s->sink || !s->ring.data || !s->staging ||
        !Format_Equal(&s->capture_fmt, capture_fmt)) {
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }

    InterlockedIncrement(&s->capture_epoch);
    Ring_Clear(&s->ring);
    s->app_fmt = *app_fmt;
    s->capture_fmt = *capture_fmt;
    s->app_submitted = 0;
    s->app_clock_start.QuadPart = 0;
    s->frames_played = 0;
    s->capture_frames = 0;
    s->started = FALSE;
    s->suspended = FALSE;
    s->bitperfect = s->sink->bitperfect;
    LOGI("capture resumed: stream=%p capture=%.0f Hz sink_started=%d",
         (void*)s, (double)capture_fmt->rate, s->sink_started ? 1 : 0);
    LeaveCriticalSection(&s->lock);
    if (s->pace_event) SetEvent(s->pace_event);
    return TRUE;
}

void Stream_SuspendFake(Stream* s)
{
    if (!s) return;
    EnterCriticalSection(&s->lock);
    if (s->fake) {
        s->started = FALSE;
        s->suspended = TRUE;
        s->app_submitted = 0;
        s->app_clock_start.QuadPart = 0;
        s->frames_played = 0;
        Ring_Clear(&s->ring);
        LOGI("normal stream suspended: stream=%p sink=%p", (void*)s, (void*)s->sink);
    }
    LeaveCriticalSection(&s->lock);
    if (s->pace_event) SetEvent(s->pace_event);
}

void Stream_SuspendCapture(Stream* s)
{
    if (!s) return;
    EnterCriticalSection(&s->lock);
    if (s->capture) {
        s->started = FALSE;
        s->suspended = TRUE;
        s->app_submitted = 0;
        s->app_clock_start.QuadPart = 0;
        s->frames_played = 0;
        s->capture_frames = 0;
        Ring_Clear(&s->ring);
        LOGI("capture suspended: stream=%p sink=%p", (void*)s, (void*)s->sink);
    }
    LeaveCriticalSection(&s->lock);
    if (s->pace_event) SetEvent(s->pace_event);
}

typedef struct CloseCaptureCtx {
    Stream* stream;
    LONG epoch;
    DWORD delay_ms;
} CloseCaptureCtx;

static DWORD WINAPI CloseCaptureThread(void* parameter)
{
    CloseCaptureCtx* ctx = (CloseCaptureCtx*)parameter;
    Sleep(ctx->delay_ms);
    Stream* s = ctx->stream;
    if (s && s->capture_epoch == ctx->epoch && s->capture && !s->started) {
        Stream_EndFake(s);
    }
    if (s) Stream_Release(s);
    free(ctx);
    return 0;
}

void Stream_ScheduleClose(Stream* s, DWORD delay_ms)
{
    if (!s) return;
    CloseCaptureCtx* ctx = (CloseCaptureCtx*)calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->stream = s;
    ctx->epoch = InterlockedIncrement(&s->capture_epoch);
    ctx->delay_ms = delay_ms ? delay_ms : 1000;
    Stream_AddRef(s);
    HANDLE thread = CreateThread(NULL, 0, CloseCaptureThread, ctx, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        Stream_Release(s);
        free(ctx);
    }
}

static SRWLOCK g_capture_sink_lock = SRWLOCK_INIT;

static BOOL EnsureCaptureSinkImpl(Stream* s)
{
    if (!s) return FALSE;

    AmbAudioFormat fmt;
    int mode;
    BOOL have_real_fns;
    RealClientFns real_fns;
    EnterCriticalSection(&s->lock);
    if (!s->capture || s->sink) {
        LeaveCriticalSection(&s->lock);
        return s->capture != FALSE;
    }
    fmt = s->capture_fmt;
    mode = s->mode;
    have_real_fns = s->has_real_fns;
    real_fns = s->real_fns;
    LeaveCriticalSection(&s->lock);

    AmbSink* sink = NULL;
    BOOL ok;
    if (mode == AMB_MODE_EXCLUSIVE && have_real_fns) {
        ok = Sink_WasapiCreate(s, &fmt, &real_fns, &sink);
    } else {
        ok = Sink_AsioCreate(s, &fmt, &sink);
    }
    if (!ok || !sink) {
        return FALSE;
    }

    EnterCriticalSection(&s->lock);
    if (!s->capture || s->sink) {
        BOOL ready = s->capture && s->sink != NULL;
        LeaveCriticalSection(&s->lock);
        sink->Close(sink);
        free(sink);
        return ready;
    }

    sink->SetRing(sink, &s->ring);
    s->sink = sink;
    s->app_period_frames = AMB_APP_BUFFER_FRAMES;
    s->bitperfect = sink->bitperfect;

    LeaveCriticalSection(&s->lock);
    LOGI("capture sink ready: capture %.0f Hz, app period %lu frames, %s; waiting for tap prefill",
         (double)fmt.rate, (unsigned long)AMB_APP_BUFFER_FRAMES,
         s->bitperfect ? "bit-perfect" : "transparent conversion");
    return TRUE;
}

BOOL Stream_EnsureCaptureSink(Stream* s)
{
    if (!s) return FALSE;
    AcquireSRWLockExclusive(&g_capture_sink_lock);
    BOOL ok = EnsureCaptureSinkImpl(s);
    ReleaseSRWLockExclusive(&g_capture_sink_lock);
    return ok;
}

static DWORD WINAPI CaptureSinkThread(void* parameter)
{
    Stream* s = (Stream*)parameter;
    LOGI("capture: ASIO worker start");
    BOOL ok = Stream_EnsureCaptureSink(s);
    if (!ok) LOGW("capture: background ASIO setup failed");
    InterlockedExchange(&s->sink_starting, 0);
    Stream_Release(s);
    return 0;
}

static void RequestCaptureSink(Stream* s)
{
    if (!s || !s->capture || s->sink) return;
    if (InterlockedCompareExchange(&s->sink_starting, 1, 0) != 0) return;

    LOGI("capture: request ASIO sink");
    Stream_AddRef(s);
    HANDLE thread = CreateThread(NULL, 0, CaptureSinkThread, s, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&s->sink_starting, 0);
        Stream_Release(s);
    }
}

BOOL Stream_WriteCapture(Stream* s, const void* data, uint32_t frames)
{
    if (!s || !data || !frames) return FALSE;
    if (s->capture && !s->suspended && !s->sink) {
        RequestCaptureSink(s);
    }
    EnterCriticalSection(&s->lock);
    if (!s->capture || s->suspended || !s->ring.data) {
        LeaveCriticalSection(&s->lock);
        return FALSE;
    }
#if AMB_CAPTURE_THROTTLE
    if (s->sink && s->sink_started) {
        const UINT32 target = 22050;
        for (int wait = 0; wait < 1000; ++wait) {
            if (Ring_Fill(&s->ring) + frames <= target) break;
            Sleep(1);
        }
    }
#endif
#if AMB_CAPTURE_SILENCE
    static const float silence[4096 * 2] = {0};
    UINT32 written = 0;
    UINT32 remaining = frames;
    while (remaining > 0) {
        UINT32 chunk = remaining > 4096 ? 4096 : remaining;
        UINT32 n = Ring_Write(&s->ring, silence, chunk);
        written += n;
        if (n < chunk) break;
        remaining -= chunk;
    }
#else
    UINT32 written = Ring_Write(&s->ring, data, frames);
#endif
    LONG64 total = InterlockedExchangeAdd64(&s->capture_frames, frames) + frames;
    if (s->capture_fmt.kind == AMB_SAMPLE_F32 && s->capture_fmt.frame_bytes == 8) {
        const float* samples = (const float*)data;
        float peak = 0.0f;
        double sum = 0.0;
        size_t count = (size_t)frames * 2u;
        for (size_t i = 0; i < count; ++i) {
            float v = samples[i];
            float a = v < 0.0f ? -v : v;
            if (a > peak) peak = a;
            sum += (double)v * (double)v;
        }
        if (total >= 44100) {
            static volatile LONG64 meter_next = 44100;
            if (total >= InterlockedCompareExchange64(&meter_next, 0, 0)) {
                float rms = (float)sqrt(sum / (double)(count ? count : 1));
                LOGI("capture level: peak=%.4f rms=%.4f frames=%lld", peak, rms,
                     (long long)total);
                if (peak > 1.001f) LOGW("capture level clipping: peak=%.4f", peak);
                InterlockedExchange64(&meter_next, total + 44100);
            }
        }
    }
    if (total <= 16384) {
        LOGI("capture write: %lu frames, total %lld, ring fill %lu",
             (unsigned long)frames, (long long)total,
             (unsigned long)Ring_Fill(&s->ring));
    }
    AmbSink* start_sink = NULL;
    uint32_t prefill = 1024;
    if (s->sink && !s->sink_started && Ring_Fill(&s->ring) >= prefill) {
        s->sink_started = TRUE;
        start_sink = s->sink;
        LOGI("capture: starting sink after prefill %lu frames",
             (unsigned long)Ring_Fill(&s->ring));
    }
    LeaveCriticalSection(&s->lock);

    if (start_sink) {
        HRESULT hr = start_sink->Start(start_sink);
        if (FAILED(hr)) {
            LOGW("capture: ASIO start failed 0x%08lx", (unsigned long)hr);
            EnterCriticalSection(&s->lock);
            if (s->sink == start_sink) s->sink_started = FALSE;
            LeaveCriticalSection(&s->lock);
        }
    }

    if (written < frames) {
        static volatile LONG warned = 0;
        if (InterlockedExchange(&warned, 1) == 0) {
            LOGW("capture ring overflow: wrote %lu of %lu frames",
                 (unsigned long)written, (unsigned long)frames);
        }
    }
    return written == frames;
}

IAudioRenderClient* Stream_GetRenderClient(Stream* s)
{
    if (!s) return NULL;
    FakeRender* fr = (FakeRender*)calloc(1, sizeof(*fr));
    if (!fr) return NULL;
    fr->lpVtbl = &g_render_vtbl;
    fr->refs = 1;
    fr->stream = s;
    Stream_AddRef(s);
    return (IAudioRenderClient*)fr;
}

IAudioClock* Stream_GetClock(Stream* s)
{
    if (!s) return NULL;
    FakeClock* fc = (FakeClock*)calloc(1, sizeof(*fc));
    if (!fc) return NULL;
    fc->lpVtbl = &g_clock_vtbl;
    fc->refs = 1;
    fc->stream = s;
    Stream_AddRef(s);
    return (IAudioClock*)fc;
}

ISimpleAudioVolume* Stream_GetVolume(Stream* s)
{
    if (!s) return NULL;
    FakeVolume* fv = (FakeVolume*)calloc(1, sizeof(*fv));
    if (!fv) return NULL;
    fv->lpVtbl = &g_volume_vtbl;
    fv->refs = 1;
    fv->stream = s;
    Stream_AddRef(s);
    return (ISimpleAudioVolume*)fv;
}

IAudioStreamVolume* Stream_GetStreamVolume(Stream* s)
{
    if (!s) return NULL;
    FakeStreamVolume* fv = (FakeStreamVolume*)calloc(1, sizeof(*fv));
    if (!fv) return NULL;
    fv->lpVtbl = &g_stream_volume_vtbl;
    fv->refs = 1;
    fv->stream = s;
    for (int i = 0; i < AMB_MAX_CHANNELS; ++i) fv->volumes[i] = 1.0f;
    Stream_AddRef(s);
    return (IAudioStreamVolume*)fv;
}

IAudioSessionControl2* Stream_GetSessionControl(Stream* s)
{
    if (!s) return NULL;
    FakeSession* fs = (FakeSession*)calloc(1, sizeof(*fs));
    if (!fs) return NULL;
    fs->lpVtbl = &g_session_vtbl;
    fs->refs = 1;
    fs->stream = s;
    Stream_AddRef(s);
    return (IAudioSessionControl2*)fs;
}



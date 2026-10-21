#include "stream.h"
#include "sink.h"
#include "asio_host.h"
#include "config.h"
#include "log.h"

static SRWLOCK g_streams_lock = SRWLOCK_INIT;
static Stream* g_streams[AMB_MAX_STREAMS];

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

    EnterCriticalSection(&s->lock);
    if (!s->fake || !s->staging) {
        LeaveCriticalSection(&s->lock);
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    if (NumFramesRequested > s->ring.cap_frames) {
        LeaveCriticalSection(&s->lock);
        return E_INVALIDARG;
    }
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
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Clock_GetPosition(IAudioClock* This, UINT64* pPosition,
                                                   UINT64* pQPCPosition)
{
    FakeClock* self = (FakeClock*)This;
    if (!pPosition) return E_POINTER;
    *pPosition = (UINT64)InterlockedCompareExchange64(&self->stream->frames_played, 0, 0);
    if (pQPCPosition) *pQPCPosition = 0;
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
        if (s->started) s->sink->Stop(s->sink);
        s->sink->Close(s->sink);
        free(s->sink);
        s->sink = NULL;
    }
    s->fake = FALSE;
    s->initialized = FALSE;
    s->started = FALSE;
    s->period_frames = 0;
}

void Stream_EndFake(Stream* s)
{
    if (!s) return;
    EnterCriticalSection(&s->lock);
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
    DeleteCriticalSection(&s->lock);
    free(s);
}

void Stream_Signal(Stream* s)
{
    if (s && s->app_event) SetEvent(s->app_event);
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
    s->bitperfect = FALSE;
    s->frames_played = 0;
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
    s->period_frames = period;
    s->fake = TRUE;
    s->initialized = TRUE;
    s->started = FALSE;
    s->bitperfect = TRUE;

    LOGI("bridge stream: %s, app buffer %lu frames (%lu bytes/frame), period %lu frames",
         mode == AMB_MODE_ASIO ? "ASIO" : "WASAPI-exclusive",
         (unsigned long)s->ring.cap_frames, (unsigned long)fmt->frame_bytes,
         (unsigned long)period);
    LeaveCriticalSection(&s->lock);
    return TRUE;
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

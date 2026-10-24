#include "sink.h"
#include "config.h"
#include "log.h"
#include "hooks.h"

#include <math.h>

BOOL Wasapi_PickExclusive(IAudioClient* client, const RealClientFns* fns,
                          const AmbAudioFormat* mix, AmbAudioFormat* out);
void Wasapi_EnableRaw(IAudioClient* client);

typedef struct WasapiSink {
    AmbSink base;
    Stream* stream;
    IAudioClient* client;
    RealClientFns fns;
    IAudioRenderClient* render;
    HANDLE event;
    HANDLE thread;
    volatile LONG stop;
    BOOL own_client;
    BOOL event_mode;
    BOOL started;
    BOOL converting;
    UINT32 frames;
    AmbAudioFormat app_fmt;
    AmbAudioFormat fmt;
    BYTE* conv;
    size_t conv_bytes;
    LONG underruns;
} WasapiSink;

static BOOL TryOneExclusive(IAudioClient* client, const RealClientFns* fns,
                            const AmbAudioFormat* cand, AmbAudioFormat* out)
{
    WAVEFORMATEX* closest = NULL;
    HRESULT hr = fns->IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        &cand->wfx.Format, &closest);
    if (hr == S_OK) {
        *out = *cand;
        if (closest) CoTaskMemFree(closest);
        return TRUE;
    }
    /* For exclusive mode, S_FALSE is not a usable "closest match"; the
       device must accept the exact format or we fall back to shared. */
    if (closest) CoTaskMemFree(closest);
    return FALSE;
}

BOOL Wasapi_PickExclusive(IAudioClient* client, const RealClientFns* fns,
                          const AmbAudioFormat* mix, AmbAudioFormat* out)
{
    AmbAudioFormat cands[6];
    int count = 0;
    cands[count++] = *mix;
    Format_Make(&cands[count++], mix->rate, mix->channels, AMB_SAMPLE_F32, 32);
    Format_Make(&cands[count++], mix->rate, mix->channels, AMB_SAMPLE_S32, 32);
    Format_Make(&cands[count++], mix->rate, mix->channels, AMB_SAMPLE_S32, 24);
    Format_Make(&cands[count++], mix->rate, mix->channels, AMB_SAMPLE_S24, 24);
    Format_Make(&cands[count++], mix->rate, mix->channels, AMB_SAMPLE_S16, 16);

    for (int i = 0; i < count; ++i) {
        AmbAudioFormat chosen;
        if (TryOneExclusive(client, fns, &cands[i], &chosen)) {
            char a[128], b[128];
            Format_ToString(&cands[i], a, sizeof(a));
            Format_ToString(&chosen, b, sizeof(b));
            if (i > 0 || !Format_Equal(&chosen, &cands[i])) {
                LOGW("WASAPI exclusive: requested %s, device accepts %s", a, b);
            }
            *out = chosen;
            return TRUE;
        }
    }
    return FALSE;
}

void Wasapi_EnableRaw(IAudioClient* client)
{
    IAudioClient2* c2 = NULL;
    HRESULT hr = client->lpVtbl->QueryInterface(client, &IID_IAudioClient2, (void**)&c2);
    if (FAILED(hr) || !c2) return;

    AudioClientProperties props;
    ZeroMemory(&props, sizeof(props));
    props.cbSize = sizeof(props);
    props.bIsOffload = FALSE;
    props.eCategory = AudioCategory_Media;
    props.Options = AUDCLNT_STREAMOPTIONS_RAW;
    hr = c2->lpVtbl->SetClientProperties(c2, &props);
    if (FAILED(hr)) {
        LOGD("WASAPI: SetClientProperties(RAW) failed (hr=0x%08lx)", (unsigned long)hr);
    } else {
        LOGI("WASAPI: shared fallback requested RAW stream options");
    }
    c2->lpVtbl->Release(c2);
}

static void ApplyVolume(WasapiSink* sink, BYTE* data, UINT32 frames)
{
    if (!g_cfg.volume_control || !sink->fmt.is_float) return;
    float gain = sink->stream->gain;
    BOOL mute = sink->stream->mute;
    if (!mute && fabsf(gain - 1.0f) < 1e-7f) return;

    float* f = (float*)data;
    size_t samples = (size_t)frames * sink->fmt.channels;
    if (mute) {
        memset(f, 0, samples * sizeof(float));
    } else {
        for (size_t i = 0; i < samples; ++i) f[i] *= gain;
    }
}

static int32_t QuantizeSigned(double v, UINT32 bits)
{
    double scale = ldexp(1.0, (int)bits - 1);
    double x = v * scale;
    if (x >= scale - 1.0) return (int32_t)(scale - 1.0);
    if (x <= -scale) return (int32_t)(-scale);
    return (int32_t)llrint(x);
}

static int32_t ReadPacked24(const BYTE* p)
{
    int32_t v = (int32_t)((UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16));
    if (v & 0x00800000) v |= (int32_t)0xFF000000;
    return v;
}

static void WritePacked24(BYTE* p, int32_t v)
{
    p[0] = (BYTE)(v & 0xFF);
    p[1] = (BYTE)((v >> 8) & 0xFF);
    p[2] = (BYTE)((v >> 16) & 0xFF);
}

static double ReadSample(const AmbAudioFormat* f, const BYTE* frame, UINT32 ch)
{
    const BYTE* p = frame + (size_t)ch * f->bytes_per_sample;
    if (f->is_float) {
        float v;
        memcpy(&v, p, sizeof(v));
        return (double)v;
    }
    if (f->kind == AMB_SAMPLE_S16) {
        int16_t v;
        memcpy(&v, p, sizeof(v));
        return (double)v / 32768.0;
    }
    if (f->kind == AMB_SAMPLE_S24) {
        return (double)ReadPacked24(p) / 8388608.0;
    }
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return (double)(v >> (32 - f->valid_bits)) /
           ldexp(1.0, (int)f->valid_bits - 1);
}

static void WriteSample(const AmbAudioFormat* f, BYTE* frame, UINT32 ch, double v)
{
    BYTE* p = frame + (size_t)ch * f->bytes_per_sample;
    if (f->is_float) {
        float x = (float)v;
        memcpy(p, &x, sizeof(x));
        return;
    }
    int32_t q = QuantizeSigned(v, f->valid_bits);
    if (f->kind == AMB_SAMPLE_S16) {
        int16_t x = (int16_t)q;
        memcpy(p, &x, sizeof(x));
    } else if (f->kind == AMB_SAMPLE_S24) {
        WritePacked24(p, q);
    } else {
        int32_t x = (int32_t)((UINT32)q << (32 - f->valid_bits));
        memcpy(p, &x, sizeof(x));
    }
}

static void ConvertFrames(WasapiSink* sink, const BYTE* src, BYTE* dst, UINT32 frames)
{
    const AmbAudioFormat* a = &sink->app_fmt;
    const AmbAudioFormat* b = &sink->fmt;
    BOOL apply = g_cfg.volume_control;
    float gain = sink->stream->gain;
    BOOL mute = sink->stream->mute;

    for (UINT32 i = 0; i < frames; ++i) {
        const BYTE* in = src + (size_t)i * a->frame_bytes;
        BYTE* out = dst + (size_t)i * b->frame_bytes;
        for (UINT32 ch = 0; ch < b->channels; ++ch) {
            double v = ch < a->channels ? ReadSample(a, in, ch) : 0.0;
            if (mute) v = 0.0;
            else if (apply) v *= (double)gain;
            WriteSample(b, out, ch, v);
        }
    }
}

static DWORD WINAPI WasapiFeeder(void* param)
{
    WasapiSink* sink = (WasapiSink*)param;
    Stream* s = sink->stream;

    while (!sink->stop) {
        if (sink->event_mode) {
            WaitForSingleObject(sink->event, 100);
        } else {
            Sleep(4);
        }
        if (sink->stop) break;

        UINT32 avail = sink->frames;
        if (!sink->event_mode) {
            UINT32 padding = 0;
            if (FAILED(sink->fns.GetCurrentPadding(sink->client, &padding))) continue;
            if (padding >= sink->frames) continue;
            avail = sink->frames - padding;
        }
        BYTE* data = NULL;
        if (FAILED(sink->render->lpVtbl->GetBuffer(sink->render, avail, &data)) || !data) {
            continue;
        }
        uint32_t got;
        if (sink->converting) {
            size_t need = (size_t)avail * sink->app_fmt.frame_bytes;
            if (!sink->conv || sink->conv_bytes < need) {
                BYTE* p = (BYTE*)realloc(sink->conv, need);
                if (!p) {
                    sink->render->lpVtbl->ReleaseBuffer(sink->render, avail,
                                                       AUDCLNT_BUFFERFLAGS_SILENT);
                    continue;
                }
                sink->conv = p;
                sink->conv_bytes = need;
            }
            got = Ring_ReadZero(&s->ring, sink->conv, avail);
            ConvertFrames(sink, sink->conv, data, avail);
        } else {
            got = Ring_ReadZero(&s->ring, data, avail);
            ApplyVolume(sink, data, avail);
        }
        if (got < avail) {
            sink->underruns++;
        }
        sink->render->lpVtbl->ReleaseBuffer(sink->render, avail, 0);

        if (got) InterlockedExchangeAdd64(&s->frames_played, (LONG64)got);
        if (s->app_event) SetEvent(s->app_event);
    }
    return 0;
}

static SRWLOCK g_wasapi_lock = SRWLOCK_INIT;
static WasapiSink* g_wasapi_active = NULL;

static void WasapiSink_Stop(AmbSink* base);

static void WasapiSink_ReleaseClient(WasapiSink* sink)
{
    if (!sink->client) return;
    if (sink->own_client) {
        sink->fns.Reset(sink->client);
        sink->client->lpVtbl->Release(sink->client);
    }
    sink->client = NULL;
    sink->own_client = FALSE;
}

static HRESULT WasapiSink_CreateClient(IAudioClient** out)
{
    *out = NULL;
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) return hr;
    hr = en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) {
        hr = Hooks_ActivateDevice(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL,
                                  (void**)out);
    }
    if (dev) dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
    return hr;
}

static void WasapiSink_ClosePrevious(WasapiSink* self)
{
    WasapiSink* prev = NULL;
    AcquireSRWLockExclusive(&g_wasapi_lock);
    prev = g_wasapi_active;
    if (prev == self) prev = NULL;
    g_wasapi_active = NULL;
    ReleaseSRWLockExclusive(&g_wasapi_lock);

    if (prev) {
        LOGI("WASAPI exclusive: closing previous stream before opening a new one");
        WasapiSink_Stop(&prev->base);
        WasapiSink_ReleaseClient(prev);
        if (prev->conv) {
            free(prev->conv);
            prev->conv = NULL;
            prev->conv_bytes = 0;
        }
    }
}

static HRESULT WasapiSink_Open(AmbSink* base, Stream* stream, const AmbAudioFormat* fmt)
{
    WasapiSink* sink = (WasapiSink*)base;
    sink->stream = stream;
    sink->app_fmt = *fmt;
    sink->fmt = *fmt;

    WasapiSink_ClosePrevious(sink);

    IAudioClient* dedicated = NULL;
    HRESULT chr = WasapiSink_CreateClient(&dedicated);
    if (SUCCEEDED(chr) && dedicated) {
        sink->client = dedicated;
        sink->own_client = TRUE;
    } else {
        LOGW("WASAPI exclusive: dedicated client activation failed (0x%08lx), using app client",
             (unsigned long)chr);
        sink->client = stream->client;
        sink->own_client = FALSE;
    }

    AmbAudioFormat chosen;
    if (!Wasapi_PickExclusive(sink->client, &sink->fns, fmt, &chosen)) {
        char text[128];
        Format_ToString(fmt, text, sizeof(text));
        LOGW("WASAPI exclusive: no supported device format for %s", text);
        WasapiSink_ReleaseClient(sink);
        return E_FAIL;
    }
    sink->fmt = chosen;

    if (!Format_Equal(&sink->app_fmt, &sink->fmt)) {
        if (!(sink->app_fmt.is_float && sink->app_fmt.bits == 32) ||
            sink->fmt.is_float) {
            char a[128], b[128];
            Format_ToString(&sink->app_fmt, a, sizeof(a));
            Format_ToString(&sink->fmt, b, sizeof(b));
            LOGE("WASAPI exclusive: cannot convert app format %s to %s", a, b);
            WasapiSink_ReleaseClient(sink);
            return E_FAIL;
        }
        sink->converting = TRUE;
    }

    sink->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!sink->event) {
        WasapiSink_ReleaseClient(sink);
        return E_FAIL;
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    HRESULT hr = sink->fns.GetDevicePeriod(sink->client, &default_period, &min_period);
    if (FAILED(hr) || min_period <= 0) {
        min_period = default_period > 0 ? default_period : 100000;
    }

    REFERENCE_TIME hns = (REFERENCE_TIME)g_cfg.wasapi_buffer_ms * 10000;
    if (hns < min_period * 2) hns = min_period * 2;
    if (hns > 20000000) hns = 20000000;
    hns = min_period * ((hns + min_period - 1) / min_period);

    hr = sink->fns.Initialize(sink->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hns, hns,
                               &sink->fmt.wfx.Format, NULL);
    if (SUCCEEDED(hr)) {
        sink->event_mode = TRUE;
    } else {
        LOGD("WASAPI: event-driven exclusive init failed (0x%08lx), retrying polling",
             (unsigned long)hr);
        hr = sink->fns.Initialize(sink->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   0, hns, 0, &sink->fmt.wfx.Format, NULL);
        sink->event_mode = FALSE;
    }
    if (FAILED(hr)) {
        LOGE("WASAPI: exclusive Initialize failed (hr=0x%08lx)", (unsigned long)hr);
        CloseHandle(sink->event);
        sink->event = NULL;
        WasapiSink_ReleaseClient(sink);
        return hr;
    }

    UINT32 frames = 0;
    hr = sink->fns.GetBufferSize(sink->client, &frames);
    if (FAILED(hr) || frames == 0) {
        LOGE("WASAPI: GetBufferSize failed (hr=0x%08lx)", (unsigned long)hr);
        sink->fns.Reset(sink->client);
        CloseHandle(sink->event);
        sink->event = NULL;
        WasapiSink_ReleaseClient(sink);
        return E_FAIL;
    }

    if (sink->event_mode) {
        hr = sink->fns.SetEventHandle(sink->client, sink->event);
        if (FAILED(hr)) {
            LOGE("WASAPI: SetEventHandle failed (hr=0x%08lx)", (unsigned long)hr);
            sink->fns.Reset(sink->client);
            CloseHandle(sink->event);
            sink->event = NULL;
            WasapiSink_ReleaseClient(sink);
            return hr;
        }
    }

    hr = sink->fns.GetService(sink->client, &IID_IAudioRenderClient, (void**)&sink->render);
    if (FAILED(hr) || !sink->render) {
        LOGE("WASAPI: GetService(IAudioRenderClient) failed (hr=0x%08lx)", (unsigned long)hr);
        sink->fns.Reset(sink->client);
        CloseHandle(sink->event);
        sink->event = NULL;
        WasapiSink_ReleaseClient(sink);
        return E_FAIL;
    }

    sink->frames = frames;
    base->period_frames = frames;
    base->bitperfect = !sink->converting;

    AcquireSRWLockExclusive(&g_wasapi_lock);
    g_wasapi_active = sink;
    ReleaseSRWLockExclusive(&g_wasapi_lock);

    char app[128], out[128];
    Format_ToString(&sink->app_fmt, app, sizeof(app));
    Format_ToString(&sink->fmt, out, sizeof(out));
    LOGI("WASAPI exclusive sink: %s -> %s, %lu frames (%.2f ms), %s, %s, buffer=%lld x100ns",
         app, out, (unsigned long)frames,
         (double)frames * 1000.0 / (double)sink->fmt.rate,
         sink->event_mode ? "event-driven" : "polling",
         sink->converting ? "float32 conversion" : "bit-perfect",
         (long long)hns);
    return S_OK;
}

static void WasapiSink_SetRing(AmbSink* base, AmbRing* ring)
{
    AMB_UNUSED(base);
    AMB_UNUSED(ring);
}

static HRESULT WasapiSink_Start(AmbSink* base)
{
    WasapiSink* sink = (WasapiSink*)base;
    if (sink->started) return S_OK;
    if (!sink->client) return E_FAIL;

    HRESULT hr = sink->fns.Start(sink->client);
    if (FAILED(hr)) {
        LOGE("WASAPI: Start failed (hr=0x%08lx)", (unsigned long)hr);
        return hr;
    }

    InterlockedExchange(&sink->stop, 0);
    sink->thread = CreateThread(NULL, 0, WasapiFeeder, sink, 0, NULL);
    if (!sink->thread) {
        sink->fns.Stop(sink->client);
        return E_FAIL;
    }
    sink->started = TRUE;
    if (sink->stream->app_event) SetEvent(sink->stream->app_event);
    LOGI("WASAPI: streaming (exclusive)");
    return S_OK;
}

static void WasapiSink_Stop(AmbSink* base)
{
    WasapiSink* sink = (WasapiSink*)base;
    if (!sink->started) return;

    InterlockedExchange(&sink->stop, 1);
    if (sink->client) sink->fns.Stop(sink->client);
    if (sink->event) SetEvent(sink->event);
    if (sink->thread) {
        WaitForSingleObject(sink->thread, 3000);
        CloseHandle(sink->thread);
        sink->thread = NULL;
    }
    sink->started = FALSE;
    if (sink->underruns > 0) {
        LOGW("WASAPI: stream stopped, %ld underruns", sink->underruns);
    }
    if (sink->stream->app_event) SetEvent(sink->stream->app_event);
}

static void WasapiSink_Close(AmbSink* base)
{
    WasapiSink* sink = (WasapiSink*)base;

    AcquireSRWLockExclusive(&g_wasapi_lock);
    if (g_wasapi_active == sink) g_wasapi_active = NULL;
    ReleaseSRWLockExclusive(&g_wasapi_lock);

    WasapiSink_Stop(base);

    if (sink->render) {
        sink->render->lpVtbl->Release(sink->render);
        sink->render = NULL;
    }
    if (sink->event) {
        CloseHandle(sink->event);
        sink->event = NULL;
    }
    if (sink->conv) {
        free(sink->conv);
        sink->conv = NULL;
        sink->conv_bytes = 0;
    }
    WasapiSink_ReleaseClient(sink);
}

BOOL Sink_WasapiCreate(Stream* stream, const AmbAudioFormat* fmt,
                       const RealClientFns* real_fns, AmbSink** out_sink)
{
    if (!real_fns) return FALSE;
    WasapiSink* sink = (WasapiSink*)calloc(1, sizeof(*sink));
    if (!sink) return FALSE;

    sink->base.name = "wasapi-exclusive";
    sink->base.Open = WasapiSink_Open;
    sink->base.SetRing = WasapiSink_SetRing;
    sink->base.Start = WasapiSink_Start;
    sink->base.Stop = WasapiSink_Stop;
    sink->base.Close = WasapiSink_Close;
    sink->fns = *real_fns;

    HRESULT hr = sink->base.Open(&sink->base, stream, fmt);
    if (FAILED(hr)) {
        free(sink);
        return FALSE;
    }
    *out_sink = &sink->base;
    return TRUE;
}

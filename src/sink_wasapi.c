#include "sink.h"
#include "config.h"
#include "log.h"

#include <math.h>

BOOL Wasapi_PickExclusive(IAudioClient* client, const RealClientFns* fns,
                          const AmbAudioFormat* mix, AmbAudioFormat* out);
void Wasapi_EnableRaw(IAudioClient* client);

typedef struct WasapiSink {
    AmbSink base;
    Stream* stream;
    IAudioClient* client;
    const RealClientFns* fns;
    IAudioRenderClient* render;
    HANDLE event;
    HANDLE thread;
    volatile LONG stop;
    BOOL event_mode;
    BOOL started;
    UINT32 frames;
    AmbAudioFormat fmt;
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
    if (hr == S_FALSE && closest) {
        AmbAudioFormat c;
        if (Format_Parse(&c, closest) && c.channels == cand->channels) {
            *out = c;
            CoTaskMemFree(closest);
            return TRUE;
        }
    }
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

        UINT32 padding = 0;
        if (FAILED(sink->fns->GetCurrentPadding(sink->client, &padding))) continue;
        if (padding >= sink->frames) continue;

        UINT32 avail = sink->frames - padding;
        BYTE* data = NULL;
        if (FAILED(sink->render->lpVtbl->GetBuffer(sink->render, avail, &data)) || !data) {
            continue;
        }
        uint32_t got = Ring_ReadZero(&s->ring, data, avail);
        if (got < avail) {
            sink->underruns++;
        }
        ApplyVolume(sink, data, avail);
        sink->render->lpVtbl->ReleaseBuffer(sink->render, avail, 0);

        if (got) InterlockedExchangeAdd64(&s->frames_played, (LONG64)got);
        if (s->app_event) SetEvent(s->app_event);
    }
    return 0;
}

static HRESULT WasapiSink_Open(AmbSink* base, Stream* stream, const AmbAudioFormat* fmt)
{
    WasapiSink* sink = (WasapiSink*)base;
    sink->stream = stream;
    sink->client = stream->client;
    sink->fmt = *fmt;

    WAVEFORMATEX* closest = NULL;
    HRESULT hr = sink->fns->IsFormatSupported(sink->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                              &fmt->wfx.Format, &closest);
    if (closest) CoTaskMemFree(closest);
    if (hr != S_OK) {
        char text[128];
        Format_ToString(fmt, text, sizeof(text));
        LOGW("WASAPI exclusive: format not supported natively (%s, hr=0x%08lx)",
             text, (unsigned long)hr);
        return E_FAIL;
    }

    sink->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!sink->event) return E_FAIL;

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    hr = sink->fns->GetDevicePeriod(sink->client, &default_period, &min_period);
    if (FAILED(hr) || min_period <= 0) {
        min_period = default_period > 0 ? default_period : 100000;
    }

    REFERENCE_TIME hns = (REFERENCE_TIME)g_cfg.wasapi_buffer_ms * 10000;
    if (hns < min_period * 4) hns = min_period * 4;
    if (hns < min_period * 2) hns = min_period * 2;
    if (hns > 20000000) hns = 20000000;
    hns = min_period * ((hns + min_period - 1) / min_period);

    hr = sink->fns->Initialize(sink->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hns, 0,
                               &fmt->wfx.Format, NULL);
    if (SUCCEEDED(hr)) {
        sink->event_mode = TRUE;
    } else {
        LOGD("WASAPI: event-driven exclusive init failed (0x%08lx), retrying polling",
             (unsigned long)hr);
        hr = sink->fns->Initialize(sink->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   0, hns, 0, &fmt->wfx.Format, NULL);
        sink->event_mode = FALSE;
    }
    if (FAILED(hr)) {
        LOGE("WASAPI: exclusive Initialize failed (hr=0x%08lx)", (unsigned long)hr);
        CloseHandle(sink->event);
        sink->event = NULL;
        return hr;
    }

    UINT32 frames = 0;
    hr = sink->fns->GetBufferSize(sink->client, &frames);
    if (FAILED(hr) || frames == 0) {
        LOGE("WASAPI: GetBufferSize failed (hr=0x%08lx)", (unsigned long)hr);
        sink->fns->Reset(sink->client);
        CloseHandle(sink->event);
        sink->event = NULL;
        return E_FAIL;
    }

    if (sink->event_mode) {
        hr = sink->fns->SetEventHandle(sink->client, sink->event);
        if (FAILED(hr)) {
            LOGE("WASAPI: SetEventHandle failed (hr=0x%08lx)", (unsigned long)hr);
            sink->fns->Reset(sink->client);
            CloseHandle(sink->event);
            sink->event = NULL;
            return hr;
        }
    }

    hr = sink->fns->GetService(sink->client, &IID_IAudioRenderClient, (void**)&sink->render);
    if (FAILED(hr) || !sink->render) {
        LOGE("WASAPI: GetService(IAudioRenderClient) failed (hr=0x%08lx)", (unsigned long)hr);
        sink->fns->Reset(sink->client);
        CloseHandle(sink->event);
        sink->event = NULL;
        return E_FAIL;
    }

    sink->frames = frames;
    base->period_frames = frames;

    char text[128];
    Format_ToString(fmt, text, sizeof(text));
    LOGI("WASAPI exclusive sink: %s, %lu frames (%.2f ms), %s, buffer=%lld x100ns",
         text, (unsigned long)frames,
         (double)frames * 1000.0 / (double)fmt->rate,
         sink->event_mode ? "event-driven" : "polling",
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

    HRESULT hr = sink->fns->Start(sink->client);
    if (FAILED(hr)) {
        LOGE("WASAPI: Start failed (hr=0x%08lx)", (unsigned long)hr);
        return hr;
    }

    InterlockedExchange(&sink->stop, 0);
    sink->thread = CreateThread(NULL, 0, WasapiFeeder, sink, 0, NULL);
    if (!sink->thread) {
        sink->fns->Stop(sink->client);
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
    sink->fns->Stop(sink->client);
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
    WasapiSink_Stop(base);

    if (sink->render) {
        sink->render->lpVtbl->Release(sink->render);
        sink->render = NULL;
    }
    if (sink->event) {
        CloseHandle(sink->event);
        sink->event = NULL;
    }
    if (sink->client) {
        sink->fns->Reset(sink->client);
    }
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
    sink->fns = real_fns;

    HRESULT hr = sink->base.Open(&sink->base, stream, fmt);
    if (FAILED(hr)) {
        free(sink);
        return FALSE;
    }
    *out_sink = &sink->base;
    return TRUE;
}

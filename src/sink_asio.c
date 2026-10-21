#include "sink.h"
#include "asio_host.h"
#include "asio_iface.h"
#include "config.h"
#include "log.h"

typedef struct AsioSink {
    AmbSink base;
    Stream* stream;
    AmbAudioFormat fmt;
    AsioFormatInfo info;
} AsioSink;

BOOL Asio_MapNativeFormat(long type, UINT32 rate, UINT32 channels, AmbAudioFormat* out)
{
    switch (type) {
    case ASIOSTFloat32LSB:
        Format_Make(out, rate, channels, AMB_SAMPLE_F32, 32);
        return TRUE;
    case ASIOSTInt32LSB:
        Format_Make(out, rate, channels, AMB_SAMPLE_S32, 32);
        return TRUE;
    case ASIOSTInt32LSB24:
        Format_Make(out, rate, channels, AMB_SAMPLE_S32, 24);
        return TRUE;
    case ASIOSTInt32LSB20:
        Format_Make(out, rate, channels, AMB_SAMPLE_S32, 20);
        return TRUE;
    case ASIOSTInt32LSB18:
        Format_Make(out, rate, channels, AMB_SAMPLE_S32, 18);
        return TRUE;
    case ASIOSTInt32LSB16:
        Format_Make(out, rate, channels, AMB_SAMPLE_S32, 16);
        return TRUE;
    case ASIOSTInt24LSB:
        Format_Make(out, rate, channels, AMB_SAMPLE_S24, 24);
        return TRUE;
    case ASIOSTInt16LSB:
        Format_Make(out, rate, channels, AMB_SAMPLE_S16, 16);
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL Asio_TypeAllowsFloatConversion(long type)
{
    switch (type) {
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB24:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB16:
    case ASIOSTInt24LSB:
    case ASIOSTInt16LSB:
        return TRUE;
    default:
        return FALSE;
    }
}

static HRESULT AsioSink_Open(AmbSink* base, Stream* stream, const AmbAudioFormat* fmt)
{
    AsioSink* sink = (AsioSink*)base;
    sink->stream = stream;
    sink->fmt = *fmt;

    HRESULT hr = AsioHost_OpenStream((double)fmt->rate, (int)fmt->channels,
                                     g_cfg.asio_buffer_frames, &sink->info);
    if (FAILED(hr)) return hr;

    if (sink->info.rate > 0 && sink->info.rate != (double)fmt->rate) {
        LOGE("ASIO: driver is running at %.0f Hz but the app requested %lu Hz",
             sink->info.rate, (unsigned long)fmt->rate);
        AsioHost_CloseStream();
        return E_FAIL;
    }

    AmbAudioFormat native;
    if (!Asio_MapNativeFormat(sink->info.sample_type, fmt->rate, fmt->channels, &native)) {
        LOGE("ASIO: unsupported sample type %s (%ld)",
             Asio_SampleTypeName(sink->info.sample_type), sink->info.sample_type);
        AsioHost_CloseStream();
        return E_FAIL;
    }

    if (Format_Equal(fmt, &native)) {
        base->bitperfect = TRUE;
        LOGI("ASIO: bit-perfect path, app format matches driver format (%s)",
             Asio_SampleTypeName(sink->info.sample_type));
    } else if (fmt->is_float && fmt->bits == 32 &&
               Asio_TypeAllowsFloatConversion(sink->info.sample_type)) {
        base->bitperfect = FALSE;
        LOGI("ASIO: float32 -> %s conversion (no resampling; 16/24-bit content is "
             "reconstructed exactly)",
             Asio_SampleTypeName(sink->info.sample_type));
    } else {
        char want[128], have[128];
        Format_ToString(fmt, want, sizeof(want));
        Format_ToString(&native, have, sizeof(have));
        LOGE("ASIO: app format %s cannot feed driver format %s without resampling",
             want, have);
        AsioHost_CloseStream();
        return E_FAIL;
    }

    base->period_frames = (uint32_t)sink->info.buffer_frames;
    return S_OK;
}

static void AsioSink_SetRing(AmbSink* base, AmbRing* ring)
{
    AsioSink* sink = (AsioSink*)base;
    Stream* s = sink->stream;

    AsioConsumer consumer;
    ZeroMemory(&consumer, sizeof(consumer));
    consumer.ring = ring;
    consumer.frame_bytes = sink->fmt.frame_bytes;
    consumer.channels = sink->fmt.channels;
    consumer.event = &s->app_event;
    consumer.frames_played = &s->frames_played;
    consumer.gain = &s->gain;
    consumer.mute = &s->mute;
    consumer.is_float = sink->fmt.is_float;
    consumer.volume_enabled = g_cfg.volume_control;
    consumer.asio_type = sink->info.sample_type;
    AsioHost_SetConsumer(&consumer);

    LOGI("ASIO sink: %s, %lu frames, ring %lu frames, volume=%s",
         Asio_SampleTypeName(sink->info.sample_type),
         (unsigned long)sink->info.buffer_frames,
         (unsigned long)ring->cap_frames,
         consumer.volume_enabled ? "app" : "unity");
}

static HRESULT AsioSink_Start(AmbSink* base)
{
    AMB_UNUSED(base);
    return AsioHost_Start();
}

static void AsioSink_Stop(AmbSink* base)
{
    AMB_UNUSED(base);
    AsioHost_Stop();
    LONG64 frames = 0;
    LONG underruns = 0;
    AsioHost_GetStats(&frames, &underruns);
    if (underruns > 0) {
        LOGW("ASIO: stream stopped, %lld frames played, %ld underruns",
             (long long)frames, (long)underruns);
    } else {
        LOGI("ASIO: stream stopped, %lld frames played", (long long)frames);
    }
}

static void AsioSink_Close(AmbSink* base)
{
    AsioSink* sink = (AsioSink*)base;
    AMB_UNUSED(sink);
    AsioHost_CloseStream();
    AsioHost_Close();
}

BOOL Sink_AsioCreate(Stream* stream, const AmbAudioFormat* fmt, AmbSink** out_sink)
{
    AMB_UNUSED(stream);
    AsioSink* sink = (AsioSink*)calloc(1, sizeof(*sink));
    if (!sink) return FALSE;

    sink->base.name = "asio";
    sink->base.Open = AsioSink_Open;
    sink->base.SetRing = AsioSink_SetRing;
    sink->base.Start = AsioSink_Start;
    sink->base.Stop = AsioSink_Stop;
    sink->base.Close = AsioSink_Close;

    HRESULT hr = sink->base.Open(&sink->base, stream, fmt);
    if (FAILED(hr)) {
        free(sink);
        return FALSE;
    }
    *out_sink = &sink->base;
    return TRUE;
}

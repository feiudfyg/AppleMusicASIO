#pragma once

#include "stream.h"

typedef struct AmbSink {
    const char* name;
    HRESULT (*Open)(struct AmbSink* sink, Stream* stream, const AmbAudioFormat* fmt);
    void (*SetRing)(struct AmbSink* sink, AmbRing* ring);
    HRESULT (*Start)(struct AmbSink* sink);
    void (*Stop)(struct AmbSink* sink);
    void (*Close)(struct AmbSink* sink);
    uint32_t period_frames;
    BOOL bitperfect;
} AmbSink;

BOOL Sink_AsioCreate(Stream* stream, const AmbAudioFormat* fmt, AmbSink** out_sink);
BOOL Sink_WasapiCreate(Stream* stream, const AmbAudioFormat* fmt,
                       const RealClientFns* real_fns, AmbSink** out_sink);
BOOL Asio_MapNativeFormat(long type, UINT32 rate, UINT32 channels, AmbAudioFormat* out);
BOOL Wasapi_PickExclusive(IAudioClient* client, const RealClientFns* fns,
                          const AmbAudioFormat* mix, AmbAudioFormat* out);
void Wasapi_EnableRaw(IAudioClient* client);

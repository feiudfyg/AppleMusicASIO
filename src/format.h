#pragma once

#include "common.h"

typedef enum AmbSampleKind {
    AMB_SAMPLE_F32 = 0,
    AMB_SAMPLE_S16,
    AMB_SAMPLE_S24,
    AMB_SAMPLE_S32
} AmbSampleKind;

typedef struct AmbAudioFormat {
    WAVEFORMATEXTENSIBLE wfx;
    UINT32 rate;
    UINT32 channels;
    UINT32 bits;
    UINT32 valid_bits;
    UINT32 frame_bytes;
    UINT32 bytes_per_sample;
    BOOL is_float;
    AmbSampleKind kind;
} AmbAudioFormat;

BOOL Format_Parse(AmbAudioFormat* f, const WAVEFORMATEX* wf);
void Format_Make(AmbAudioFormat* f, UINT32 rate, UINT32 channels,
                 AmbSampleKind kind, UINT32 valid_bits);
void Format_Copy(AmbAudioFormat* dst, const AmbAudioFormat* src);
WAVEFORMATEX* Format_AllocCo(const AmbAudioFormat* f);
BOOL Format_Equal(const AmbAudioFormat* a, const AmbAudioFormat* b);
const char* Format_KindName(AmbSampleKind kind);
void Format_ToString(const AmbAudioFormat* f, char* buf, size_t count);

#pragma once

#include "common.h"

typedef struct AmbRing {
    uint8_t* data;
    uint32_t cap_frames;
    uint32_t frame_bytes;
    volatile LONG64 write_frame;
    volatile LONG64 read_frame;
} AmbRing;

BOOL Ring_Init(AmbRing* ring, uint32_t cap_frames, uint32_t frame_bytes);
void Ring_Free(AmbRing* ring);
void Ring_Clear(AmbRing* ring);
uint32_t Ring_Fill(const AmbRing* ring);
uint32_t Ring_Space(const AmbRing* ring);
uint32_t Ring_Write(AmbRing* ring, const void* src, uint32_t frames);
uint32_t Ring_Read(AmbRing* ring, void* dst, uint32_t frames);
uint32_t Ring_ReadZero(AmbRing* ring, void* dst, uint32_t frames);

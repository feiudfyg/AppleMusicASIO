#include "ring.h"

BOOL Ring_Init(AmbRing* ring, uint32_t cap_frames, uint32_t frame_bytes)
{
    if (!ring || frame_bytes == 0) return FALSE;
    ZeroMemory(ring, sizeof(*ring));

    uint32_t cap = Amb_NextPow2(cap_frames ? cap_frames : 1024);
    if (cap > (1u << 20)) cap = 1u << 20;

    ring->data = (uint8_t*)calloc(cap, frame_bytes);
    if (!ring->data) return FALSE;
    ring->cap_frames = cap;
    ring->frame_bytes = frame_bytes;
    return TRUE;
}

void Ring_Free(AmbRing* ring)
{
    if (!ring) return;
    free(ring->data);
    ring->data = NULL;
    ring->cap_frames = 0;
    ring->frame_bytes = 0;
    ring->write_frame = 0;
    ring->read_frame = 0;
}

void Ring_Clear(AmbRing* ring)
{
    if (!ring) return;
    ring->read_frame = ring->write_frame;
}

uint32_t Ring_Fill(const AmbRing* ring)
{
    if (!ring || !ring->data) return 0;
    LONG64 w = ring->write_frame;
    LONG64 r = ring->read_frame;
    LONG64 fill = w - r;
    if (fill < 0) fill = 0;
    if (fill > (LONG64)ring->cap_frames) fill = ring->cap_frames;
    return (uint32_t)fill;
}

uint32_t Ring_Space(const AmbRing* ring)
{
    if (!ring || !ring->data) return 0;
    uint32_t fill = Ring_Fill(ring);
    return ring->cap_frames - fill;
}

uint32_t Ring_Write(AmbRing* ring, const void* src, uint32_t frames)
{
    if (!ring || !ring->data || !src || frames == 0) return 0;
    uint32_t space = Ring_Space(ring);
    if (frames > space) frames = space;
    if (frames == 0) return 0;

    const uint8_t* s = (const uint8_t*)src;
    uint32_t offset = (uint32_t)(ring->write_frame & (LONG64)(ring->cap_frames - 1));
    uint32_t first = ring->cap_frames - offset;
    if (first > frames) first = frames;
    memcpy(ring->data + (size_t)offset * ring->frame_bytes, s,
           (size_t)first * ring->frame_bytes);
    if (frames > first) {
        memcpy(ring->data, s + (size_t)first * ring->frame_bytes,
               (size_t)(frames - first) * ring->frame_bytes);
    }
    MemoryBarrier();
    ring->write_frame += frames;
    return frames;
}

uint32_t Ring_Read(AmbRing* ring, void* dst, uint32_t frames)
{
    if (!ring || !ring->data || !dst || frames == 0) return 0;
    uint32_t fill = Ring_Fill(ring);
    if (frames > fill) frames = fill;
    if (frames == 0) return 0;

    uint8_t* d = (uint8_t*)dst;
    uint32_t offset = (uint32_t)(ring->read_frame & (LONG64)(ring->cap_frames - 1));
    uint32_t first = ring->cap_frames - offset;
    if (first > frames) first = frames;
    memcpy(d, ring->data + (size_t)offset * ring->frame_bytes,
           (size_t)first * ring->frame_bytes);
    if (frames > first) {
        memcpy(d + (size_t)first * ring->frame_bytes, ring->data,
               (size_t)(frames - first) * ring->frame_bytes);
    }
    MemoryBarrier();
    ring->read_frame += frames;
    return frames;
}

uint32_t Ring_ReadZero(AmbRing* ring, void* dst, uint32_t frames)
{
    uint32_t got = Ring_Read(ring, dst, frames);
    if (got < frames && ring && ring->data) {
        memset((uint8_t*)dst + (size_t)got * ring->frame_bytes, 0,
               (size_t)(frames - got) * ring->frame_bytes);
    }
    return got;
}

#include "format.h"

static const GUID SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID SUBTYPE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

const char* Format_KindName(AmbSampleKind kind)
{
    switch (kind) {
    case AMB_SAMPLE_F32: return "float32";
    case AMB_SAMPLE_S16: return "pcm16";
    case AMB_SAMPLE_S24: return "pcm24";
    case AMB_SAMPLE_S32: return "pcm32";
    default: return "?";
    }
}

void Format_Make(AmbAudioFormat* f, UINT32 rate, UINT32 channels,
                 AmbSampleKind kind, UINT32 valid_bits)
{
    ZeroMemory(f, sizeof(*f));
    f->rate = rate;
    f->channels = channels;
    f->kind = kind;
    f->is_float = (kind == AMB_SAMPLE_F32);

    switch (kind) {
    case AMB_SAMPLE_F32:
        f->bits = 32;
        break;
    case AMB_SAMPLE_S16:
        f->bits = 16;
        break;
    case AMB_SAMPLE_S24:
        f->bits = 24;
        break;
    case AMB_SAMPLE_S32:
        f->bits = 32;
        break;
    default:
        f->bits = 32;
        break;
    }

    if (valid_bits == 0 || valid_bits > f->bits) valid_bits = f->bits;
    f->valid_bits = valid_bits;
    f->bytes_per_sample = f->bits / 8;
    f->frame_bytes = f->bytes_per_sample * channels;

    WAVEFORMATEXTENSIBLE* w = &f->wfx;
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = (WORD)channels;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = (WORD)f->bits;
    w->Format.nBlockAlign = (WORD)f->frame_bytes;
    w->Format.nAvgBytesPerSec = rate * f->frame_bytes;
    w->Format.cbSize = 22;
    w->Samples.wValidBitsPerSample = (WORD)f->valid_bits;
    w->Samples.wReserved = 0;

    switch (channels) {
    case 1:
        w->dwChannelMask = SPEAKER_FRONT_CENTER;
        break;
    case 2:
        w->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        break;
    default:
        w->dwChannelMask = 0;
        break;
    }

    w->SubFormat = f->is_float ? SUBTYPE_FLOAT : SUBTYPE_PCM;
}

BOOL Format_Parse(AmbAudioFormat* f, const WAVEFORMATEX* wf)
{
    if (!f || !wf) return FALSE;
    ZeroMemory(f, sizeof(*f));

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wf->cbSize < 22) return FALSE;
        const WAVEFORMATEXTENSIBLE* w = (const WAVEFORMATEXTENSIBLE*)wf;
        if (IsEqualGUID(&w->SubFormat, &SUBTYPE_PCM)) {
            f->is_float = FALSE;
        } else if (IsEqualGUID(&w->SubFormat, &SUBTYPE_FLOAT)) {
            f->is_float = TRUE;
        } else {
            return FALSE;
        }
        f->wfx = *w;
        f->valid_bits = w->Samples.wValidBitsPerSample;
    } else if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        f->is_float = FALSE;
        ZeroMemory(&f->wfx, sizeof(f->wfx));
        f->wfx.Format = *wf;
        f->wfx.Format.cbSize = 0;
        f->valid_bits = wf->wBitsPerSample;
    } else if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        f->is_float = TRUE;
        ZeroMemory(&f->wfx, sizeof(f->wfx));
        f->wfx.Format = *wf;
        f->wfx.Format.cbSize = 0;
        f->valid_bits = wf->wBitsPerSample;
    } else {
        return FALSE;
    }

    f->rate = wf->nSamplesPerSec;
    f->channels = wf->nChannels;
    f->bits = wf->wBitsPerSample;
    if (f->valid_bits == 0 || f->valid_bits > f->bits) f->valid_bits = f->bits;

    if (f->rate < 8000 || f->rate > 768000) return FALSE;
    if (f->channels < 1 || f->channels > AMB_MAX_CHANNELS) return FALSE;
    if (f->bits != 16 && f->bits != 24 && f->bits != 32) return FALSE;
    if (f->is_float && f->bits != 32) return FALSE;

    f->bytes_per_sample = f->bits / 8;
    f->frame_bytes = wf->nBlockAlign;
    if (f->frame_bytes != f->bytes_per_sample * f->channels) return FALSE;

    if (f->is_float) {
        f->kind = AMB_SAMPLE_F32;
    } else if (f->bits == 16) {
        f->kind = AMB_SAMPLE_S16;
    } else if (f->bits == 24) {
        f->kind = AMB_SAMPLE_S24;
    } else {
        f->kind = AMB_SAMPLE_S32;
    }
    return TRUE;
}

void Format_Copy(AmbAudioFormat* dst, const AmbAudioFormat* src)
{
    if (dst && src) *dst = *src;
}

WAVEFORMATEX* Format_AllocCo(const AmbAudioFormat* f)
{
    WAVEFORMATEX* p = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
    if (!p) return NULL;
    memcpy(p, &f->wfx, sizeof(WAVEFORMATEXTENSIBLE));
    p->cbSize = 22;
    return p;
}

BOOL Format_Equal(const AmbAudioFormat* a, const AmbAudioFormat* b)
{
    return a->rate == b->rate &&
           a->channels == b->channels &&
           a->bits == b->bits &&
           a->valid_bits == b->valid_bits &&
           a->is_float == b->is_float &&
           a->frame_bytes == b->frame_bytes;
}

void Format_ToString(const AmbAudioFormat* f, char* buf, size_t count)
{
    _snprintf_s(buf, count, _TRUNCATE, "%lu Hz / %lu ch / %s %lu-bit (valid %lu)",
                (unsigned long)f->rate, (unsigned long)f->channels,
                f->is_float ? "float" : "pcm",
                (unsigned long)f->bits, (unsigned long)f->valid_bits);
}

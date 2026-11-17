#include "common.h"
#include "log.h"
#include "app_hooks.h"
#include "format.h"
#include "hooks.h"
#include <tlhelp32.h>

#define AMB_TAP_DIAGNOSTIC 0
#include <MinHook.h>

typedef int32_t OSStatus;

typedef struct AudioStreamBasicDescription {
    double mSampleRate;
    uint32_t mFormatID;
    uint32_t mFormatFlags;
    uint32_t mBytesPerPacket;
    uint32_t mFramesPerPacket;
    uint32_t mBytesPerFrame;
    uint32_t mChannelsPerFrame;
    uint32_t mBitsPerChannel;
    uint32_t mReserved;
} AudioStreamBasicDescription;

typedef OSStatus (*PFN_AudioQueueNewOutput)(const AudioStreamBasicDescription*, void*, void*,
                                            void*, void*, uint32_t, void**);
typedef struct AudioQueueBuffer {
    uint32_t mAudioDataBytesCapacity;
    void* mAudioData;
    uint32_t mAudioDataByteSize;
    void* mUserData;
    uint32_t mPacketDescriptionCapacity;
    void* mPacketDescriptions;
    uint32_t mPacketDescriptionCount;
} AudioQueueBuffer;
typedef OSStatus (*PFN_AudioQueueEnqueueBufferWithParameters)(
    void*, AudioQueueBuffer*, uint32_t, const void*, uint32_t, uint32_t, uint32_t,
    const void*, const void*, void*);
typedef OSStatus (*PFN_AudioQueueStart)(void*, const void*);
typedef OSStatus (*PFN_AudioQueueStop)(void*, uint32_t);
typedef OSStatus (*PFN_AudioQueueProcessingTapCallback)(void*, void*, uint32_t, void*, uint32_t*,
                                                        uint32_t*, void*);
typedef OSStatus (*PFN_AudioQueueProcessingTapGetSourceAudio)(void*, uint32_t, void*, uint32_t*,
                                                              uint32_t*, void*);
typedef OSStatus (*PFN_AudioQueueProcessingTapGetQueueTime)(void*, double*, uint32_t*);
typedef OSStatus (*PFN_AudioQueueProcessingTapNew)(void*, PFN_AudioQueueProcessingTapCallback,
                                                   void*, uint32_t, uint32_t*,
                                                   AudioStreamBasicDescription*, void**);
typedef OSStatus (*PFN_AudioConverterNew)(const AudioStreamBasicDescription*,
                                          const AudioStreamBasicDescription*, void**);
typedef OSStatus (*PFN_AudioConverterNewSpecific)(const AudioStreamBasicDescription*,
                                                  const AudioStreamBasicDescription*,
                                                  uint32_t, const void*, void**);
typedef OSStatus (*PFN_AudioConverterConvertBuffer)(void*, uint32_t, const void*, uint32_t*,
                                                    void*);
typedef OSStatus (*PFN_AudioConverterFillBuffer)(void*, void*, void*, uint32_t*, void*);
typedef OSStatus (*PFN_AudioConverterComplexInputDataProc)(void*, uint32_t*, void*, void*, void*);
typedef OSStatus (*PFN_AudioConverterFillComplexBuffer)(void*, void*, void*, uint32_t*,
                                                        void*, void*);
typedef OSStatus (*PFN_AudioConverterGetProperty)(void*, uint32_t, uint32_t*, void*);
typedef OSStatus (*PFN_AudioConverterDispose)(void*);
typedef OSStatus (*PFN_AudioUnitRender)(void*, uint32_t*, const void*, uint32_t, uint32_t, void*);
typedef OSStatus (*PFN_AudioUnitSetProperty)(void*, uint32_t, uint32_t, uint32_t, const void*,
                                             uint32_t);

typedef OSStatus (*PFN_AURenderCallback)(void*, uint32_t*, const void*, uint32_t, uint32_t,
                                          void*);

typedef struct AURenderCallbackStruct {
    PFN_AURenderCallback input_proc;
    void* input_ref;
} AURenderCallbackStruct;

typedef struct AudioBuffer {
    uint32_t number_channels;
    uint32_t data_byte_size;
    void* data;
} AudioBuffer;

typedef struct AudioBufferList {
    uint32_t number_buffers;
    AudioBuffer buffers[1];
} AudioBufferList;

typedef struct CaptureEntry {
    void* converter;
    AmbAudioFormat format;
    BOOL planar;
} CaptureEntry;

typedef struct UnitTap {
    void* unit;
    PFN_AURenderCallback original;
    void* original_ref;
    AmbAudioFormat format;
    BOOL have_format;
    BOOL used;
    volatile LONG calls;
    volatile LONG64 frames;
    LARGE_INTEGER first_call;
} UnitTap;

typedef struct QueueInfo {
    void* queue;
    AudioStreamBasicDescription format;
    BOOL used;
} QueueInfo;

typedef struct ConverterInfo {
    void* converter;
    AudioStreamBasicDescription src;
    AudioStreamBasicDescription dst;
    volatile LONG convert_calls;
    volatile LONG fill_calls;
    volatile LONG complex_calls;
    BOOL used;
} ConverterInfo;

#define AMB_MAX_CAPTURE_CONVERTERS 16
#define AMB_MAX_QUEUES 16
#define AMB_MAX_CONVERTERS 32
#define AMB_CAPTURE_SCRATCH_FRAMES 32768

static HMODULE g_module = NULL;
static PFN_AudioQueueNewOutput g_queue_new = NULL;
static PFN_AudioQueueStart g_queue_start = NULL;
static PFN_AudioQueueStop g_queue_stop = NULL;
static PFN_AudioQueueEnqueueBufferWithParameters g_queue_enqueue = NULL;
static PFN_AudioQueueProcessingTapNew g_tap_new = NULL;
static PFN_AudioQueueProcessingTapGetSourceAudio g_tap_get_source = NULL;
static PFN_AudioQueueProcessingTapGetQueueTime g_tap_get_queue_time = NULL;
static volatile LONG g_tap_active = 0;
static void* volatile g_active_tap = NULL;
static PFN_AudioConverterNew g_converter_new = NULL;
static PFN_AudioConverterNewSpecific g_converter_specific = NULL;
static PFN_AudioConverterConvertBuffer g_convert_buffer = NULL;
static PFN_AudioConverterFillBuffer g_fill_buffer = NULL;
static PFN_AudioConverterFillComplexBuffer g_fill_complex = NULL;
static PFN_AudioConverterGetProperty g_converter_get_property = NULL;
static PFN_AudioConverterDispose g_converter_dispose = NULL;
static PFN_AudioUnitRender g_unit_render = NULL;
static PFN_AudioUnitSetProperty g_unit_set_property = NULL;
static volatile LONG g_source_rate = 0;
static volatile LONG g_qlac_rate = 0;
static volatile LONG g_switch_pending = 0;
static volatile LONG g_source_codec = 0;
static BOOL g_unit_format_set = FALSE;

static SRWLOCK g_capture_lock = SRWLOCK_INIT;
static SRWLOCK g_unit_tap_lock = SRWLOCK_INIT;
static SRWLOCK g_queue_lock = SRWLOCK_INIT;
static SRWLOCK g_converter_lock = SRWLOCK_INIT;
static CaptureEntry g_capture_entries[AMB_MAX_CAPTURE_CONVERTERS];
static UnitTap g_unit_taps[16];
static QueueInfo g_queues[AMB_MAX_QUEUES];
static ConverterInfo g_converters[AMB_MAX_CONVERTERS];
static void* volatile g_capture_unit = NULL;
static void* volatile g_capture_decoder = NULL;
static volatile LONG g_decoder_capture_active = 0;
static volatile LONG64 g_decoder_last_write = 0;
static double g_capture_rate_error = 1e9;
static int g_capture_count = 0;
static AmbAudioFormat g_capture_format;
static BOOL g_have_capture_format = FALSE;
static float g_capture_scratch[AMB_CAPTURE_SCRATCH_FRAMES * 2];
static void* volatile g_normal_queue = NULL;

static OSStatus CapturedRenderCallback(void* ref, uint32_t* flags, const void* timestamp,
                                       uint32_t bus, uint32_t frames, void* io_data);

static uintptr_t g_seen_modules[256];
static volatile LONG g_seen_count = 0;

static void FormatFourCC(uint32_t id, char out[5])
{
    out[0] = (char)((id >> 24) & 0xFF);
    out[1] = (char)((id >> 16) & 0xFF);
    out[2] = (char)((id >> 8) & 0xFF);
    out[3] = (char)(id & 0xFF);
    out[4] = 0;
    for (int i = 0; i < 4; ++i) {
        if (out[i] < 32 || out[i] > 126) out[i] = '.';
    }
}

static void TrackConverter(void* converter, const AudioStreamBasicDescription* src,
                           const AudioStreamBasicDescription* dst)
{
    if (!converter || !src || !dst) return;
    BOOL added = FALSE;
    AcquireSRWLockExclusive(&g_converter_lock);
    for (int i = 0; i < AMB_MAX_CONVERTERS; ++i) {
        if (!g_converters[i].used || g_converters[i].converter == converter) {
            added = !g_converters[i].used;
            g_converters[i].used = TRUE;
            g_converters[i].converter = converter;
            g_converters[i].src = *src;
            g_converters[i].dst = *dst;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_converter_lock);
    if (added) {
        char s[5], d[5];
        FormatFourCC(src->mFormatID, s);
        FormatFourCC(dst->mFormatID, d);
        LOGI("converter %p: '%s' %.0f Hz flags=0x%lx -> '%s' %.0f Hz flags=0x%lx",
             converter, s, src->mSampleRate, (unsigned long)src->mFormatFlags,
             d, dst->mSampleRate, (unsigned long)dst->mFormatFlags);
    }
}

static void NoteConverterCall(void* converter, int kind, uint32_t bytes)
{
    ConverterInfo info;
    BOOL found = FALSE;
    LONG calls = 0;
    AcquireSRWLockExclusive(&g_converter_lock);
    for (int i = 0; i < AMB_MAX_CONVERTERS; ++i) {
        if (g_converters[i].used && g_converters[i].converter == converter) {
            info = g_converters[i];
            if (kind == 0) calls = InterlockedIncrement(&g_converters[i].convert_calls);
            else if (kind == 1) calls = InterlockedIncrement(&g_converters[i].fill_calls);
            else calls = InterlockedIncrement(&g_converters[i].complex_calls);
            found = TRUE;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_converter_lock);
    if (found && calls <= 3) {
        const char* names[3] = {"ConvertBuffer", "FillBuffer", "FillComplexBuffer"};
        char s[5], d[5];
        FormatFourCC(info.src.mFormatID, s);
        FormatFourCC(info.dst.mFormatID, d);
        LOGI("converter call %s: %p call=%ld bytes=%lu '%s' %.0f -> '%s' %.0f",
             names[kind], converter, (long)calls, (unsigned long)bytes,
             s, info.src.mSampleRate, d, info.dst.mSampleRate);
    }
}

static void RemoveConverterInfo(void* converter)
{
    AcquireSRWLockExclusive(&g_converter_lock);
    for (int i = 0; i < AMB_MAX_CONVERTERS; ++i) {
        if (g_converters[i].used && g_converters[i].converter == converter) {
            g_converters[i].used = FALSE;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_converter_lock);
}

static void SetSourceRate(double rate, const char* source)
{
    if (rate < 8000.0 || rate > 768000.0) return;
    uint32_t value = (uint32_t)(rate + 0.5);
    uint32_t old = (uint32_t)InterlockedCompareExchange(&g_source_rate, 0, 0);
    if (old == 0 || value < old) {
        InterlockedExchange(&g_source_rate, (LONG)value);
        LOGI("source rate: %u Hz (from %s)", value, source);
    }
}

static void ResetSourceRate(void)
{
    InterlockedExchange(&g_source_rate, 0);
}

uint32_t AppHooks_GetSourceRate(void)
{
    LONG qlac = InterlockedCompareExchange(&g_qlac_rate, 0, 0);
    if (qlac) return (uint32_t)qlac;
    return (uint32_t)InterlockedCompareExchange(&g_source_rate, 0, 0);
}

uint32_t AppHooks_GetSourceCodec(void)
{
    return (uint32_t)InterlockedCompareExchange(&g_source_codec, 0, 0);
}

BOOL AppHooks_GetCaptureFormat(WAVEFORMATEX* out, DWORD bytes)
{
    if (!out) return FALSE;
    BOOL found = FALSE;
    AcquireSRWLockShared(&g_capture_lock);
    if (g_have_capture_format) {
        DWORD need = sizeof(WAVEFORMATEX) + g_capture_format.wfx.Format.cbSize;
        if (bytes >= need) {
            memcpy(out, &g_capture_format.wfx, need);
            found = TRUE;
        }
    }
    ReleaseSRWLockShared(&g_capture_lock);
    return found;
}

void AppHooks_ResetCapture(void)
{
    AcquireSRWLockExclusive(&g_capture_lock);
    g_capture_count = 0;
    g_have_capture_format = FALSE;
    g_unit_format_set = FALSE;
    ZeroMemory(g_capture_entries, sizeof(g_capture_entries));
    ZeroMemory(&g_capture_format, sizeof(g_capture_format));
    ReleaseSRWLockExclusive(&g_capture_lock);

    AcquireSRWLockExclusive(&g_unit_tap_lock);
    ZeroMemory(g_unit_taps, sizeof(g_unit_taps));
    g_capture_unit = NULL;
    g_capture_rate_error = 1e9;
    ReleaseSRWLockExclusive(&g_unit_tap_lock);
}

static BOOL IsFloatLpcm(const AudioStreamBasicDescription* fmt)
{
    return fmt && fmt->mFormatID == 0x6c70636d /* lpcm */ &&
           (fmt->mFormatFlags & 1u) != 0 && fmt->mChannelsPerFrame == 2 &&
           fmt->mBitsPerChannel == 32 && fmt->mSampleRate >= 8000.0 &&
           fmt->mSampleRate <= 768000.0 &&
           (fmt->mBytesPerFrame == 8 || (fmt->mFormatFlags & 0x20u) != 0);
}

static void SetCaptureFormat(void* converter, const AudioStreamBasicDescription* src,
                             const AudioStreamBasicDescription* dst)
{
    if (g_tap_new) return;
    if (!converter || !src || !dst) return;
    if (src->mFormatID == 0x6c70636d /* lpcm */) return;
    if (!IsFloatLpcm(dst)) return;

    AmbAudioFormat format;
    Format_Make(&format, (UINT32)(dst->mSampleRate + 0.5), dst->mChannelsPerFrame,
                AMB_SAMPLE_F32, 32);
    BOOL planar = (dst->mFormatFlags & 0x20u) != 0;

    AcquireSRWLockExclusive(&g_capture_lock);
    int slot = -1;
    for (int i = 0; i < g_capture_count; ++i) {
        if (g_capture_entries[i].converter == converter) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && g_capture_count < AMB_MAX_CAPTURE_CONVERTERS) {
        slot = g_capture_count++;
    }
    if (slot >= 0) {
        g_capture_entries[slot].converter = converter;
        g_capture_entries[slot].format = format;
        g_capture_entries[slot].planar = planar;
        if (!g_unit_format_set) {
            g_capture_format = format;
            g_have_capture_format = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&g_capture_lock);

    LOGI("capture format: converter=%p %.0f Hz, 2 ch, float32 (%s)",
         converter, dst->mSampleRate, planar ? "planar" : "interleaved");
#if !AMB_TAP_DIAGNOSTIC
    if (g_source_codec == 0x716c6163u && !g_tap_new) {
        Hooks_SwitchToCapture();
    }
#endif
}

static void RemoveCaptureConverter(void* converter)
{
    AcquireSRWLockExclusive(&g_capture_lock);
    for (int i = 0; i < g_capture_count; ++i) {
        if (g_capture_entries[i].converter == converter) {
            g_capture_entries[i] = g_capture_entries[g_capture_count - 1];
            --g_capture_count;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_capture_lock);
}

static void SetUnitFormat(const AudioStreamBasicDescription* src)
{
    if (!IsFloatLpcm(src)) return;

    AmbAudioFormat format;
    Format_Make(&format, (UINT32)(src->mSampleRate + 0.5), src->mChannelsPerFrame,
                AMB_SAMPLE_F32, 32);

    AcquireSRWLockExclusive(&g_capture_lock);
    g_capture_format = format;
    g_have_capture_format = TRUE;
    g_unit_format_set = TRUE;
    ReleaseSRWLockExclusive(&g_capture_lock);

    AcquireSRWLockExclusive(&g_unit_tap_lock);
    for (size_t i = 0; i < sizeof(g_unit_taps) / sizeof(g_unit_taps[0]); ++i) {
        if (g_unit_taps[i].used) {
            g_unit_taps[i].format = format;
            g_unit_taps[i].have_format = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&g_unit_tap_lock);

    char text[128];
    Format_ToString(&format, text, sizeof(text));
    LOGI("AudioUnit input format: %s", text);
}

static UnitTap* BindUnitTap(void* unit, PFN_AURenderCallback original, void* original_ref)
{
    if (!unit || !original) return NULL;

    AmbAudioFormat format;
    BOOL have_format = FALSE;
    AcquireSRWLockShared(&g_capture_lock);
    if (g_have_capture_format) {
        format = g_capture_format;
        have_format = TRUE;
    }
    ReleaseSRWLockShared(&g_capture_lock);

    UnitTap* result = NULL;
    AcquireSRWLockExclusive(&g_unit_tap_lock);
    for (size_t i = 0; i < sizeof(g_unit_taps) / sizeof(g_unit_taps[0]); ++i) {
        if (g_unit_taps[i].used && g_unit_taps[i].unit == unit) {
            result = &g_unit_taps[i];
            break;
        }
    }
    if (!result) {
        for (size_t i = 0; i < sizeof(g_unit_taps) / sizeof(g_unit_taps[0]); ++i) {
            if (!g_unit_taps[i].used) {
                result = &g_unit_taps[i];
                result->used = TRUE;
                break;
            }
        }
    }
    if (result) {
        result->unit = unit;
        result->original = original;
        result->original_ref = original_ref;
        result->format = format;
        result->have_format = have_format;
        result->calls = 0;
        result->frames = 0;
        result->first_call.QuadPart = 0;

    }
    ReleaseSRWLockExclusive(&g_unit_tap_lock);
    return result;
}

static void UpdateCaptureUnit(UnitTap* tap, LARGE_INTEGER now)
{
    if (!tap || !tap->have_format || !tap->format.is_float ||
        tap->format.channels != 2 || tap->calls < 20 ||
        g_capture_unit != NULL) {
        return;
    }

    uint32_t source_rate = AppHooks_GetSourceRate();
    if (!source_rate || tap->first_call.QuadPart == 0) return;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double seconds = (double)(now.QuadPart - tap->first_call.QuadPart) /
                     (double)freq.QuadPart;
    if (seconds < 0.15) return;

    LONG64 total = InterlockedCompareExchange64(&tap->frames, 0, 0);
    double rate = (double)total / seconds;
    double error = fabs(rate - (double)source_rate);
    if (error > (double)source_rate * 0.15) return;

    AcquireSRWLockExclusive(&g_unit_tap_lock);
    if (!g_capture_unit && error < g_capture_rate_error) {
        g_capture_rate_error = error;
        g_capture_unit = tap->unit;
        LOGI("capture unit selected by rate: %p measured=%.0f target=%lu error=%.0f",
             tap->unit, rate, (unsigned long)source_rate, error);
    }
    ReleaseSRWLockExclusive(&g_unit_tap_lock);
}

static BOOL DecoderActive(void)
{
    if (InterlockedCompareExchange(&g_decoder_capture_active, 0, 0) == 0) return FALSE;
    LONG64 last = InterlockedCompareExchange64(&g_decoder_last_write, 0, 0);
    if (last == 0 || GetTickCount64() - (ULONGLONG)last > 500) {
        InterlockedExchange(&g_decoder_capture_active, 0);
        return FALSE;
    }
    return TRUE;
}

static OSStatus CapturedRenderCallback(void* ref, uint32_t* flags, const void* timestamp,
                                       uint32_t bus, uint32_t frames, void* io_data)
{
    UnitTap* tap = (UnitTap*)ref;
    if (!tap || !tap->original) return -1;

    OSStatus result = tap->original(tap->original_ref, flags, timestamp, bus, frames, io_data);
    LONG call = InterlockedIncrement(&tap->calls);
    InterlockedExchangeAdd64(&tap->frames, (LONG64)frames);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (tap->first_call.QuadPart == 0) tap->first_call = now;
    if (call <= 8) {
        LOGI("AudioUnit render: unit=%p call=%ld result=%ld frames=%lu rate=%lu ch=%lu qpc=%lld",
             tap->unit, (long)call, (long)result, (unsigned long)frames,
             (unsigned long)(tap->have_format ? tap->format.rate : 0),
             (unsigned long)(tap->have_format ? tap->format.channels : 0),
             (long long)now.QuadPart);
    }
    if ((call % 200) == 0 && tap->first_call.QuadPart != 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        LONG64 total = InterlockedCompareExchange64(&tap->frames, 0, 0);
        double seconds = (double)(now.QuadPart - tap->first_call.QuadPart) /
                         (double)freq.QuadPart;
        LOGI("AudioUnit rate: unit=%p calls=%ld frames=%lld seconds=%.3f rate=%.0f format=%.0f",
             tap->unit, (long)call, (long long)total, seconds,
             seconds > 0.0 ? (double)total / seconds : 0.0,
             (double)(tap->have_format ? tap->format.rate : 0));
    }
    UpdateCaptureUnit(tap, now);
    if (!g_tap_new && result == 0 && io_data && frames && tap->have_format && tap->format.is_float &&
        tap->format.channels == 2 && tap->unit == g_capture_unit &&
        InterlockedCompareExchange(&g_tap_active, 0, 0) == 0 && !DecoderActive()) {
        Hooks_CaptureAudioUnit(io_data, frames);
    }
    return result;
}

static OSStatus Hook_AudioUnitSetProperty(void* unit, uint32_t property, uint32_t scope,
                                           uint32_t element, const void* data, uint32_t size)
{
    if (unit && data && scope == 1) {
        if (property == 8 && size >= sizeof(AudioStreamBasicDescription)) {
            SetUnitFormat((const AudioStreamBasicDescription*)data);
        } else if (property == 23 && size >= sizeof(AURenderCallbackStruct)) {
            const AURenderCallbackStruct* callback = (const AURenderCallbackStruct*)data;
            if (callback->input_proc && callback->input_proc != CapturedRenderCallback) {
                UnitTap* tap = BindUnitTap(unit, callback->input_proc, callback->input_ref);
                if (tap) {
                    AURenderCallbackStruct wrapped = *callback;
                    wrapped.input_proc = CapturedRenderCallback;
                    wrapped.input_ref = tap;
                    LOGI("AudioUnit render callback hooked: unit=%p element=%lu",
                         unit, (unsigned long)element);
                    return g_unit_set_property
                               ? g_unit_set_property(unit, property, scope, element, &wrapped, size)
                               : -1;
                }
            }
        }
    }
    return g_unit_set_property
               ? g_unit_set_property(unit, property, scope, element, data, size)
               : -1;
}

static void CaptureOutput(void* converter, const AudioBufferList* list, uint32_t packets)
{
    if (!converter || !list || packets == 0) return;

    CaptureEntry entry;
    BOOL found = FALSE;
    AcquireSRWLockShared(&g_capture_lock);
    for (int i = 0; i < g_capture_count; ++i) {
        if (g_capture_entries[i].converter == converter) {
            entry = g_capture_entries[i];
            found = TRUE;
            break;
        }
    }
    ReleaseSRWLockShared(&g_capture_lock);
    if (!found || !list->buffers[0].data) return;

    uint32_t frames = 0;
    const float* source = NULL;
    if (!entry.planar) {
        if (list->number_buffers < 1) return;
        uint32_t bytes = list->buffers[0].data_byte_size;
        if (bytes < entry.format.frame_bytes || bytes % entry.format.frame_bytes) return;
        frames = bytes / entry.format.frame_bytes;
        source = (const float*)list->buffers[0].data;
        if (frames > packets) frames = packets;
        Hooks_WriteCapture(source, frames);
        return;
    }

    if (list->number_buffers < 2 || !list->buffers[1].data) return;
    uint32_t bytes0 = list->buffers[0].data_byte_size;
    uint32_t bytes1 = list->buffers[1].data_byte_size;
    if (bytes0 < sizeof(float) || bytes0 % sizeof(float) || bytes0 != bytes1) return;
    frames = bytes0 / sizeof(float);
    if (frames > packets) frames = packets;
    if (frames > AMB_CAPTURE_SCRATCH_FRAMES) {
        LOGW("capture: planar block too large (%lu frames)", (unsigned long)frames);
        return;
    }
    const float* left = (const float*)list->buffers[0].data;
    const float* right = (const float*)list->buffers[1].data;
    for (uint32_t i = 0; i < frames; ++i) {
        g_capture_scratch[i * 2] = left[i];
        g_capture_scratch[i * 2 + 1] = right[i];
    }
    Hooks_WriteCapture(g_capture_scratch, frames);
}

static void CapturePcmBuffer(const AudioBufferList* list, uint32_t frames, BOOL planar)
{
    if (!list || frames == 0 || !list->buffers[0].data) return;

    if (planar) {
        if (list->number_buffers < 2 || !list->buffers[1].data) return;
        if (frames > AMB_CAPTURE_SCRATCH_FRAMES) {
            LOGW("capture: planar block too large (%lu frames)", (unsigned long)frames);
            return;
        }
        const float* left = (const float*)list->buffers[0].data;
        const float* right = (const float*)list->buffers[1].data;
        for (uint32_t i = 0; i < frames; ++i) {
            g_capture_scratch[i * 2] = left[i];
            g_capture_scratch[i * 2 + 1] = right[i];
        }
        Hooks_WriteCapture(g_capture_scratch, frames);
        return;
    }

    uint32_t available = list->buffers[0].data_byte_size / sizeof(float);
    if (frames > available) frames = available;
    Hooks_WriteCapture(list->buffers[0].data, frames);
}

typedef struct TapBuffer {
    uint32_t channels;
    uint32_t bytes;
    void* data;
} TapBuffer;

typedef struct TapTime {
    double sample_time;
    uint64_t host_time;
} TapTime;

typedef struct TapCtx {
    AudioStreamBasicDescription fmt;
    void* queue;
    volatile LONG calls;
    volatile LONG started;
    volatile LONG64 last_tick;
} TapCtx;

static TapCtx* volatile g_tap_ctxs[16];
static volatile LONG g_tap_ctx_count;
static volatile LONG g_tap_total_calls;

static OSStatus AudioQueueTapCallback(void* client, void* tap, uint32_t frames, void* timestamp,
                                      uint32_t* flags, uint32_t* out_frames, void* data)
{
    TapCtx* ctx = (TapCtx*)client;
    LONG total = InterlockedIncrement(&g_tap_total_calls);
    LONG call = ctx ? InterlockedIncrement(&ctx->calls) : 0;
    uint32_t got = 0;
    OSStatus result = 0;
    if (g_tap_get_source && tap && data) {
        result = g_tap_get_source(tap, frames, timestamp, flags, &got, data);
    }
    if (out_frames) *out_frames = got;
    BOOL active = FALSE;
    if (ctx && result == 0 && got && data) {
        LONG64 now = (LONG64)GetTickCount64();
        InterlockedExchange64(&ctx->last_tick, now);
        void* current = InterlockedCompareExchangePointer((PVOID volatile*)&g_active_tap, NULL, NULL);
        if (!current) {
            current = InterlockedCompareExchangePointer((PVOID volatile*)&g_active_tap, ctx, NULL);
            if (!current) current = ctx;
        }
        if (current == (void*)ctx) {
            active = TRUE;
        } else {
            TapCtx* other = (TapCtx*)current;
            LONG64 last = InterlockedCompareExchange64(&other->last_tick, 0, 0);
            if (last == 0 || now - last > 50) {
                InterlockedExchangePointer((PVOID volatile*)&g_active_tap, ctx);
                active = TRUE;
            }
        }
    }
    if (call <= 20) {
        uint32_t count = data ? *(const uint32_t*)data : 0;
        const TapBuffer* buffers = data ? (const TapBuffer*)((const unsigned char*)data + 8) : NULL;
        const TapBuffer* first = (count && buffers) ? &buffers[0] : NULL;
        uint32_t bytes0 = first ? first->bytes : 0;
        const void* ptr0 = first ? first->data : NULL;
        float sample0 = (ptr0 && bytes0 >= sizeof(float)) ? *(const float*)ptr0 : 0.0f;
        LOGI("AQ tap data: total=%ld calls=%ld queue=%p frames=%lu got=%lu result=%ld active=%d count=%lu bytes0=%lu ptr0=%p sample0=%.6f flags=0x%lx ctx=%p tid=%lu",
             (long)total, (long)call, ctx ? ctx->queue : NULL,
             (unsigned long)frames, (unsigned long)got, (long)result, active ? 1 : 0,
             (unsigned long)count, (unsigned long)bytes0, ptr0, (double)sample0,
             (unsigned long)(flags ? *flags : 0), (void*)ctx,
             (unsigned long)GetCurrentThreadId());
        if (timestamp) {
            const TapTime* time = (const TapTime*)timestamp;
            LOGI("AQ tap time: calls=%ld sample=%.3f host=%llu now=%lld started=%ld",
                 (long)call, time->sample_time,
                 (unsigned long long)time->host_time,
                 (long long)GetTickCount64(),
                 ctx ? (long)InterlockedCompareExchange(&ctx->started, 0, 0) : -1);
        }
        if (g_tap_get_queue_time) {
            double queue_sample = 0.0;
            uint32_t queue_flags = 0;
            OSStatus queue_status = g_tap_get_queue_time(tap, &queue_sample, &queue_flags);
            LOGI("AQ tap queue: queue=%p tap=%p sample=%.3f flags=0x%lx status=%ld",
                 ctx ? ctx->queue : NULL, tap,
                 queue_sample, (unsigned long)queue_flags,
                 (long)queue_status);
        }
    }
    if (call > 20 && (call % 50) == 0) {
        double queue_sample = 0.0;
        uint32_t queue_flags = 0;
        OSStatus queue_status = g_tap_get_queue_time
                                    ? g_tap_get_queue_time(tap, &queue_sample, &queue_flags)
                                    : -1;
        LOGI("AQ tap progress: total=%ld calls=%ld queue=%p frames=%lu got=%lu result=%ld active=%d tid=%lu queue_sample=%.3f queue_flags=0x%lx queue_status=%ld",
             (long)total, (long)call, ctx ? ctx->queue : NULL,
             (unsigned long)frames, (unsigned long)got, (long)result, active ? 1 : 0,
             (unsigned long)GetCurrentThreadId(), queue_sample,
             (unsigned long)queue_flags, (long)queue_status);
    }
#if !AMB_TAP_DIAGNOSTIC
    if (active && data) {
        Hooks_CaptureAudioUnit(data, got);
    }
#endif
    return result;
}

static void MaybeCreateProcessingTap(void* queue, const AudioStreamBasicDescription* fmt)
{
    static volatile LONG attempts = 0;
    if (!g_tap_new || !queue || !fmt) return;

    LONG attempt = InterlockedIncrement(&attempts);
    TapCtx* ctx = (TapCtx*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TapCtx));
    if (!ctx) return;
    ctx->fmt = *fmt;
    ctx->queue = queue;
    InterlockedExchangePointer((PVOID volatile*)&g_active_tap, NULL);

    uint32_t max_frames = 0;
    AudioStreamBasicDescription processing;
    ZeroMemory(&processing, sizeof(processing));
    void* tap = NULL;
    OSStatus status = g_tap_new(queue, AudioQueueTapCallback, ctx, 2, &max_frames,
                                &processing, &tap);
    if (status == 0) {
        AmbAudioFormat capture_fmt;
        Format_Make(&capture_fmt, (uint32_t)(processing.mSampleRate + 0.5),
                    processing.mChannelsPerFrame, AMB_SAMPLE_F32, 32);
        AcquireSRWLockExclusive(&g_capture_lock);
        g_capture_format = capture_fmt;
        g_have_capture_format = TRUE;
        g_unit_format_set = TRUE;
        ReleaseSRWLockExclusive(&g_capture_lock);
        LONG slot = InterlockedIncrement(&g_tap_ctx_count) - 1;
        if (slot >= 0 && slot < (LONG)(sizeof(g_tap_ctxs) / sizeof(g_tap_ctxs[0]))) {
            InterlockedExchangePointer((PVOID volatile*)&g_tap_ctxs[slot], ctx);
        }
        InterlockedExchange(&g_tap_active, 1);
        InterlockedExchangePointer((PVOID volatile*)&g_capture_decoder, NULL);
        InterlockedExchange(&g_decoder_capture_active, 0);
        LOGI("AQ processing tap created: attempt=%ld queue=%p rate=%.0f ch=%lu max=%lu tap=%p",
             (long)attempt, queue, processing.mSampleRate,
             (unsigned long)processing.mChannelsPerFrame, (unsigned long)max_frames, tap);
    } else {
        if (attempt <= 5) {
            LOGW("AQ processing tap failed: attempt=%ld queue=%p status=%ld",
                 (long)attempt, queue, (long)status);
        }
        HeapFree(GetProcessHeap(), 0, ctx);
    }
}

static OSStatus Hook_AudioQueueStop(void* queue, uint32_t immediate)
{
    TapCtx* tap = (TapCtx*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_active_tap, NULL, NULL);
    void* tap_queue = tap ? tap->queue : NULL;
    DWORD tid = GetCurrentThreadId();
    LOGI("AudioQueueStop: enter queue=%p immediate=%lu active_tap_queue=%p tid=%lu",
         queue, (unsigned long)immediate, tap_queue, (unsigned long)tid);
    if (tap && tap->queue == queue) {
        InterlockedExchangePointer((PVOID volatile*)&g_active_tap, NULL);
        InterlockedExchange(&g_tap_active, 0);
        InterlockedExchangePointer((PVOID volatile*)&g_normal_queue, NULL);
        Hooks_StopCapture();
    } else if (InterlockedCompareExchangePointer(
                   (PVOID volatile*)&g_normal_queue, NULL, NULL) == queue) {
        InterlockedExchangePointer((PVOID volatile*)&g_normal_queue, NULL);
        Hooks_StopNormalStream();
    }
    OSStatus result = g_queue_stop ? g_queue_stop(queue, immediate) : -1;
    LOGI("AudioQueueStop: exit queue=%p result=%ld immediate=%lu active_tap_queue=%p tid=%lu",
         queue, (long)result, (unsigned long)immediate, tap_queue, (unsigned long)tid);
    return result;
}

static DWORD WINAPI CaptureSwitchThread(void* parameter)
{
    AMB_UNUSED(parameter);
    Hooks_SwitchToCapture();
    InterlockedExchange(&g_switch_pending, 0);
    return 0;
}

static OSStatus Hook_AudioQueueStart(void* queue, const void* start_time)
{
    TapCtx* tap_ctx = NULL;
    LONG count = InterlockedCompareExchange(&g_tap_ctx_count, 0, 0);
    for (LONG i = 0; i < count && i < (LONG)(sizeof(g_tap_ctxs) / sizeof(g_tap_ctxs[0])); ++i) {
        TapCtx* ctx = (TapCtx*)InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_tap_ctxs[i], NULL, NULL);
        if (ctx && ctx->queue == queue) {
            tap_ctx = ctx;
            break;
        }
    }

    /* Put the capture endpoint in place before Apple starts the queue clock.
       During a seek this keeps the old position silent until audio is ready. */
    BOOL prepared = tap_ctx ? Hooks_SwitchToCapture() : FALSE;
    OSStatus result = g_queue_start ? g_queue_start(queue, start_time) : -1;
    if (result == 0 && !tap_ctx) {
        InterlockedExchangePointer((PVOID volatile*)&g_normal_queue, queue);
    }
    if (result == 0 && tap_ctx) {
        InterlockedExchangePointer((PVOID volatile*)&g_normal_queue, NULL);
        InterlockedExchange(&tap_ctx->started, 1);
        LOGI("AudioQueueStart: qlac tap queue=%p prepared=%d", queue, prepared ? 1 : 0);
        if (!prepared && InterlockedCompareExchange(&g_switch_pending, 1, 0) == 0) {
            HANDLE thread = CreateThread(NULL, 0, CaptureSwitchThread, NULL, 0, NULL);
            if (thread) {
                CloseHandle(thread);
            } else {
                InterlockedExchange(&g_switch_pending, 0);
            }
        }
    }
    LOGI("AudioQueueStart: queue=%p result=%ld", queue, (long)result);
    return result;
}

static OSStatus Hook_AudioQueueNewOutput(const AudioStreamBasicDescription* fmt,
                                         void* callback, void* user, void* loop,
                                         void* mode, uint32_t flags, void** out)
{
    g_capture_unit = NULL;
    InterlockedExchangePointer((PVOID volatile*)&g_capture_decoder, NULL);
    InterlockedExchange(&g_decoder_capture_active, 0);
    if (fmt && fmt->mFormatID == 0x716c6163u) {
        InterlockedExchange(&g_tap_active, 1);
    }
    g_capture_rate_error = 1e9;
    if (fmt) {
        char fourcc[5];
        FormatFourCC(fmt->mFormatID, fourcc);
        InterlockedExchange(&g_source_codec, (LONG)fmt->mFormatID);
        LOGI("AudioQueueNewOutput: %.0f Hz fmt='%s' flags=0x%08lx %lu ch",
             fmt->mSampleRate, fourcc, (unsigned long)fmt->mFormatFlags,
             (unsigned long)fmt->mChannelsPerFrame);
        ResetSourceRate();
        SetSourceRate(fmt->mSampleRate, "AudioQueueNewOutput");
        if (fmt->mFormatID == 0x716c6163u) {
            InterlockedExchange(&g_qlac_rate, (LONG)(fmt->mSampleRate + 0.5));
        }
    }
    OSStatus result = g_queue_new ? g_queue_new(fmt, callback, user, loop, mode, flags, out) : -1;
    if (result == 0 && out && *out && fmt) {
        AcquireSRWLockExclusive(&g_queue_lock);
        int slot = -1;
        for (int i = 0; i < AMB_MAX_QUEUES; ++i) {
            if (!g_queues[i].used || g_queues[i].queue == *out) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            g_queues[slot].used = TRUE;
            g_queues[slot].queue = *out;
            g_queues[slot].format = *fmt;
        }
        ReleaseSRWLockExclusive(&g_queue_lock);
        LOGI("AudioQueueNewOutput: queue=%p", *out);
        if (fmt->mFormatID == 0x716c6163u) {
            MaybeCreateProcessingTap(*out, fmt);
        }
    }
    return result;
}

static OSStatus Hook_AudioQueueEnqueueBufferWithParameters(
    void* queue, AudioQueueBuffer* buffer, uint32_t packet_desc_count,
    const void* packet_descs, uint32_t trim_start, uint32_t trim_end,
    uint32_t parameter_count, const void* parameters, const void* start_time,
    void* actual_start_time)
{
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (queue && buffer) {
        DWORD tid = GetCurrentThreadId();
        if (call <= 12) {
            AudioStreamBasicDescription fmt;
            BOOL known = FALSE;
            AcquireSRWLockShared(&g_queue_lock);
            for (int i = 0; i < AMB_MAX_QUEUES; ++i) {
                if (g_queues[i].used && g_queues[i].queue == queue) {
                    fmt = g_queues[i].format;
                    known = TRUE;
                    break;
                }
            }
            ReleaseSRWLockShared(&g_queue_lock);

            char fourcc[5] = {0};
            if (known) FormatFourCC(fmt.mFormatID, fourcc);
            LOGI("AudioQueueEnqueue: call=%ld tid=%lu queue=%p known=%d buf=%p bytes=%lu cap=%lu packets=%lu "
                 "desc=%lu fmt='%s' %.0f Hz %lu ch %lu bits flags=0x%lx",
                 (long)call, (unsigned long)tid, queue, known ? 1 : 0, (void*)buffer,
                 (unsigned long)buffer->mAudioDataByteSize,
                 (unsigned long)buffer->mAudioDataBytesCapacity,
                 (unsigned long)buffer->mPacketDescriptionCount,
                 (unsigned long)packet_desc_count, fourcc,
                 known ? fmt.mSampleRate : 0.0,
                 known ? (unsigned long)fmt.mChannelsPerFrame : 0,
                 known ? (unsigned long)fmt.mBitsPerChannel : 0,
                 known ? (unsigned long)fmt.mFormatFlags : 0);
        } else {
            LOGI("AudioQueueEnqueue: call=%ld tid=%lu queue=%p buf=%p bytes=%lu cap=%lu",
                 (long)call, (unsigned long)tid, queue, (void*)buffer,
                 (unsigned long)buffer->mAudioDataByteSize,
                 (unsigned long)buffer->mAudioDataBytesCapacity);
        }
    }
    return g_queue_enqueue
               ? g_queue_enqueue(queue, buffer, packet_desc_count, packet_descs,
                                 trim_start, trim_end, parameter_count, parameters,
                                 start_time, actual_start_time)
               : -1;
}

static OSStatus Hook_AudioConverterNew(const AudioStreamBasicDescription* src,
                                       const AudioStreamBasicDescription* dst, void** out)
{
    if (src && dst) {
        LOGI("AudioConverterNew: %.0f -> %.0f Hz (fmt flags 0x%lx -> 0x%lx)",
             src->mSampleRate, dst->mSampleRate, (unsigned long)src->mFormatFlags,
             (unsigned long)dst->mFormatFlags);
        SetSourceRate(src->mSampleRate, "AudioConverterNew source");
    }
    OSStatus result = g_converter_new ? g_converter_new(src, dst, out) : -1;
    if (result == 0 && out && *out) {
        LOGI("AudioConverterNew: converter=%p", *out);
        SetCaptureFormat(*out, src, dst);
        TrackConverter(*out, src, dst);
    }
    return result;
}

static OSStatus Hook_AudioConverterNewSpecific(const AudioStreamBasicDescription* src,
                                               const AudioStreamBasicDescription* dst,
                                               uint32_t manufacturer, const void* class_id,
                                               void** out)
{
    if (src && dst) {
        LOGI("AudioConverterNewSpecific: %.0f -> %.0f Hz", src->mSampleRate,
             dst->mSampleRate);
        SetSourceRate(src->mSampleRate, "AudioConverterNewSpecific source");
    }
    OSStatus result = g_converter_specific
                          ? g_converter_specific(src, dst, manufacturer, class_id, out)
                          : -1;
    if (result == 0 && out && *out) {
        LOGI("AudioConverterNewSpecific: converter=%p", *out);
        SetCaptureFormat(*out, src, dst);
        TrackConverter(*out, src, dst);
    }
    return result;
}

static OSStatus Hook_AudioConverterConvertBuffer(void* converter, uint32_t in_size,
                                                 const void* in_data, uint32_t* io_size,
                                                 void* out_data)
{
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 5) {
        LOGI("AudioConverterConvertBuffer: converter=%p in=%lu out=%lu inptr=%p outptr=%p",
             converter, (unsigned long)in_size,
             io_size ? (unsigned long)*io_size : 0, in_data, out_data);
    }
    OSStatus result = g_convert_buffer
                           ? g_convert_buffer(converter, in_size, in_data, io_size, out_data)
                           : -1;
    NoteConverterCall(converter, 0, io_size ? *io_size : in_size);
    return result;
}

static OSStatus Hook_AudioConverterFillBuffer(void* converter, void* input_proc, void* user,
                                              uint32_t* io_size, void* out_data)
{
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 5) {
        LOGI("AudioConverterFillBuffer: converter=%p input=%p user=%p size=%lu out=%p",
             converter, input_proc, user, io_size ? (unsigned long)*io_size : 0, out_data);
    }
    OSStatus result = g_fill_buffer
                          ? g_fill_buffer(converter, input_proc, user, io_size, out_data)
                          : -1;
    NoteConverterCall(converter, 1, io_size ? *io_size : 0);
    return result;
}

typedef struct SourceCallback {
    PFN_AudioConverterComplexInputDataProc original;
    void* original_user;
    void* converter;
} SourceCallback;

static OSStatus SourceInputCallback(void* converter, uint32_t* packets, void* data, void* desc,
                                    void* user)
{
    SourceCallback* source = (SourceCallback*)user;
    if (!source || !source->original) return -1;
    OSStatus result = source->original(converter, packets, data, desc, source->original_user);

    static void* seen[32];
    static volatile LONG counts[32];
    static volatile LONG slots = 0;
    int slot = -1;
    for (int i = 0; i < 32; ++i) {
        if (seen[i] == source->converter) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        LONG next = InterlockedIncrement(&slots) - 1;
        if (next < 32) {
            seen[next] = source->converter;
            slot = (int)next;
        }
    }
    if (slot >= 0) {
        LONG call = InterlockedIncrement(&counts[slot]);
        if (call <= 8) {
            AudioBufferList* list = (AudioBufferList*)data;
            uint32_t bytes = 0;
            if (list && list->number_buffers > 0) bytes = list->buffers[0].data_byte_size;
            LOGI("AudioConverter input: converter=%p call=%ld result=%ld packets=%lu buffers=%lu bytes=%lu",
                 source->converter, (long)call, (long)result,
                 packets ? (unsigned long)*packets : 0,
                 list ? (unsigned long)list->number_buffers : 0,
                 (unsigned long)bytes);
        }
    }
    return result;
}

static OSStatus Hook_AudioConverterFillComplexBuffer(void* converter, void* input_proc,
                                                     void* user, uint32_t* packets,
                                                     void* output, void* packet_desc)
{
    SourceCallback source = {(PFN_AudioConverterComplexInputDataProc)input_proc, user, converter};
    void* use_proc = input_proc ? (void*)SourceInputCallback : NULL;
    void* use_user = input_proc ? &source : user;
    OSStatus result = g_fill_complex
                          ? g_fill_complex(converter, use_proc, use_user, packets, output,
                                           packet_desc)
                          : -1;
    NoteConverterCall(converter, 2, packets ? *packets : 0);
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 5) {
        LOGI("AudioConverterFillComplexBuffer: converter=%p result=%ld packets=%lu output=%p",
             converter, (long)result, packets ? (unsigned long)*packets : 0, output);
    }
    static volatile LONG format_logs = 0;
    AudioStreamBasicDescription dst = {0};
    BOOL have_dst = FALSE;
    if (g_converter_get_property) {
        uint32_t size = sizeof(dst);
        OSStatus hr = g_converter_get_property(converter, 0x61636f64u, &size, &dst);
        have_dst = hr == 0;
        if (have_dst && format_logs < 20) {
            LONG n = InterlockedIncrement(&format_logs);
            if (n <= 20) {
                char fourcc[5];
                FormatFourCC(dst.mFormatID, fourcc);
                LOGI("converter output format: %p '%s' %.0f Hz flags=0x%lx ch=%lu bits=%lu",
                     converter, fourcc, dst.mSampleRate, (unsigned long)dst.mFormatFlags,
                     (unsigned long)dst.mChannelsPerFrame, (unsigned long)dst.mBitsPerChannel);
            }
        }
    }
    uint32_t out_rate = have_dst ? (uint32_t)(dst.mSampleRate + 0.5) : 0;
    if (!g_tap_new && have_dst && result >= 0 && output && packets && *packets &&
        InterlockedCompareExchange(&g_tap_active, 0, 0) == 0 &&
        dst.mFormatID == 0x6c70636d && (dst.mFormatFlags & 1u) != 0 &&
        dst.mChannelsPerFrame == 2 && dst.mBitsPerChannel == 32 &&
        out_rate != 0 && out_rate == g_source_rate) {
        void* selected = InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_capture_decoder, converter, NULL);
        if (selected != converter) {
            selected = InterlockedExchangePointer((PVOID volatile*)&g_capture_decoder, converter);
            LOGI("capture decoder %s: %p 'lpcm' %lu Hz flags=0x%lx",
                 selected ? "switched" : "selected", converter,
                 (unsigned long)out_rate, (unsigned long)dst.mFormatFlags);
        }
        if (g_capture_decoder == converter) {
            InterlockedExchange(&g_decoder_capture_active, 1);
            InterlockedExchange64(&g_decoder_last_write, (LONG64)GetTickCount64());
            CapturePcmBuffer((const AudioBufferList*)output, *packets,
                             (dst.mFormatFlags & 0x20u) != 0);
        }
    }
    if (!g_tap_new && output && packets && *packets && !DecoderActive() &&
        InterlockedCompareExchange(&g_tap_active, 0, 0) == 0) {
        CaptureOutput(converter, (const AudioBufferList*)output, *packets);
    }
    return result;
}

static OSStatus Hook_AudioConverterDispose(void* converter)
{
    RemoveCaptureConverter(converter);
    RemoveConverterInfo(converter);
    return g_converter_dispose ? g_converter_dispose(converter) : -1;
}

static OSStatus Hook_AudioUnitRender(void* unit, uint32_t* flags, const void* timestamp,
                                     uint32_t bus, uint32_t frames, void* io_data)
{
    OSStatus result = g_unit_render
                          ? g_unit_render(unit, flags, timestamp, bus, frames, io_data)
                          : -1;
    static volatile LONG calls = 0;
    LONG call = InterlockedIncrement(&calls);
    if (call <= 8) {
        LOGI("AudioUnitRender: unit=%p bus=%lu frames=%lu output=%p result=%ld",
             unit, (unsigned long)bus, (unsigned long)frames, io_data, (long)result);
    }
    return result;
}

typedef struct {
    const char* name;
    void* hook;
    void** original;
} HookSpec;

static const HookSpec g_hooks[] = {
    {"AudioQueueNewOutput", (void*)Hook_AudioQueueNewOutput, (void**)&g_queue_new},
    {"AudioQueueStart", (void*)Hook_AudioQueueStart, (void**)&g_queue_start},
    {"AudioQueueStop", (void*)Hook_AudioQueueStop, (void**)&g_queue_stop},
    {"AudioConverterConvertBuffer", (void*)Hook_AudioConverterConvertBuffer,
     (void**)&g_convert_buffer},
    {"AudioConverterFillBuffer", (void*)Hook_AudioConverterFillBuffer,
     (void**)&g_fill_buffer},
    {"AudioConverterFillComplexBuffer", (void*)Hook_AudioConverterFillComplexBuffer,
     (void**)&g_fill_complex},
};

static BOOL ModuleSeen(uintptr_t base)
{
    LONG count = g_seen_count;
    for (LONG i = 0; i < count; ++i) {
        if (g_seen_modules[i] == base) return TRUE;
    }
    return FALSE;
}

static void MarkModule(uintptr_t base)
{
    LONG index = InterlockedIncrement(&g_seen_count) - 1;
    if (index >= 0 && index < (LONG)(sizeof(g_seen_modules) / sizeof(g_seen_modules[0]))) {
        g_seen_modules[index] = base;
    }
}

static BOOL Readable(const void* address, size_t size)
{
    uintptr_t current = (uintptr_t)address;
    uintptr_t end = current + size;
    if (end < current) return FALSE;

    while (current < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((LPCVOID)current, &info, sizeof(info))) return FALSE;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            return FALSE;
        }
        uintptr_t region_end = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (region_end <= current) return FALSE;
        current = region_end;
    }
    return TRUE;
}

static int PatchModule(HMODULE module, const wchar_t* module_name)
{
    BYTE* base = (BYTE*)module;
    if (!base || !Readable(base, sizeof(IMAGE_DOS_HEADER))) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return 0;
    if (!Readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !Readable(base + dir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        return 0;
    }

    int patched = 0;
    DWORD count = dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress);
    for (DWORD d = 0; d < count; ++d, ++imp) {
        if (!Readable(imp, sizeof(*imp)) || !imp->Name ||
            !Readable(base + imp->Name, 1)) {
            break;
        }
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, "CoreAudioToolbox.dll") != 0) continue;
        PIMAGE_THUNK_DATA oft = imp->OriginalFirstThunk
                                    ? (PIMAGE_THUNK_DATA)(base + imp->OriginalFirstThunk)
                                    : (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        PIMAGE_THUNK_DATA ft = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        for (;; ++oft, ++ft) {
            if (!Readable(oft, sizeof(*oft)) || !Readable(ft, sizeof(*ft)) ||
                !oft->u1.AddressOfData) {
                break;
            }
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (!Readable(base + oft->u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) {
                break;
            }
            PIMAGE_IMPORT_BY_NAME name =
                (PIMAGE_IMPORT_BY_NAME)(base + oft->u1.AddressOfData);
            for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
                if (strcmp((const char*)name->Name, g_hooks[i].name) != 0) continue;
                if (!*g_hooks[i].original) break;
                if ((void*)ft->u1.Function == g_hooks[i].hook) break;
                DWORD old_protect = 0;
                if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE,
                                    &old_protect)) {
                    break;
                }
                ft->u1.Function = (ULONG_PTR)g_hooks[i].hook;
                DWORD ignored = 0;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old_protect, &ignored);
                LOGI("coreaudio hook: %ls!%s", module_name, g_hooks[i].name);
                ++patched;
                break;
            }
        }
    }
    return patched;
}

static int PatchModuleSafe(HMODULE module, const wchar_t* module_name)
{
    __try {
        return PatchModule(module, module_name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void PatchAllModules(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snap, &entry)) {
        do {
            if ((HMODULE)entry.modBaseAddr == g_module) continue;
            if (ModuleSeen((uintptr_t)entry.modBaseAddr)) continue;
            MarkModule((uintptr_t)entry.modBaseAddr);
            PatchModuleSafe((HMODULE)entry.modBaseAddr, entry.szModule);
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

static const char* InstallInlineHook(HMODULE module, const char* name, void* detour,
                                     void** original)
{
    FARPROC target = GetProcAddress(module, name);
    if (!target) return "missing";
    MH_STATUS status = MH_CreateHook((LPVOID)target, detour, original);
    if (status != MH_OK) return MH_StatusToString(status);
    LOGI("coreaudio inline hook: %s", name);
    return NULL;
}

static DWORD WINAPI AppHooksThread(void* param)
{
    AMB_UNUSED(param);
    for (int i = 0; i < 200; ++i) {
        if (GetModuleHandleW(L"CoreAudioToolbox.dll")) break;
        Sleep(50);
    }

    HMODULE coreaudio = GetModuleHandleW(L"CoreAudioToolbox.dll");
    if (!coreaudio) {
        LOGW("coreaudio hook: CoreAudioToolbox not loaded");
        return 0;
    }
    g_tap_new = (PFN_AudioQueueProcessingTapNew)GetProcAddress(coreaudio,
                                                               "AudioQueueProcessingTapNew");
    g_tap_get_source = (PFN_AudioQueueProcessingTapGetSourceAudio)GetProcAddress(
        coreaudio, "AudioQueueProcessingTapGetSourceAudio");
    g_tap_get_queue_time = (PFN_AudioQueueProcessingTapGetQueueTime)GetProcAddress(
        coreaudio, "AudioQueueProcessingTapGetQueueTime");
    g_converter_get_property = (PFN_AudioConverterGetProperty)GetProcAddress(
        coreaudio, "AudioConverterGetProperty");
    LOGI("coreaudio processing tap: %s", g_tap_new ? "available" : "missing");
    LOGI("coreaudio converter get property: %s", g_converter_get_property ? "available" : "missing");
    for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
        void* fn = (void*)GetProcAddress(coreaudio, g_hooks[i].name);
        if (fn) *g_hooks[i].original = fn;
    }

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) {
        LOGW("MinHook init failed: %s", MH_StatusToString(mh));
    } else {
        const char* errors[7];
        errors[0] = InstallInlineHook(coreaudio, "AudioConverterNew",
                                      (void*)Hook_AudioConverterNew,
                                      (void**)&g_converter_new);
        errors[1] = InstallInlineHook(coreaudio, "AudioConverterNewSpecific",
                                      (void*)Hook_AudioConverterNewSpecific,
                                      (void**)&g_converter_specific);
        errors[2] = InstallInlineHook(coreaudio, "AudioConverterConvertBuffer",
                                      (void*)Hook_AudioConverterConvertBuffer,
                                      (void**)&g_convert_buffer);
        errors[3] = InstallInlineHook(coreaudio, "AudioConverterFillBuffer",
                                      (void*)Hook_AudioConverterFillBuffer,
                                      (void**)&g_fill_buffer);
        errors[4] = InstallInlineHook(coreaudio, "AudioConverterFillComplexBuffer",
                                      (void*)Hook_AudioConverterFillComplexBuffer,
                                      (void**)&g_fill_complex);
        errors[5] = InstallInlineHook(coreaudio, "AudioConverterDispose",
                                      (void*)Hook_AudioConverterDispose,
                                      (void**)&g_converter_dispose);
        errors[6] = NULL;
        const char* enqueue_error =
            InstallInlineHook(coreaudio, "AudioQueueEnqueueBufferWithParameters",
                              (void*)Hook_AudioQueueEnqueueBufferWithParameters,
                              (void**)&g_queue_enqueue);
        const char* setprop_error = InstallInlineHook(coreaudio, "AudioUnitSetProperty",
                                                      (void*)Hook_AudioUnitSetProperty,
                                                      (void**)&g_unit_set_property);
        if (enqueue_error && strcmp(enqueue_error, "missing") != 0) {
            LOGW("AudioQueue enqueue inline hook failed: %s", enqueue_error);
        }
        if (setprop_error && strcmp(setprop_error, "missing") != 0) {
            LOGW("AudioUnitSetProperty inline hook failed: %s", setprop_error);
        }
        for (int i = 0; i < 7; ++i) {
            if (errors[i] && strcmp(errors[i], "missing") != 0) {
                LOGW("coreaudio inline hook failed: %s", errors[i]);
            }
        }
        MH_STATUS enable = MH_EnableHook(MH_ALL_HOOKS);
        LOGI("coreaudio inline hooks enabled: %s", MH_StatusToString(enable));
    }

    for (int pass = 0; pass < 300; ++pass) {
        PatchAllModules();
        Sleep(100);
    }
    LOGI("coreaudio hook scan complete");
    return 0;
}

void AppHooks_Start(HMODULE module)
{
    g_module = module;
    AppHooks_ResetCapture();
    HANDLE thread = CreateThread(NULL, 0, AppHooksThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

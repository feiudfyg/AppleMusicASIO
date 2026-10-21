#include "hooks.h"
#include "config.h"
#include "log.h"
#include "stream.h"
#include "sink.h"
#include "asio_host.h"
#include "asio_iface.h"

typedef HRESULT (STDMETHODCALLTYPE* PFN_Initialize)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD,
                                                    REFERENCE_TIME, REFERENCE_TIME,
                                                    const WAVEFORMATEX*, LPCGUID);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetBufferSize)(IAudioClient*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetStreamLatency)(IAudioClient*, REFERENCE_TIME*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetCurrentPadding)(IAudioClient*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_IsFormatSupported)(IAudioClient*, AUDCLNT_SHAREMODE,
                                                           const WAVEFORMATEX*, WAVEFORMATEX**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetMixFormat)(IAudioClient*, WAVEFORMATEX**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDevicePeriod)(IAudioClient*, REFERENCE_TIME*,
                                                         REFERENCE_TIME*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Start)(IAudioClient*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Stop)(IAudioClient*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Reset)(IAudioClient*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SetEventHandle)(IAudioClient*, HANDLE);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetService)(IAudioClient*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SetClientProperties)(IAudioClient*,
                                                             const AudioClientProperties*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetBufferSizeLimits)(IAudioClient*, const WAVEFORMATEX*,
                                                             BOOL, REFERENCE_TIME*,
                                                             REFERENCE_TIME*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetSharedModeEnginePeriod)(IAudioClient*,
                                                                    const WAVEFORMATEX*,
                                                                    UINT32*, UINT32*,
                                                                    UINT32*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetCurrentSharedModeEnginePeriod)(IAudioClient*,
                                                                           WAVEFORMATEX**,
                                                                           UINT32*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_InitializeSharedAudioStream)(IAudioClient*, DWORD, UINT32,
                                                                     const WAVEFORMATEX*, LPCGUID);
typedef ULONG (STDMETHODCALLTYPE* PFN_Release)(IUnknown*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Activate)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetId)(IMMDevice*, LPWSTR*);

typedef struct VtblCtx {
    void** vtbl;
    void* orig[21];
    BOOL has2;
    BOOL has3;
} VtblCtx;

static SRWLOCK g_ctx_lock = SRWLOCK_INIT;
static VtblCtx g_ctx[AMB_MAX_VTABLES];
static int g_ctx_count = 0;

static HRESULT (STDMETHODCALLTYPE* g_device_activate_orig)(IMMDevice*, REFIID, DWORD,
                                                           PROPVARIANT*, void**);
static void** g_device_vtbl = NULL;

static volatile LONG g_mode = AMB_MODE_AUTO;

static ULONG STDMETHODCALLTYPE Hook_Release(IAudioClient* This);
static HRESULT STDMETHODCALLTYPE Hook_Initialize(IAudioClient* This, AUDCLNT_SHAREMODE share,
                                                  DWORD flags, REFERENCE_TIME buffer_duration,
                                                  REFERENCE_TIME periodicity,
                                                  const WAVEFORMATEX* format, LPCGUID session);
static HRESULT STDMETHODCALLTYPE Hook_GetBufferSize(IAudioClient* This, UINT32* frames);
static HRESULT STDMETHODCALLTYPE Hook_GetStreamLatency(IAudioClient* This, REFERENCE_TIME* latency);
static HRESULT STDMETHODCALLTYPE Hook_GetCurrentPadding(IAudioClient* This, UINT32* padding);
static HRESULT STDMETHODCALLTYPE Hook_IsFormatSupported(IAudioClient* This,
                                                        AUDCLNT_SHAREMODE share,
                                                        const WAVEFORMATEX* format,
                                                        WAVEFORMATEX** closest);
static HRESULT STDMETHODCALLTYPE Hook_GetMixFormat(IAudioClient* This, WAVEFORMATEX** format);
static HRESULT STDMETHODCALLTYPE Hook_GetDevicePeriod(IAudioClient* This,
                                                      REFERENCE_TIME* default_period,
                                                      REFERENCE_TIME* min_period);
static HRESULT STDMETHODCALLTYPE Hook_Start(IAudioClient* This);
static HRESULT STDMETHODCALLTYPE Hook_Stop(IAudioClient* This);
static HRESULT STDMETHODCALLTYPE Hook_Reset(IAudioClient* This);
static HRESULT STDMETHODCALLTYPE Hook_SetEventHandle(IAudioClient* This, HANDLE event);
static HRESULT STDMETHODCALLTYPE Hook_GetService(IAudioClient* This, REFIID riid, void** ppv);
static HRESULT STDMETHODCALLTYPE Hook_SetClientProperties(IAudioClient* This,
                                                          const AudioClientProperties* props);
static HRESULT STDMETHODCALLTYPE Hook_GetBufferSizeLimits(IAudioClient* This,
                                                          const WAVEFORMATEX* format,
                                                          BOOL event_driven,
                                                          REFERENCE_TIME* min_duration,
                                                          REFERENCE_TIME* max_duration);
static HRESULT STDMETHODCALLTYPE Hook_GetSharedModeEnginePeriod(IAudioClient* This,
                                                                const WAVEFORMATEX* format,
                                                                UINT32* default_period,
                                                                UINT32* fundamental_period,
                                                                UINT32* min_period,
                                                                UINT32* max_period);
static HRESULT STDMETHODCALLTYPE Hook_GetCurrentSharedModeEnginePeriod(IAudioClient* This,
                                                                       WAVEFORMATEX** format,
                                                                       UINT32* frames);
static HRESULT STDMETHODCALLTYPE Hook_InitializeSharedAudioStream(IAudioClient* This,
                                                                  DWORD flags,
                                                                  UINT32 period_frames,
                                                                  const WAVEFORMATEX* format,
                                                                  LPCGUID session);
static HRESULT STDMETHODCALLTYPE Hook_Activate(IMMDevice* This, REFIID iid, DWORD clsctx,
                                               PROPVARIANT* params, void** ppv);

static AmbMode CurrentMode(void)
{
    return (AmbMode)InterlockedCompareExchange(&g_mode, AMB_MODE_AUTO, AMB_MODE_AUTO);
}

static VtblCtx* CtxForObj(void* obj)
{
    if (!obj) return NULL;
    void** vtbl = *(void***)obj;
    AcquireSRWLockShared(&g_ctx_lock);
    VtblCtx* found = NULL;
    for (int i = 0; i < g_ctx_count; ++i) {
        if (g_ctx[i].vtbl == vtbl) {
            found = &g_ctx[i];
            break;
        }
    }
    ReleaseSRWLockShared(&g_ctx_lock);
    return found;
}

static RealClientFns MakeFns(VtblCtx* c)
{
    RealClientFns f;
    ZeroMemory(&f, sizeof(f));
    f.Initialize = (PFN_Initialize)c->orig[3];
    f.GetBufferSize = (PFN_GetBufferSize)c->orig[4];
    f.GetStreamLatency = (PFN_GetStreamLatency)c->orig[5];
    f.GetCurrentPadding = (PFN_GetCurrentPadding)c->orig[6];
    f.IsFormatSupported = (PFN_IsFormatSupported)c->orig[7];
    f.GetMixFormat = (PFN_GetMixFormat)c->orig[8];
    f.GetDevicePeriod = (PFN_GetDevicePeriod)c->orig[9];
    f.Start = (PFN_Start)c->orig[10];
    f.Stop = (PFN_Stop)c->orig[11];
    f.Reset = (PFN_Reset)c->orig[12];
    f.SetEventHandle = (PFN_SetEventHandle)c->orig[13];
    f.GetService = (PFN_GetService)c->orig[14];
    return f;
}

static void* HookAt(int index)
{
    switch (index) {
    case 2: return (void*)Hook_Release;
    case 3: return (void*)Hook_Initialize;
    case 4: return (void*)Hook_GetBufferSize;
    case 5: return (void*)Hook_GetStreamLatency;
    case 6: return (void*)Hook_GetCurrentPadding;
    case 7: return (void*)Hook_IsFormatSupported;
    case 8: return (void*)Hook_GetMixFormat;
    case 9: return (void*)Hook_GetDevicePeriod;
    case 10: return (void*)Hook_Start;
    case 11: return (void*)Hook_Stop;
    case 12: return (void*)Hook_Reset;
    case 13: return (void*)Hook_SetEventHandle;
    case 14: return (void*)Hook_GetService;
    case 16: return (void*)Hook_SetClientProperties;
    case 17: return (void*)Hook_GetBufferSizeLimits;
    case 18: return (void*)Hook_GetSharedModeEnginePeriod;
    case 19: return (void*)Hook_GetCurrentSharedModeEnginePeriod;
    case 20: return (void*)Hook_InitializeSharedAudioStream;
    default: return NULL;
    }
}

static BOOL InstallClientCtx(void** vtbl, BOOL has2, BOOL has3)
{
    if (!vtbl) return FALSE;
    if (has3) has2 = TRUE;

    AcquireSRWLockExclusive(&g_ctx_lock);
    for (int i = 0; i < g_ctx_count; ++i) {
        if (g_ctx[i].vtbl == vtbl) {
            ReleaseSRWLockExclusive(&g_ctx_lock);
            return TRUE;
        }
    }
    if (g_ctx_count >= AMB_MAX_VTABLES) {
        ReleaseSRWLockExclusive(&g_ctx_lock);
        LOGE("too many IAudioClient vtables; not hooking %p", (void*)vtbl);
        return FALSE;
    }

    VtblCtx* c = &g_ctx[g_ctx_count];
    ZeroMemory(c, sizeof(*c));
    c->vtbl = vtbl;
    c->has2 = has2;
    c->has3 = has3;

    int slots = has3 ? 21 : has2 ? 18 : 15;
    for (int i = 0; i < slots; ++i) c->orig[i] = vtbl[i];

    DWORD old_protect = 0;
    if (!VirtualProtect(vtbl, (SIZE_T)slots * sizeof(void*), PAGE_READWRITE, &old_protect)) {
        ReleaseSRWLockExclusive(&g_ctx_lock);
        LOGE("VirtualProtect failed for IAudioClient vtable");
        return FALSE;
    }
    for (int i = 0; i < slots; ++i) {
        void* hook = HookAt(i);
        if (hook) vtbl[i] = hook;
    }
    DWORD ignored = 0;
    VirtualProtect(vtbl, (SIZE_T)slots * sizeof(void*), old_protect, &ignored);

    g_ctx_count++;
    ReleaseSRWLockExclusive(&g_ctx_lock);

    HMODULE mod = NULL;
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)vtbl, &mod)) {
        GetModuleFileNameW(mod, path, MAX_PATH);
    }
    LOGI("hooked IAudioClient vtable %p (module %ls, IAudioClient2=%d IAudioClient3=%d)",
         (void*)vtbl, path[0] ? path : L"?", has2, has3);
    return TRUE;
}

static void InstallDeviceCtx(void** vtbl)
{
    if (!vtbl || g_device_vtbl) return;
    g_device_vtbl = vtbl;
    g_device_activate_orig = (PFN_Activate)vtbl[3];

    DWORD old_protect = 0;
    if (!VirtualProtect(&vtbl[3], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        LOGE("VirtualProtect failed for IMMDevice vtable");
        return;
    }
    vtbl[3] = (void*)Hook_Activate;
    DWORD ignored = 0;
    VirtualProtect(&vtbl[3], sizeof(void*), old_protect, &ignored);
    LOGI("hooked IMMDevice::Activate at %p", (void*)vtbl[3]);
}

static BOOL BuildAsioPlan(const AmbAudioFormat* mix, AmbAudioFormat* out)
{
    UINT32 channels = g_cfg.asio_channels > 0 ? (UINT32)g_cfg.asio_channels : mix->channels;
    double rates[3];
    int count = 0;
    if (g_cfg.asio_rate > 0) {
        rates[count++] = (double)g_cfg.asio_rate;
    } else {
        rates[count++] = (double)mix->rate;
        if (mix->rate != 48000) rates[count++] = 48000.0;
        if (mix->rate != 44100) rates[count++] = 44100.0;
    }

    for (int i = 0; i < count; ++i) {
        AsioFormatInfo info;
        HRESULT hr = AsioHost_Probe(rates[i], (int)channels, g_cfg.asio_buffer_frames, &info);
        if (FAILED(hr)) continue;
        if (!Asio_MapNativeFormat(info.sample_type, (UINT32)info.rate,
                                  (UINT32)info.channels, out)) {
            LOGE("ASIO: cannot expose sample type %s", Asio_SampleTypeName(info.sample_type));
            return FALSE;
        }
        char text[128];
        Format_ToString(out, text, sizeof(text));
        LOGI("ASIO plan: %s (%s), %ld frames", text,
             Asio_SampleTypeName(info.sample_type), info.buffer_frames);
        if (info.rate != (double)mix->rate) {
            LOGW("ASIO: driver cannot run at %lu Hz, plan uses %.0f Hz; the app must resample",
                 (unsigned long)mix->rate, info.rate);
        }
        return TRUE;
    }
    return FALSE;
}

static HRESULT BeginBridge(Stream* s, VtblCtx* c, const WAVEFORMATEX* pFormat)
{
    AmbAudioFormat fmt;
    if (!Format_Parse(&fmt, pFormat)) {
        LOGW("bridge: unsupported app format; not intercepting");
        return E_INVALIDARG;
    }
    RealClientFns fns = MakeFns(c);

    AmbMode mode = CurrentMode();
    if (mode == AMB_MODE_ASIO) {
        if (Stream_BeginFake(s, &fmt, AMB_MODE_ASIO, &fns)) {
            LOGI("bridge: ASIO stream active (bit-perfect)");
            return S_OK;
        }
        LOGW("bridge: ASIO setup failed, trying WASAPI exclusive");
        mode = AMB_MODE_EXCLUSIVE;
    }
    if (mode == AMB_MODE_EXCLUSIVE) {
        if (Stream_BeginFake(s, &fmt, AMB_MODE_EXCLUSIVE, &fns)) {
            LOGI("bridge: WASAPI exclusive stream active (bit-perfect)");
            return S_OK;
        }
    }
    return E_FAIL;
}

static void LogFormat(const char* what, const WAVEFORMATEX* wf)
{
    AmbAudioFormat f;
    char text[128] = "?";
    if (wf && Format_Parse(&f, wf)) Format_ToString(&f, text, sizeof(text));
    LOGD("%s: %s", what, text);
}

static ULONG STDMETHODCALLTYPE Hook_Release(IAudioClient* This)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return 0;
    PFN_Release orig = (PFN_Release)c->orig[2];
    ULONG refs = orig((IUnknown*)This);
    if (refs == 0) {
        LOGD("IAudioClient released");
        Stream_Remove(This);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE Hook_GetMixFormat(IAudioClient* This, WAVEFORMATEX** format)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    PFN_GetMixFormat orig = (PFN_GetMixFormat)c->orig[8];
    HRESULT hr = orig(This, format);
    if (FAILED(hr) || !format || !*format) return hr;

    AmbMode mode = CurrentMode();
    LogFormat("GetMixFormat (device)", *format);
    if (mode == AMB_MODE_MONITOR || mode == AMB_MODE_OFF) return hr;

    AmbAudioFormat mix;
    if (!Format_Parse(&mix, *format)) {
        LOGW("GetMixFormat: unsupported device format, passing through");
        return hr;
    }

    if (mode == AMB_MODE_ASIO) {
        AmbAudioFormat plan;
        if (BuildAsioPlan(&mix, &plan)) {
            WAVEFORMATEX* replacement = Format_AllocCo(&plan);
            if (replacement) {
                CoTaskMemFree(*format);
                *format = replacement;
                Stream* s = Stream_Get(This, TRUE);
                if (s) {
                    s->plan_fmt = plan;
                    s->have_plan = TRUE;
                }
                return S_OK;
            }
        }
        LOGW("GetMixFormat: ASIO unavailable, switching to WASAPI exclusive");
        InterlockedExchange(&g_mode, AMB_MODE_EXCLUSIVE);
        mode = AMB_MODE_EXCLUSIVE;
    }

    if (mode == AMB_MODE_EXCLUSIVE) {
        RealClientFns fns = MakeFns(c);
        AmbAudioFormat chosen;
        if (Wasapi_PickExclusive(This, &fns, &mix, &chosen)) {
            WAVEFORMATEX* replacement = Format_AllocCo(&chosen);
            if (replacement) {
                CoTaskMemFree(*format);
                *format = replacement;
                Stream* s = Stream_Get(This, TRUE);
                if (s) {
                    s->plan_fmt = chosen;
                    s->have_plan = TRUE;
                }
                char text[128];
                Format_ToString(&chosen, text, sizeof(text));
                LOGI("exclusive plan: %s", text);
                return S_OK;
            }
        }
        LOGW("GetMixFormat: no exclusive format for this endpoint, shared mode will be used");
        Stream* s = Stream_Get(This, TRUE);
        if (s) s->have_plan = FALSE;
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_IsFormatSupported(IAudioClient* This,
                                                        AUDCLNT_SHAREMODE share,
                                                        const WAVEFORMATEX* format,
                                                        WAVEFORMATEX** closest)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    PFN_IsFormatSupported orig = (PFN_IsFormatSupported)c->orig[7];

    AmbMode mode = CurrentMode();
    if (mode == AMB_MODE_MONITOR || mode == AMB_MODE_OFF) {
        return orig(This, share, format, closest);
    }

    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake && format) {
        AmbAudioFormat f;
        if (Format_Parse(&f, format) && Format_Equal(&f, &s->app_fmt)) {
            if (closest) *closest = NULL;
            return S_OK;
        }
        if (share == AUDCLNT_SHAREMODE_SHARED) {
            if (closest) *closest = Format_AllocCo(&s->app_fmt);
            return S_FALSE;
        }
        if (closest) *closest = NULL;
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    if (s && s->have_plan && format) {
        AmbAudioFormat f;
        if (Format_Parse(&f, format) && Format_Equal(&f, &s->plan_fmt)) {
            if (closest) *closest = NULL;
            return S_OK;
        }
    }

    return orig(This, share, format, closest);
}

static HRESULT STDMETHODCALLTYPE Hook_Initialize(IAudioClient* This, AUDCLNT_SHAREMODE share,
                                                  DWORD flags, REFERENCE_TIME buffer_duration,
                                                  REFERENCE_TIME periodicity,
                                                  const WAVEFORMATEX* format, LPCGUID session)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    PFN_Initialize orig = (PFN_Initialize)c->orig[3];

    char text[128] = "?";
    AmbAudioFormat fmt;
    BOOL parsed = format && Format_Parse(&fmt, format);
    if (parsed) Format_ToString(&fmt, text, sizeof(text));
    LOGI("Initialize: share=%d flags=0x%08lx buffer=%lld period=%lld format=%s",
         (int)share, (unsigned long)flags, (long long)buffer_duration,
         (long long)periodicity, text);

    AmbMode mode = CurrentMode();
    if (mode == AMB_MODE_MONITOR || mode == AMB_MODE_OFF || !parsed) {
        return orig(This, share, flags, buffer_duration, periodicity, format, session);
    }

    Stream* s = Stream_Get(This, TRUE);
    if (!s) return orig(This, share, flags, buffer_duration, periodicity, format, session);

    if (s->fake) Stream_EndFake(s);
    HRESULT hr = BeginBridge(s, c, format);
    if (SUCCEEDED(hr)) return S_OK;

    LOGW("bridge: initialization failed, falling back to the normal Windows path");
    Wasapi_EnableRaw(This);
    return orig(This, share, flags, buffer_duration, periodicity, format, session);
}

static HRESULT STDMETHODCALLTYPE Hook_GetBufferSize(IAudioClient* This, UINT32* frames)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (!frames) return E_POINTER;
        *frames = s->ring.cap_frames;
        return S_OK;
    }
    return ((PFN_GetBufferSize)c->orig[4])(This, frames);
}

static HRESULT STDMETHODCALLTYPE Hook_GetStreamLatency(IAudioClient* This, REFERENCE_TIME* latency)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (!latency) return E_POINTER;
        *latency = (REFERENCE_TIME)s->period_frames * 10000000 / s->app_fmt.rate;
        return S_OK;
    }
    return ((PFN_GetStreamLatency)c->orig[5])(This, latency);
}

static HRESULT STDMETHODCALLTYPE Hook_GetCurrentPadding(IAudioClient* This, UINT32* padding)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (!padding) return E_POINTER;
        *padding = Ring_Fill(&s->ring);
        return S_OK;
    }
    return ((PFN_GetCurrentPadding)c->orig[6])(This, padding);
}

static HRESULT STDMETHODCALLTYPE Hook_GetDevicePeriod(IAudioClient* This,
                                                      REFERENCE_TIME* default_period,
                                                      REFERENCE_TIME* min_period)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        REFERENCE_TIME t = (REFERENCE_TIME)s->period_frames * 10000000 / s->app_fmt.rate;
        if (default_period) *default_period = t;
        if (min_period) *min_period = t;
        return S_OK;
    }
    return ((PFN_GetDevicePeriod)c->orig[9])(This, default_period, min_period);
}

static HRESULT STDMETHODCALLTYPE Hook_Start(IAudioClient* This)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (s->started) return S_OK;
        if (!s->sink) return AUDCLNT_E_NOT_INITIALIZED;
        HRESULT hr = s->sink->Start(s->sink);
        if (SUCCEEDED(hr)) {
            s->started = TRUE;
            Stream_Signal(s);
        }
        return hr;
    }
    return ((PFN_Start)c->orig[10])(This);
}

static HRESULT STDMETHODCALLTYPE Hook_Stop(IAudioClient* This)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (s->started && s->sink) s->sink->Stop(s->sink);
        s->started = FALSE;
        Stream_Signal(s);
        return S_OK;
    }
    return ((PFN_Stop)c->orig[11])(This);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IAudioClient* This)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (s->started && s->sink) s->sink->Stop(s->sink);
        s->started = FALSE;
        Ring_Clear(&s->ring);
        s->frames_played = 0;
        Stream_Signal(s);
        return S_OK;
    }
    return ((PFN_Reset)c->orig[12])(This);
}

static HRESULT STDMETHODCALLTYPE Hook_SetEventHandle(IAudioClient* This, HANDLE event)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, TRUE);
    if (s) s->app_event = event;

    if (s && s->fake) {
        Stream_Signal(s);
        return S_OK;
    }
    return ((PFN_SetEventHandle)c->orig[13])(This, event);
}

static void LogIid(const IID* iid, wchar_t* out, size_t count)
{
    if (!iid || !StringFromGUID2(iid, out, (int)count)) {
        if (out && count) out[0] = 0;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_GetService(IAudioClient* This, REFIID riid, void** ppv)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    if (!ppv) return E_POINTER;

    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        *ppv = NULL;
        if (IsEqualIID(riid, &IID_IAudioRenderClient)) {
            *ppv = Stream_GetRenderClient(s);
            if (!*ppv) return E_OUTOFMEMORY;
            return S_OK;
        }
        if (IsEqualIID(riid, &IID_IAudioClock)) {
            *ppv = Stream_GetClock(s);
            if (!*ppv) return E_OUTOFMEMORY;
            return S_OK;
        }
        if (IsEqualIID(riid, &IID_ISimpleAudioVolume)) {
            *ppv = Stream_GetVolume(s);
            if (!*ppv) return E_OUTOFMEMORY;
            return S_OK;
        }
        if (IsEqualIID(riid, &IID_IAudioStreamVolume)) {
            *ppv = Stream_GetStreamVolume(s);
            if (!*ppv) return E_OUTOFMEMORY;
            return S_OK;
        }
        wchar_t iid_text[64];
        LogIid(riid, iid_text, 64);
        LOGW("GetService(%ls) unsupported in bridge mode", iid_text);
        return E_NOINTERFACE;
    }
    return ((PFN_GetService)c->orig[14])(This, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE Hook_SetClientProperties(IAudioClient* This,
                                                          const AudioClientProperties* props)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    if (props) {
        LOGD("SetClientProperties: category=%d offload=%d options=0x%08lx",
             (int)props->eCategory, props->bIsOffload,
             (unsigned long)props->Options);
    }
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) return S_OK;
    return ((PFN_SetClientProperties)c->orig[16])(This, props);
}

static HRESULT STDMETHODCALLTYPE Hook_GetBufferSizeLimits(IAudioClient* This,
                                                          const WAVEFORMATEX* format,
                                                          BOOL event_driven,
                                                          REFERENCE_TIME* min_duration,
                                                          REFERENCE_TIME* max_duration)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    AMB_UNUSED(format);
    AMB_UNUSED(event_driven);
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        REFERENCE_TIME t = (REFERENCE_TIME)s->period_frames * 10000000 / s->app_fmt.rate;
        if (min_duration) *min_duration = t;
        if (max_duration) *max_duration = t * 32;
        return S_OK;
    }
    return ((PFN_GetBufferSizeLimits)c->orig[17])(This, format, event_driven,
                                                   min_duration, max_duration);
}

static HRESULT STDMETHODCALLTYPE Hook_GetSharedModeEnginePeriod(IAudioClient* This,
                                                                const WAVEFORMATEX* format,
                                                                UINT32* default_period,
                                                                UINT32* fundamental_period,
                                                                UINT32* min_period,
                                                                UINT32* max_period)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    AMB_UNUSED(format);
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (default_period) *default_period = s->period_frames;
        if (fundamental_period) *fundamental_period = s->period_frames;
        if (min_period) *min_period = s->period_frames;
        if (max_period) *max_period = s->period_frames;
        return S_OK;
    }
    return ((PFN_GetSharedModeEnginePeriod)c->orig[18])(This, format, default_period,
                                                        fundamental_period, min_period,
                                                        max_period);
}

static HRESULT STDMETHODCALLTYPE Hook_GetCurrentSharedModeEnginePeriod(IAudioClient* This,
                                                                       WAVEFORMATEX** format,
                                                                       UINT32* frames)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    Stream* s = Stream_Get(This, FALSE);
    if (s && s->fake) {
        if (format) *format = Format_AllocCo(&s->app_fmt);
        if (frames) *frames = s->period_frames;
        return S_OK;
    }
    return ((PFN_GetCurrentSharedModeEnginePeriod)c->orig[19])(This, format, frames);
}

static HRESULT STDMETHODCALLTYPE Hook_InitializeSharedAudioStream(IAudioClient* This,
                                                                  DWORD flags,
                                                                  UINT32 period_frames,
                                                                  const WAVEFORMATEX* format,
                                                                  LPCGUID session)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    LOGI("InitializeSharedAudioStream: flags=0x%08lx period=%lu", (unsigned long)flags,
         (unsigned long)period_frames);

    AmbMode mode = CurrentMode();
    if (mode == AMB_MODE_MONITOR || mode == AMB_MODE_OFF || !format) {
        return ((PFN_InitializeSharedAudioStream)c->orig[20])(This, flags, period_frames,
                                                             format, session);
    }

    Stream* s = Stream_Get(This, TRUE);
    if (!s) {
        return ((PFN_InitializeSharedAudioStream)c->orig[20])(This, flags, period_frames,
                                                             format, session);
    }
    if (s->fake) Stream_EndFake(s);
    if (SUCCEEDED(BeginBridge(s, c, format))) return S_OK;

    LOGW("InitializeSharedAudioStream bridge failed; falling back");
    return ((PFN_InitializeSharedAudioStream)c->orig[20])(This, flags, period_frames, format,
                                                         session);
}

static HRESULT STDMETHODCALLTYPE Hook_Activate(IMMDevice* This, REFIID iid, DWORD clsctx,
                                               PROPVARIANT* params, void** ppv)
{
    if (!g_device_activate_orig) return E_FAIL;
    HRESULT hr = g_device_activate_orig(This, iid, clsctx, params, ppv);
    if (FAILED(hr) || !ppv || !*ppv) return hr;

    BOOL audio_client = IsEqualIID(iid, &IID_IAudioClient) ||
                        IsEqualIID(iid, &IID_IAudioClient2) ||
                        IsEqualIID(iid, &IID_IAudioClient3);
    if (audio_client) {
        BOOL has2 = FALSE, has3 = FALSE;
        IUnknown* unk = (IUnknown*)*ppv;
        IAudioClient3* a3 = NULL;
        if (SUCCEEDED(unk->lpVtbl->QueryInterface(unk, &IID_IAudioClient3, (void**)&a3)) && a3) {
            has3 = TRUE;
            has2 = TRUE;
            a3->lpVtbl->Release(a3);
        } else {
            IAudioClient2* a2 = NULL;
            if (SUCCEEDED(unk->lpVtbl->QueryInterface(unk, &IID_IAudioClient2, (void**)&a2)) && a2) {
                has2 = TRUE;
                a2->lpVtbl->Release(a2);
            }
        }
        InstallClientCtx(*(void***)*ppv, has2, has3);

        LPWSTR device_id = NULL;
        if (SUCCEEDED(This->lpVtbl->GetId(This, &device_id)) && device_id) {
            wchar_t iid_text[64];
            LogIid(iid, iid_text, 64);
            LOGI("IMMDevice::Activate %ls -> %ls", iid_text, device_id);
            CoTaskMemFree(device_id);
        }
    }
    return hr;
}

BOOL Hooks_Install(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    AmbMode effective = g_cfg.mode;
    if (effective == AMB_MODE_AUTO) {
        effective = Asio_Available(g_cfg.asio_driver) ? AMB_MODE_ASIO : AMB_MODE_EXCLUSIVE;
    }
    InterlockedExchange(&g_mode, effective);
    LOGI("effective mode: %s", Config_ModeName(effective));
    if (effective == AMB_MODE_MONITOR) {
        LOGI("monitor mode: every render stream is logged but forwarded unchanged");
    } else if (effective == AMB_MODE_ASIO) {
        LOGI("ASIO driver preference: %ls", g_cfg.asio_driver);
    }

    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* device = NULL;
    IAudioClient* client = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        LOGE("MMDeviceEnumerator creation failed (hr=0x%08lx)", (unsigned long)hr);
        return FALSE;
    }
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        LOGE("no default render endpoint (hr=0x%08lx)", (unsigned long)hr);
        enumerator->lpVtbl->Release(enumerator);
        return FALSE;
    }
    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL,
                                  (void**)&client);
    if (FAILED(hr) || !client) {
        LOGE("default render IAudioClient activation failed (hr=0x%08lx)", (unsigned long)hr);
        device->lpVtbl->Release(device);
        enumerator->lpVtbl->Release(enumerator);
        return FALSE;
    }

    BOOL has2 = FALSE, has3 = FALSE;
    IAudioClient3* a3 = NULL;
    if (SUCCEEDED(client->lpVtbl->QueryInterface(client, &IID_IAudioClient3, (void**)&a3)) && a3) {
        has3 = TRUE;
        has2 = TRUE;
        a3->lpVtbl->Release(a3);
    } else {
        IAudioClient2* a2 = NULL;
        if (SUCCEEDED(client->lpVtbl->QueryInterface(client, &IID_IAudioClient2, (void**)&a2)) && a2) {
            has2 = TRUE;
            a2->lpVtbl->Release(a2);
        }
    }

    InstallDeviceCtx(*(void***)device);
    InstallClientCtx(*(void***)client, has2, has3);

    client->lpVtbl->Release(client);
    device->lpVtbl->Release(device);
    enumerator->lpVtbl->Release(enumerator);
    return TRUE;
}

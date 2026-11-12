#include "hooks.h"
#include "config.h"
#include "log.h"
#include "stream.h"
#include "sink.h"
#include "asio_host.h"
#include "asio_iface.h"
#include "app_hooks.h"

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
static void** g_enum_vtbl = NULL;

static volatile LONG g_mode = AMB_MODE_AUTO;
static Stream* volatile g_capture_stream = NULL;
static SRWLOCK g_capture_stream_lock = SRWLOCK_INIT;
static Stream* volatile g_active_stream = NULL;
static SRWLOCK g_active_stream_lock = SRWLOCK_INIT;

typedef struct ClientEndpoint {
    IAudioClient* client;
    BOOL is_default;
} ClientEndpoint;

static SRWLOCK g_endpoint_lock = SRWLOCK_INIT;
static ClientEndpoint g_endpoints[64];
static int g_endpoint_count = 0;
static wchar_t g_default_endpoint[256];

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

static void RefreshMode(void)
{
    if (!Config_ReloadIfChanged()) return;
    AmbMode effective = g_cfg.mode;
    if (effective == AMB_MODE_AUTO) {
        effective = Asio_Available(g_cfg.asio_driver) ? AMB_MODE_ASIO : AMB_MODE_EXCLUSIVE;
    }
    InterlockedExchange(&g_mode, effective);
    LOGI("effective mode now: %s", Config_ModeName(effective));
}

void Hooks_WriteCapture(const void* data, uint32_t frames)
{
    AcquireSRWLockShared(&g_capture_stream_lock);
    Stream* s = (Stream*)g_capture_stream;
    if (s) Stream_AddRef(s);
    ReleaseSRWLockShared(&g_capture_stream_lock);

    if (s) {
        Stream_WriteCapture(s, data, frames);
        Stream_Release(s);
    }
}

typedef struct UnitBuffer {
    uint32_t channels;
    uint32_t bytes;
    void* data;
} UnitBuffer;

typedef struct UnitBufferList {
    uint32_t count;
    UnitBuffer buffers[1];
} UnitBufferList;

void Hooks_CaptureAudioUnit(void* io_data, uint32_t frames)
{
    if (!io_data || !frames) return;
    UnitBufferList* list = (UnitBufferList*)io_data;
    if (list->count == 1) {
        const UnitBuffer* b = &list->buffers[0];
        if (!b->data || b->bytes < frames * 8u) return;
        Hooks_WriteCapture(b->data, frames);
        return;
    }
    if (list->count >= 2) {
        const UnitBuffer* left = &list->buffers[0];
        const UnitBuffer* right = &list->buffers[1];
        if (!left->data || !right->data || left->bytes < frames * 4u ||
            right->bytes < frames * 4u) {
            return;
        }
        static float scratch[32768 * 2];
        if (frames > 32768) return;
        const float* l = (const float*)left->data;
        const float* r = (const float*)right->data;
        for (uint32_t i = 0; i < frames; ++i) {
            scratch[i * 2] = l[i];
            scratch[i * 2 + 1] = r[i];
        }
        Hooks_WriteCapture(scratch, frames);
    }
}

static void SetCaptureStream(Stream* s)
{
    Stream* old = NULL;
    AcquireSRWLockExclusive(&g_capture_stream_lock);
    old = (Stream*)g_capture_stream;
    if (s) Stream_AddRef(s);
    g_capture_stream = s;
    ReleaseSRWLockExclusive(&g_capture_stream_lock);
    if (old) Stream_Release(old);
}

static void ClearCaptureStream(Stream* s)
{
    Stream* old = NULL;
    AcquireSRWLockExclusive(&g_capture_stream_lock);
    if ((Stream*)g_capture_stream == s) {
        old = (Stream*)g_capture_stream;
        g_capture_stream = NULL;
    }
    ReleaseSRWLockExclusive(&g_capture_stream_lock);
    if (old) Stream_Release(old);
}

void Hooks_StopCapture(void)
{
    Stream* s = NULL;
    AcquireSRWLockExclusive(&g_capture_stream_lock);
    s = (Stream*)g_capture_stream;
    g_capture_stream = NULL;
    ReleaseSRWLockExclusive(&g_capture_stream_lock);
    if (!s) return;
    LOGI("capture stop requested: stream=%p", (void*)s);
    Stream_EndFake(s);
    Stream_Release(s);
}

static void SetActiveStream(Stream* s)
{
    Stream* old = NULL;
    AcquireSRWLockExclusive(&g_active_stream_lock);
    old = (Stream*)g_active_stream;
    if (s) Stream_AddRef(s);
    g_active_stream = s;
    ReleaseSRWLockExclusive(&g_active_stream_lock);
    if (old) Stream_Release(old);
}

static Stream* GetActiveStream(void)
{
    Stream* s = NULL;
    AcquireSRWLockShared(&g_active_stream_lock);
    s = (Stream*)g_active_stream;
    if (s) Stream_AddRef(s);
    ReleaseSRWLockShared(&g_active_stream_lock);
    return s;
}

static void ClearActiveStream(Stream* s)
{
    Stream* old = NULL;
    AcquireSRWLockExclusive(&g_active_stream_lock);
    if ((Stream*)g_active_stream == s) {
        old = (Stream*)g_active_stream;
        g_active_stream = NULL;
    }
    ReleaseSRWLockExclusive(&g_active_stream_lock);
    if (old) Stream_Release(old);
}

void Hooks_SwitchToCapture(void)
{
    Stream* s = GetActiveStream();
    if (!s) return;

    AmbAudioFormat app_fmt;
    EnterCriticalSection(&s->lock);
    BOOL already = s->capture;
    app_fmt = s->app_fmt;
    LeaveCriticalSection(&s->lock);
    if (already) {
        Stream_Release(s);
        return;
    }

    WAVEFORMATEXTENSIBLE wfx;
    AmbAudioFormat capture_fmt;
    if (!AppHooks_GetCaptureFormat(&wfx.Format, sizeof(wfx)) ||
        !Format_Parse(&capture_fmt, &wfx.Format)) {
        Stream_Release(s);
        return;
    }

    uint32_t capture_rate = AppHooks_GetSourceRate();
    if (!capture_rate) capture_rate = (uint32_t)(capture_fmt.rate + 0.5);
    if (!capture_rate) {
        Stream_Release(s);
        return;
    }
    Format_Make(&capture_fmt, capture_rate, 2, AMB_SAMPLE_F32, 32);
    LOGI("bridge: switching active stream to qlac source capture %lu Hz", (unsigned long)capture_rate);
    Stream_EndFake(s);
    if (Stream_BeginCapture(s, &app_fmt, &capture_fmt)) {
        SetCaptureStream(s);
        if (!Stream_EnsureCaptureSink(s)) {
            LOGW("bridge: qlac capture ASIO pre-open failed");
        }
    }
    Stream_Release(s);
}

static void SetClientEndpoint(IAudioClient* client, BOOL is_default)
{
    if (!client) return;
    AcquireSRWLockExclusive(&g_endpoint_lock);
    for (int i = 0; i < g_endpoint_count; ++i) {
        if (g_endpoints[i].client == client) {
            g_endpoints[i].is_default = is_default;
            ReleaseSRWLockExclusive(&g_endpoint_lock);
            return;
        }
    }
    if (g_endpoint_count < (int)(sizeof(g_endpoints) / sizeof(g_endpoints[0]))) {
        g_endpoints[g_endpoint_count].client = client;
        g_endpoints[g_endpoint_count].is_default = is_default;
        ++g_endpoint_count;
    } else {
        LOGW("endpoint table full; not tracking client %p", (void*)client);
    }
    ReleaseSRWLockExclusive(&g_endpoint_lock);
}

static BOOL ClientIsDefault(IAudioClient* client)
{
    if (!client) return FALSE;
    BOOL found = FALSE;
    AcquireSRWLockShared(&g_endpoint_lock);
    for (int i = 0; i < g_endpoint_count; ++i) {
        if (g_endpoints[i].client == client) {
            found = g_endpoints[i].is_default;
            break;
        }
    }
    ReleaseSRWLockShared(&g_endpoint_lock);
    return found;
}

static void RemoveClientEndpoint(IAudioClient* client)
{
    AcquireSRWLockExclusive(&g_endpoint_lock);
    for (int i = 0; i < g_endpoint_count; ++i) {
        if (g_endpoints[i].client == client) {
            g_endpoints[i] = g_endpoints[g_endpoint_count - 1];
            --g_endpoint_count;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_endpoint_lock);
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

static HRESULT STDMETHODCALLTYPE Hook_RegisterEndpointNotification(
    IMMDeviceEnumerator* This, IMMNotificationClient* client)
{
    AMB_UNUSED(This);
    LOGI("suppressing endpoint notifications for client %p", (void*)client);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Hook_UnregisterEndpointNotification(
    IMMDeviceEnumerator* This, IMMNotificationClient* client)
{
    AMB_UNUSED(This);
    AMB_UNUSED(client);
    return S_OK;
}

static void InstallEnumeratorCtx(void** vtbl)
{
    if (!vtbl || g_enum_vtbl) return;
    g_enum_vtbl = vtbl;

    DWORD old_protect = 0;
    if (!VirtualProtect(&vtbl[6], 2 * sizeof(void*), PAGE_READWRITE, &old_protect)) {
        LOGW("VirtualProtect failed for IMMDeviceEnumerator vtable");
        return;
    }
    vtbl[6] = (void*)Hook_RegisterEndpointNotification;
    vtbl[7] = (void*)Hook_UnregisterEndpointNotification;
    DWORD ignored = 0;
    VirtualProtect(&vtbl[6], 2 * sizeof(void*), old_protect, &ignored);
    LOGI("suppressed IMMDeviceEnumerator endpoint notifications");
}

static HRESULT BeginBridge(Stream* s, VtblCtx* c, const WAVEFORMATEX* pFormat)
{
    AmbAudioFormat fmt;
    if (!Format_Parse(&fmt, pFormat)) {
        LOGW("bridge: unsupported app format; not intercepting");
        return E_INVALIDARG;
    }
    RealClientFns fns = MakeFns(c);
    SetCaptureStream(NULL);

    AmbMode mode = CurrentMode();
    uint32_t source_codec = AppHooks_GetSourceCodec();
    BOOL qaac_prelude = source_codec == 0x71616163u;
    if (mode == AMB_MODE_ASIO && g_cfg.follow_source_rate && qaac_prelude) {
        LOGI("bridge: qaac prelude; using normal ASIO/resampling until qlac");
    } else if (mode == AMB_MODE_ASIO && g_cfg.follow_source_rate) {
        WAVEFORMATEXTENSIBLE capture_wfx;
        AmbAudioFormat capture_fmt;
        BOOL have_capture = AppHooks_GetCaptureFormat(&capture_wfx.Format,
                                                      sizeof(capture_wfx)) &&
                            Format_Parse(&capture_fmt, &capture_wfx.Format);
        if (have_capture) {
            uint32_t source_rate = AppHooks_GetSourceRate();
            if (source_rate && source_rate != capture_fmt.rate) {
                LOGI("bridge: using source rate %lu Hz for capture instead of %lu Hz",
                     (unsigned long)source_rate, (unsigned long)capture_fmt.rate);
                capture_fmt.rate = source_rate;
            }
            if (capture_fmt.rate != fmt.rate) {
                LOGI("bridge: source capture format differs from endpoint (%lu -> %lu Hz)",
                     (unsigned long)fmt.rate, (unsigned long)capture_fmt.rate);
            }
            if (Stream_BeginCapture(s, &fmt, &capture_fmt)) {
                /* Publish the capture stream before waiting for prefill.
                   The tap callbacks are the only producer for this ring. */
                SetCaptureStream(s);
                if (!Stream_EnsureCaptureSink(s)) {
                    LOGW("bridge: capture ASIO pre-open failed");
                }
                SetActiveStream(s);
                LOGI("bridge: ASIO AudioUnit capture stream active (%s)",
                     s->bitperfect ? "bit-perfect" : "transparent format conversion");
                return S_OK;
            }
        }
        LOGW("bridge: capture sink setup failed; using normal ASIO path");
    }
    if (mode == AMB_MODE_ASIO) {
        if (Stream_BeginFake(s, &fmt, AMB_MODE_ASIO, &fns)) {
            SetActiveStream(s);
            LOGI("bridge: ASIO stream active (%s)",
                 s->bitperfect ? "bit-perfect" : "transparent format conversion");
            return S_OK;
        }
        LOGW("bridge: ASIO setup failed, trying WASAPI exclusive");
        mode = AMB_MODE_EXCLUSIVE;
    }
    if (mode == AMB_MODE_EXCLUSIVE) {
        if (Stream_BeginFake(s, &fmt, AMB_MODE_EXCLUSIVE, &fns)) {
            SetActiveStream(s);
            LOGI("bridge: WASAPI exclusive stream active (%s)",
                 s->bitperfect ? "bit-perfect" : "transparent format conversion");
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
        Stream* s = Stream_Get(This, FALSE);
        if (s && s->capture) {
            LOGI("capture client released: %p started=%d sink=%p",
                 (void*)s, s->started ? 1 : 0, (void*)s->sink);
        }
        ClearCaptureStream(s);
        ClearActiveStream(s);
        RemoveClientEndpoint(This);
        Stream_Remove(This);
    }
    return refs;
}

static BOOL IsFloatFormat(const WAVEFORMATEX* w)
{
    if (!w) return FALSE;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return TRUE;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        w->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        static const GUID float_subtype = {
            0x00000003, 0x0000, 0x0010,
            {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)w;
        return IsEqualGUID(&e->SubFormat, &float_subtype);
    }
    return FALSE;
}

static BOOL IsSupportedSourceRate(uint32_t rate)
{
    switch (rate) {
    case 44100:
    case 48000:
    case 88200:
    case 96000:
    case 176400:
    case 192000:
        return TRUE;
    default:
        return FALSE;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_GetMixFormat(IAudioClient* This, WAVEFORMATEX** format)
{
    VtblCtx* c = CtxForObj(This);
    if (!c) return E_FAIL;
    PFN_GetMixFormat orig = (PFN_GetMixFormat)c->orig[8];
    HRESULT hr = orig(This, format);
    if (FAILED(hr) || !format || !*format) return hr;

    RefreshMode();
    LogFormat("GetMixFormat (device)", *format);

    /* Endpoint formats are never rewritten.  With capture mode the real
       device format keeps Apple's graph stable; decoded PCM is captured at
       the CoreAudio converter and sent to ASIO at its native rate. */
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

    RefreshMode();
    AmbMode mode = CurrentMode();
    if (mode == AMB_MODE_MONITOR || mode == AMB_MODE_OFF || !parsed) {
        return orig(This, share, flags, buffer_duration, periodicity, format, session);
    }
    if (g_cfg.follow_source_rate && !ClientIsDefault(This)) {
        LOGD("Initialize: non-default endpoint; forwarding");
        return orig(This, share, flags, buffer_duration, periodicity, format, session);
    }
    Stream* s = Stream_Get(This, TRUE);
    if (!s) {
        LOGW("bridge: stream table full or allocation failed; forwarding");
        return orig(This, share, flags, buffer_duration, periodicity, format, session);
    }

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
        *frames = s->capture ? s->app_period_frames : s->ring.cap_frames;
        static volatile LONG calls = 0;
        LONG call = InterlockedIncrement(&calls);
        if (call <= 12) {
            LOGI("GetBufferSize: call=%ld stream=%p capture=%d frames=%lu app=%.0f capture_rate=%.0f",
                 (long)call, (void*)s, s->capture ? 1 : 0, (unsigned long)*frames,
                 (double)s->app_fmt.rate, (double)s->capture_fmt.rate);
        }
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
        *latency = (REFERENCE_TIME)s->app_period_frames * 10000000 / s->app_fmt.rate;
        static volatile LONG calls = 0;
        LONG call = InterlockedIncrement(&calls);
        if (call <= 12) {
            LOGI("GetStreamLatency: call=%ld stream=%p capture=%d latency=%lld app=%.0f capture_rate=%.0f",
                 (long)call, (void*)s, s->capture ? 1 : 0, (long long)*latency,
                 (double)s->app_fmt.rate, (double)s->capture_fmt.rate);
        }
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
        if (!s->capture) {
            *padding = Ring_Fill(&s->ring);
            return S_OK;
        }

        LONG64 submitted = InterlockedCompareExchange64(&s->app_submitted, 0, 0);
        LONG64 played = Stream_GetAppFramesPlayed(s);
        LONG64 pending = submitted - played;
        if (pending < 0) pending = 0;
        if (pending > s->app_period_frames) pending = s->app_period_frames;
        *padding = (UINT32)pending;
        static volatile LONG calls = 0;
        LONG call = InterlockedIncrement(&calls);
        if (call <= 12 || (call % 500) == 0) {
            LOGI("padding: call=%ld stream=%p submitted=%lld played=%lld pending=%lu period=%lu started=%d sink=%d",
                 (long)call, (void*)s, (long long)submitted, (long long)played,
                 (unsigned long)*padding, (unsigned long)s->app_period_frames,
                 s->started ? 1 : 0, s->sink_started ? 1 : 0);
        }
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
        REFERENCE_TIME t = (REFERENCE_TIME)s->app_period_frames * 10000000 / s->app_fmt.rate;
        if (default_period) *default_period = t;
        if (min_period) *min_period = t;
        static volatile LONG calls = 0;
        LONG call = InterlockedIncrement(&calls);
        if (call <= 12) {
            LOGI("GetDevicePeriod: call=%ld stream=%p capture=%d period=%lld app=%.0f capture_rate=%.0f",
                 (long)call, (void*)s, s->capture ? 1 : 0, (long long)t,
                 (double)s->app_fmt.rate, (double)s->capture_fmt.rate);
        }
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
        if (s->capture) {
            s->app_submitted = 0;
            QueryPerformanceCounter(&s->app_clock_start);
            s->started = TRUE;
            Stream_Signal(s);
            if (s->pace_event) SetEvent(s->pace_event);
            return S_OK;
        }
        if (!s->sink) {
            return AUDCLNT_E_NOT_INITIALIZED;
        }
        HRESULT hr = s->sink->Start(s->sink);
        if (SUCCEEDED(hr)) {
            s->started = TRUE;
            s->sink_started = TRUE;
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
        if (s->capture) {
            s->started = FALSE;
            s->app_submitted = 0;
            s->app_clock_start.QuadPart = 0;
            Stream_Signal(s);
            if (s->pace_event) SetEvent(s->pace_event);
            return S_OK;
        }
        if (s->started && s->sink) s->sink->Stop(s->sink);
        s->started = FALSE;
        s->sink_started = FALSE;
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
        if (s->capture) {
            s->started = FALSE;
            s->app_submitted = 0;
            s->app_clock_start.QuadPart = 0;
            Stream_Signal(s);
            if (s->pace_event) SetEvent(s->pace_event);
            return S_OK;
        }
        if (s->started && s->sink) s->sink->Stop(s->sink);
        s->started = FALSE;
        s->sink_started = FALSE;
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
    wchar_t iid_text[64];
    LogIid(riid, iid_text, 64);
    LOGD("GetService %ls (fake=%d)", iid_text, s && s->fake);

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
        if (IsEqualIID(riid, &IID_IAudioSessionControl) ||
            IsEqualIID(riid, &IID_IAudioSessionControl2)) {
            *ppv = Stream_GetSessionControl(s);
            if (!*ppv) return E_OUTOFMEMORY;
            return S_OK;
        }
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
        REFERENCE_TIME t = (REFERENCE_TIME)s->app_period_frames * 10000000 / s->app_fmt.rate;
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
        if (default_period) *default_period = s->app_period_frames;
        if (fundamental_period) *fundamental_period = s->app_period_frames;
        if (min_period) *min_period = s->app_period_frames;
        if (max_period) *max_period = s->app_period_frames;
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
    LOGD("GetCurrentSharedModeEnginePeriod (fake=%d)", s && s->fake);
    if (s && s->fake) {
        if (format) *format = Format_AllocCo(&s->app_fmt);
        if (frames) *frames = s->app_period_frames;
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

HRESULT Hooks_ActivateDevice(IMMDevice* device, REFIID iid, DWORD clsctx,
                             PROPVARIANT* params, void** out)
{
    if (!g_device_activate_orig) return E_FAIL;
    return g_device_activate_orig(device, iid, clsctx, params, out);
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
        BOOL is_default = FALSE;
        if (SUCCEEDED(This->lpVtbl->GetId(This, &device_id)) && device_id) {
            is_default = g_default_endpoint[0] &&
                         _wcsicmp(device_id, g_default_endpoint) == 0;
            wchar_t iid_text[64];
            LogIid(iid, iid_text, 64);
            LOGI("IMMDevice::Activate %ls -> %ls%s", iid_text, device_id,
                 is_default ? " [default]" : "");
            CoTaskMemFree(device_id);
        }
        SetClientEndpoint((IAudioClient*)*ppv, is_default);
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
    InstallEnumeratorCtx(*(void***)enumerator);
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        LOGE("no default render endpoint (hr=0x%08lx)", (unsigned long)hr);
        enumerator->lpVtbl->Release(enumerator);
        return FALSE;
    }
    LPWSTR default_id = NULL;
    if (SUCCEEDED(device->lpVtbl->GetId(device, &default_id)) && default_id) {
        wcsncpy_s(g_default_endpoint,
                  sizeof(g_default_endpoint) / sizeof(g_default_endpoint[0]),
                  default_id, _TRUNCATE);
        LOGI("default render endpoint: %ls", g_default_endpoint);
        CoTaskMemFree(default_id);
    } else {
        LOGW("default render endpoint has no id; bridge will not fake device format");
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

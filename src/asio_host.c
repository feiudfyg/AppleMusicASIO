#include "asio_host.h"
#include "asio_iface.h"
#include "config.h"
#include "log.h"

#include <math.h>

typedef struct AsioState {
    IASIO* asio;
    AsioDriverInfo info;
    double rate;
    long channels;
    long buffer_frames;
    long sample_type;
    long min_buffer;
    long max_buffer;
    long preferred_buffer;
    long granularity;
    ASIOBufferInfo bufs[AMB_MAX_ASIO_OUT];
    ASIOChannelInfo chan[AMB_MAX_ASIO_OUT];
    ASIOCallbacks callbacks;
    volatile LONG running;
    volatile LONG have_buffers;
    volatile LONG rate_changed;
    volatile LONG64 frames_played;
    volatile LONG underruns;
    AsioConsumer consumer;
    uint8_t* scratch;
    size_t scratch_bytes;
} AsioState;

static AsioState g_asio;

static CRITICAL_SECTION g_ctl_call_cs;
static HANDLE g_ctl_cmd_event = NULL;
static HANDLE g_ctl_done_event = NULL;
static HANDLE g_ctl_ready_event = NULL;
static HANDLE g_ctl_thread = NULL;
static HRESULT (*g_ctl_fn)(void*) = NULL;
static void* g_ctl_arg = NULL;
static HRESULT g_ctl_result = E_FAIL;
static volatile LONG g_ctl_quit = 0;
static INIT_ONCE g_ctl_once = INIT_ONCE_STATIC_INIT;

static void Asio_BufferSwitch(long index, ASIOBool direct);
static void Asio_SampleRateChanged(ASIOSampleRate rate);
static long Asio_Message(long selector, long value, void* message, double* opt);
static ASIOTime* Asio_BufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct);

const char* Asio_SampleTypeName(long type)
{
    switch (type) {
    case ASIOSTFloat32LSB: return "float32-le";
    case ASIOSTInt32LSB: return "int32-le";
    case ASIOSTInt32LSB24: return "int24-in-32-le";
    case ASIOSTInt32LSB20: return "int20-in-32-le";
    case ASIOSTInt32LSB18: return "int18-in-32-le";
    case ASIOSTInt32LSB16: return "int16-in-32-le";
    case ASIOSTInt24LSB: return "int24-packed-le";
    case ASIOSTInt16LSB: return "int16-le";
    case ASIOSTFloat64LSB: return "float64-le";
    case ASIOSTFloat32MSB: return "float32-be";
    case ASIOSTInt32MSB: return "int32-be";
    case ASIOSTInt24MSB: return "int24-packed-be";
    case ASIOSTInt16MSB: return "int16-be";
    default: return "unknown";
    }
}

int Asio_EnumDrivers(AsioDriverInfo* out, int max_count)
{
    if (!out || max_count <= 0) return 0;
    HKEY root = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return 0;
    }

    int count = 0;
    for (DWORD i = 0; count < max_count; ++i) {
        wchar_t key[128];
        DWORD key_len = (DWORD)(sizeof(key) / sizeof(key[0]));
        LONG r = RegEnumKeyExW(root, i, key, &key_len, NULL, NULL, NULL, NULL);
        if (r != ERROR_SUCCESS) break;

        HKEY sub = NULL;
        if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &sub) != ERROR_SUCCESS) {
            continue;
        }

        AsioDriverInfo* d = &out[count];
        ZeroMemory(d, sizeof(*d));
        wcsncpy_s(d->key, 128, key, _TRUNCATE);

        DWORD type = 0;
        DWORD size = (DWORD)sizeof(d->description);
        RegQueryValueExW(sub, L"Description", NULL, &type, (BYTE*)d->description, &size);
        size = (DWORD)sizeof(d->clsid);
        RegQueryValueExW(sub, L"CLSID", NULL, &type, (BYTE*)d->clsid, &size);
        RegCloseKey(sub);

        if (d->clsid[0]) ++count;
    }

    RegCloseKey(root);
    return count;
}

BOOL Asio_FindDriver(const wchar_t* wanted, AsioDriverInfo* out)
{
    AsioDriverInfo list[32];
    int n = Asio_EnumDrivers(list, 32);
    if (n <= 0) return FALSE;

    if (!wanted || !wanted[0] || !_wcsicmp(wanted, L"auto")) {
        if (out) *out = list[0];
        return TRUE;
    }

    for (int i = 0; i < n; ++i) {
        if (!_wcsicmp(list[i].key, wanted) ||
            !_wcsicmp(list[i].description, wanted) ||
            (wcsstr(list[i].key, wanted) != NULL) ||
            (wcsstr(list[i].description, wanted) != NULL)) {
            if (out) *out = list[i];
            return TRUE;
        }
    }
    return FALSE;
}

BOOL Asio_Available(const wchar_t* wanted)
{
    return Asio_FindDriver(wanted, NULL);
}

static HRESULT EnsureAsioObject(void)
{
    if (g_asio.asio) return S_OK;
    AsioDriverInfo di;
    if (!Asio_FindDriver(g_cfg.asio_driver, &di)) {
        LOGE("ASIO: no registered driver found (requested: %ls)", g_cfg.asio_driver);
        return E_FAIL;
    }

    CLSID clsid;
    HRESULT hr = CLSIDFromString(di.clsid, &clsid);
    if (FAILED(hr)) {
        LOGE("ASIO: invalid CLSID for %ls", di.description);
        return hr;
    }

    IASIO* asio = NULL;
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid, (void**)&asio);
    if (FAILED(hr) || !asio) {
        hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void**)&asio);
    }
    if (FAILED(hr) || !asio) {
        LOGE("ASIO: CoCreateInstance failed for %ls (hr=0x%08lx)",
             di.description, (unsigned long)hr);
        return hr;
    }

    if (IASIO_CALL(asio, init, NULL) != ASIOTrue) {
        LOGE("ASIO: driver init() failed for %ls", di.description);
        IASIO_CALLO(asio, Release);
        return E_FAIL;
    }

    char name[64] = {0};
    IASIO_CALL(asio, getDriverName, name);
    g_asio.asio = asio;
    g_asio.info = di;
    g_asio.rate = 0;
    g_asio.sample_type = 0;
    g_asio.have_buffers = 0;
    g_asio.running = 0;
    ZeroMemory(&g_asio.consumer, sizeof(g_asio.consumer));
    LOGI("ASIO: opened driver \"%s\" (%ls), version %ld", name, di.description,
         IASIO_CALLO(asio, getDriverVersion));
    return S_OK;
}

static BOOL SetExactSampleRate(double rate)
{
    if (rate <= 0) return FALSE;
    if (g_asio.rate == rate) return TRUE;

    ASIOError e = IASIO_CALL(g_asio.asio, canSampleRate, rate);
    if (e != ASE_OK) {
        LOGE("ASIO: sample rate %.0f Hz not supported (canSampleRate=%ld)", rate, (long)e);
        return FALSE;
    }
    e = IASIO_CALL(g_asio.asio, setSampleRate, rate);
    if (e != ASE_OK && e != ASE_SUCCESS) {
        LOGE("ASIO: setSampleRate(%.0f) failed (%ld)", rate, (long)e);
        return FALSE;
    }
    ASIOSampleRate actual = 0;
    e = IASIO_CALL(g_asio.asio, getSampleRate, &actual);
    if (e == ASE_OK && actual > 0 && fabs(actual - rate) > 1.0) {
        LOGE("ASIO: driver switched to %.0f Hz instead of %.0f Hz", actual, rate);
        return FALSE;
    }
    g_asio.rate = rate;
    LOGI("ASIO: sample rate set to %.0f Hz", rate);
    return TRUE;
}

static long ChooseBufferSize(int requested)
{
    long lo = g_asio.min_buffer, hi = g_asio.max_buffer;
    long pref = g_asio.preferred_buffer > 0 ? g_asio.preferred_buffer : 512;
    long b = requested > 0 ? requested : pref;
    if (lo > 0 && b < lo) b = lo;
    if (hi > 0 && b > hi) b = hi;
    long gran = g_asio.granularity;
    if (gran > 0) {
        b = lo + ((b - lo) / gran) * gran;
        if (b < lo) b = lo;
    } else if (gran == -1) {
        long p = 1;
        while (p * 2 <= b) p *= 2;
        if (lo > 0 && p < lo) p = lo;
        if (hi > 0 && p > hi) p = hi;
        b = p;
    }
    if (b < 16) b = 16;
    return b;
}

static HRESULT QueryDriverShape(int channels_requested, AsioFormatInfo* out)
{
    long ins = 0, outs = 0;
    ASIOError e = IASIO_CALL(g_asio.asio, getChannels, &ins, &outs);
    if (e != ASE_OK || outs <= 0) {
        LOGE("ASIO: getChannels failed (%ld)", (long)e);
        return E_FAIL;
    }

    long use = channels_requested > 0 ? channels_requested : 2;
    if (use > outs) {
        LOGE("ASIO: requested %ld output channels, driver has %ld", use, outs);
        return E_FAIL;
    }
    if (use > AMB_MAX_ASIO_OUT) use = AMB_MAX_ASIO_OUT;

    e = IASIO_CALL(g_asio.asio, getBufferSize, &g_asio.min_buffer, &g_asio.max_buffer,
                   &g_asio.preferred_buffer, &g_asio.granularity);
    if (e != ASE_OK) {
        LOGE("ASIO: getBufferSize failed (%ld)", (long)e);
        return E_FAIL;
    }

    long type = -1;
    for (long c = 0; c < use; ++c) {
        ASIOChannelInfo ci;
        ZeroMemory(&ci, sizeof(ci));
        ci.channel = c;
        ci.isInput = ASIOFalse;
        e = IASIO_CALL(g_asio.asio, getChannelInfo, &ci);
        if (e != ASE_OK) {
            LOGE("ASIO: getChannelInfo(%ld) failed (%ld)", c, (long)e);
            return E_FAIL;
        }
        if (type < 0) {
            type = ci.type;
        } else if (ci.type != type) {
            LOGE("ASIO: mixed sample types on outputs (%ld vs %ld); bit-perfect routing refused",
                 type, ci.type);
            return E_FAIL;
        }
        g_asio.chan[c] = ci;
    }

    g_asio.channels = use;
    g_asio.sample_type = type;
    g_asio.buffer_frames = ChooseBufferSize(g_cfg.asio_buffer_frames);

    if (out) {
        out->rate = g_asio.rate;
        out->channels = use;
        out->buffer_frames = g_asio.buffer_frames;
        out->sample_type = type;
        char name[64] = {0};
        IASIO_CALL(g_asio.asio, getDriverName, name);
        strncpy_s(out->driver, sizeof(out->driver), name, _TRUNCATE);
    }
    return S_OK;
}

typedef struct ProbeArgs {
    double rate;
    int channels;
    int buffer_frames;
    AsioFormatInfo* out;
} ProbeArgs;

static HRESULT ProbeTask(void* param)
{
    ProbeArgs* a = (ProbeArgs*)param;
    HRESULT hr = EnsureAsioObject();
    if (FAILED(hr)) return hr;

    if (g_asio.have_buffers) {
        if (a->rate <= 0) a->rate = g_asio.rate;
        if (a->rate != g_asio.rate &&
            IASIO_CALL(g_asio.asio, canSampleRate, a->rate) != ASE_OK) {
            return E_FAIL;
        }
        HRESULT hr = QueryDriverShape(a->channels, a->out);
        if (SUCCEEDED(hr) && a->out) a->out->rate = a->rate;
        return hr;
    }

    if (!SetExactSampleRate(a->rate)) return E_FAIL;
    return QueryDriverShape(a->channels, a->out);
}

typedef struct OpenArgs {
    double rate;
    int channels;
    int buffer_frames;
    AsioFormatInfo* out;
} OpenArgs;

static HRESULT OpenStreamTask(void* param)
{
    OpenArgs* a = (OpenArgs*)param;
    HRESULT hr = EnsureAsioObject();
    if (FAILED(hr)) return hr;

    if (g_asio.running) {
        InterlockedExchange(&g_asio.running, 0);
        IASIO_CALLO(g_asio.asio, stop);
    }
    if (g_asio.have_buffers) {
        IASIO_CALLO(g_asio.asio, disposeBuffers);
        g_asio.have_buffers = 0;
    }
    free(g_asio.scratch);
    g_asio.scratch = NULL;
    g_asio.scratch_bytes = 0;
    ZeroMemory(&g_asio.consumer, sizeof(g_asio.consumer));

    if (!SetExactSampleRate(a->rate)) return E_FAIL;

    hr = QueryDriverShape(a->channels, NULL);
    if (FAILED(hr)) return hr;
    if (a->buffer_frames > 0) {
        g_asio.buffer_frames = ChooseBufferSize(a->buffer_frames);
    }

    long use = g_asio.channels;
    ZeroMemory(g_asio.bufs, sizeof(g_asio.bufs));
    for (long c = 0; c < use; ++c) {
        g_asio.bufs[c].isInput = ASIOFalse;
        g_asio.bufs[c].channelNum = c;
        g_asio.bufs[c].buffers[0] = NULL;
        g_asio.bufs[c].buffers[1] = NULL;
    }

    g_asio.callbacks.bufferSwitch = Asio_BufferSwitch;
    g_asio.callbacks.sampleRateDidChange = Asio_SampleRateChanged;
    g_asio.callbacks.asioMessage = Asio_Message;
    g_asio.callbacks.bufferSwitchTimeInfo = Asio_BufferSwitchTimeInfo;

    ASIOError e = IASIO_CALL(g_asio.asio, createBuffers, g_asio.bufs, use,
                             g_asio.buffer_frames, &g_asio.callbacks);
    if (e != ASE_OK) {
        LOGE("ASIO: createBuffers(%ld frames, %ld ch) failed (%ld)",
             g_asio.buffer_frames, use, (long)e);
        return E_FAIL;
    }

    ASIOSampleRate actual = 0;
    IASIO_CALL(g_asio.asio, getSampleRate, &actual);
    LOGI("ASIO: stream ready: %.0f Hz, %ld ch, %ld frames (%.2f ms), %s",
         actual > 0 ? actual : g_asio.rate, use, g_asio.buffer_frames,
         (double)g_asio.buffer_frames * 1000.0 / (actual > 0 ? actual : g_asio.rate),
         Asio_SampleTypeName(g_asio.sample_type));

    g_asio.have_buffers = 1;
    g_asio.running = 0;
    g_asio.rate_changed = 0;
    g_asio.underruns = 0;
    g_asio.frames_played = 0;

    if (a->out) {
        a->out->rate = actual > 0 ? actual : g_asio.rate;
        a->out->channels = use;
        a->out->buffer_frames = g_asio.buffer_frames;
        a->out->sample_type = g_asio.sample_type;
        char name[64] = {0};
        IASIO_CALL(g_asio.asio, getDriverName, name);
        strncpy_s(a->out->driver, sizeof(a->out->driver), name, _TRUNCATE);
    }
    return S_OK;
}

static HRESULT StartTask(void* param)
{
    AMB_UNUSED(param);
    if (!g_asio.asio || !g_asio.have_buffers) return E_FAIL;
    if (g_asio.running) return S_OK;
    if (!g_asio.consumer.ring || !g_asio.consumer.ring->data) {
        LOGE("ASIO: start requested before consumer was attached");
        return E_FAIL;
    }
    g_asio.rate_changed = 0;
    InterlockedExchange(&g_asio.running, 1);
    ASIOError e = IASIO_CALLO(g_asio.asio, start);
    if (e != ASE_OK) {
        InterlockedExchange(&g_asio.running, 0);
        LOGE("ASIO: start() failed (%ld)", (long)e);
        return E_FAIL;
    }
    LOGI("ASIO: streaming");
    return S_OK;
}

static HRESULT StopTask(void* param)
{
    AMB_UNUSED(param);
    if (!g_asio.asio) return S_OK;
    InterlockedExchange(&g_asio.running, 0);
    if (g_asio.have_buffers) {
        IASIO_CALLO(g_asio.asio, stop);
    }
    return S_OK;
}

static HRESULT SetConsumerTask(void* param)
{
    const AsioConsumer* c = (const AsioConsumer*)param;
    if (!g_asio.have_buffers) return E_FAIL;
    g_asio.consumer = *c;
    size_t need = (size_t)g_asio.buffer_frames * c->frame_bytes;
    if (!g_asio.scratch || g_asio.scratch_bytes < need) {
        uint8_t* p = (uint8_t*)realloc(g_asio.scratch, need);
        if (!p) return E_OUTOFMEMORY;
        g_asio.scratch = p;
        g_asio.scratch_bytes = need;
    }
    return S_OK;
}

static HRESULT CloseStreamTask(void* param)
{
    AMB_UNUSED(param);
    if (!g_asio.asio) return S_OK;
    InterlockedExchange(&g_asio.running, 0);
    if (g_asio.have_buffers) {
        IASIO_CALLO(g_asio.asio, stop);
        IASIO_CALLO(g_asio.asio, disposeBuffers);
        g_asio.have_buffers = 0;
    }
    ZeroMemory(&g_asio.consumer, sizeof(g_asio.consumer));
    free(g_asio.scratch);
    g_asio.scratch = NULL;
    g_asio.scratch_bytes = 0;
    return S_OK;
}

static HRESULT CloseAllTask(void* param)
{
    AMB_UNUSED(param);
    CloseStreamTask(NULL);
    if (g_asio.asio) {
        LOGD("ASIO: closing driver \"%ls\"", g_asio.info.description);
        IASIO_CALLO(g_asio.asio, Release);
        g_asio.asio = NULL;
    }
    g_asio.rate = 0;
    return S_OK;
}

static HRESULT CloseIfIdleTask(void* param)
{
    AMB_UNUSED(param);
    if (g_asio.have_buffers || g_asio.running) return S_OK;
    return CloseAllTask(NULL);
}

static DWORD WINAPI AsioCtlThread(void* param)
{
    AMB_UNUSED(param);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    g_asio.callbacks.bufferSwitch = Asio_BufferSwitch;
    g_asio.callbacks.sampleRateDidChange = Asio_SampleRateChanged;
    g_asio.callbacks.asioMessage = Asio_Message;
    g_asio.callbacks.bufferSwitchTimeInfo = Asio_BufferSwitchTimeInfo;

    SetEvent(g_ctl_ready_event);

    for (;;) {
        WaitForSingleObject(g_ctl_cmd_event, INFINITE);
        if (g_ctl_quit) break;
        HRESULT hr = g_ctl_fn ? g_ctl_fn(g_ctl_arg) : E_FAIL;
        g_ctl_result = hr;
        SetEvent(g_ctl_done_event);
    }

    CoUninitialize();
    return 0;
}

static BOOL CALLBACK InitControlOnce(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    AMB_UNUSED(once);
    AMB_UNUSED(param);
    AMB_UNUSED(ctx);

    InitializeCriticalSection(&g_ctl_call_cs);
    g_ctl_cmd_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ctl_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ctl_ready_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_ctl_cmd_event || !g_ctl_done_event || !g_ctl_ready_event) return FALSE;

    g_ctl_thread = CreateThread(NULL, 0, AsioCtlThread, NULL, 0, NULL);
    if (!g_ctl_thread) return FALSE;

    WaitForSingleObject(g_ctl_ready_event, 5000);
    return TRUE;
}

static HRESULT CtlCall(HRESULT (*fn)(void*), void* arg)
{
    if (!InitOnceExecuteOnce(&g_ctl_once, InitControlOnce, NULL, NULL)) return E_FAIL;
    if (!g_ctl_thread) return E_FAIL;

    EnterCriticalSection(&g_ctl_call_cs);
    g_ctl_fn = fn;
    g_ctl_arg = arg;
    ResetEvent(g_ctl_done_event);
    SetEvent(g_ctl_cmd_event);
    WaitForSingleObject(g_ctl_done_event, INFINITE);
    HRESULT hr = g_ctl_result;
    g_ctl_fn = NULL;
    g_ctl_arg = NULL;
    LeaveCriticalSection(&g_ctl_call_cs);
    return hr;
}

HRESULT AsioHost_Probe(double rate, int channels, int buffer_frames, AsioFormatInfo* out)
{
    ProbeArgs args;
    ZeroMemory(&args, sizeof(args));
    args.rate = rate;
    args.channels = channels;
    args.buffer_frames = buffer_frames;
    args.out = out;
    if (out) ZeroMemory(out, sizeof(*out));
    HRESULT hr = CtlCall(ProbeTask, &args);
    if (FAILED(hr)) LOGE("ASIO: probe failed (hr=0x%08lx)", (unsigned long)hr);
    return hr;
}

HRESULT AsioHost_OpenStream(double rate, int channels, int buffer_frames, AsioFormatInfo* out)
{
    OpenArgs args;
    ZeroMemory(&args, sizeof(args));
    args.rate = rate;
    args.channels = channels;
    args.buffer_frames = buffer_frames;
    args.out = out;
    if (out) ZeroMemory(out, sizeof(*out));
    HRESULT hr = CtlCall(OpenStreamTask, &args);
    if (FAILED(hr)) LOGE("ASIO: open stream failed (hr=0x%08lx)", (unsigned long)hr);
    return hr;
}

void AsioHost_SetConsumer(const AsioConsumer* consumer)
{
    if (!consumer) return;
    CtlCall(SetConsumerTask, (void*)consumer);
}

HRESULT AsioHost_Start(void)
{
    return CtlCall(StartTask, NULL);
}

void AsioHost_Stop(void)
{
    CtlCall(StopTask, NULL);
}

void AsioHost_CloseStream(void)
{
    CtlCall(CloseStreamTask, NULL);
}

void AsioHost_Close(void)
{
    CtlCall(CloseAllTask, NULL);
}

void AsioHost_CloseIfIdle(void)
{
    CtlCall(CloseIfIdleTask, NULL);
}

void AsioHost_GetStats(LONG64* frames, LONG* underruns)
{
    if (frames) *frames = g_asio.frames_played;
    if (underruns) *underruns = g_asio.underruns;
}

static void Asio_BufferSwitch(long index, ASIOBool direct)
{
    AMB_UNUSED(direct);
    if (index < 0 || index > 1) return;

    AsioConsumer* c = &g_asio.consumer;
    long frames = g_asio.buffer_frames;
    long channels = g_asio.channels;

    if (!g_asio.running || !g_asio.have_buffers || !c->ring || !c->ring->data ||
        !g_asio.scratch || channels <= 0) {
        for (long ch = 0; ch < channels; ++ch) {
            if (g_asio.bufs[ch].buffers[index]) {
                memset(g_asio.bufs[ch].buffers[index], 0,
                       (size_t)frames * (size_t)(g_asio.sample_type == ASIOSTInt24LSB ? 3 :
                                                  g_asio.sample_type == ASIOSTInt16LSB ? 2 :
                                                  g_asio.sample_type == ASIOSTInt24MSB ? 3 :
                                                  g_asio.sample_type == ASIOSTInt16MSB ? 2 : 4));
            }
        }
        return;
    }

    uint32_t frame_bytes = c->frame_bytes;
    uint32_t app_channels = c->channels;
    if (frame_bytes == 0 || app_channels == 0) return;
    uint32_t bytes_per_sample = frame_bytes / app_channels;
    if (bytes_per_sample == 0) return;

    uint32_t fill = Ring_Fill(c->ring);
    uint32_t want = (uint32_t)frames;
    uint32_t take = fill < want ? fill : want;
    uint32_t got = Ring_Read(c->ring, g_asio.scratch, take);
    if (got < want) {
        memset(g_asio.scratch + (size_t)got * frame_bytes, 0,
               (size_t)(want - got) * frame_bytes);
        if (fill < want) InterlockedIncrement(&g_asio.underruns);
    }

    BOOL mute = c->mute ? *c->mute : FALSE;
    float gain = c->gain ? *c->gain : 1.0f;
    BOOL apply_gain = c->volume_enabled && c->is_float && (mute || fabsf(gain - 1.0f) > 1e-7f);

    long copy_channels = channels < (long)app_channels ? channels : (long)app_channels;
    for (long ch = 0; ch < copy_channels; ++ch) {
        uint8_t* dst = (uint8_t*)g_asio.bufs[ch].buffers[index];
        if (!dst) continue;
        const uint8_t* src = g_asio.scratch + (size_t)ch * bytes_per_sample;
        if (bytes_per_sample == 4) {
            uint32_t* d = (uint32_t*)dst;
            for (long i = 0; i < frames; ++i) {
                memcpy(&d[i], src + (size_t)i * frame_bytes, 4);
            }
        } else if (bytes_per_sample == 3) {
            for (long i = 0; i < frames; ++i) {
                memcpy(dst + (size_t)i * 3, src + (size_t)i * frame_bytes, 3);
            }
        } else {
            uint16_t* d = (uint16_t*)dst;
            for (long i = 0; i < frames; ++i) {
                memcpy(&d[i], src + (size_t)i * frame_bytes, 2);
            }
        }
        if (apply_gain) {
            float* d = (float*)dst;
            for (long i = 0; i < frames; ++i) {
                d[i] = mute ? 0.0f : d[i] * gain;
            }
        }
    }
    for (long ch = copy_channels; ch < channels; ++ch) {
        if (g_asio.bufs[ch].buffers[index]) {
            memset(g_asio.bufs[ch].buffers[index], 0,
                   (size_t)frames * (size_t)bytes_per_sample);
        }
    }

    if (c->frames_played) {
        InterlockedExchangeAdd64(c->frames_played, (LONG64)got);
    }
    if (c->event && *c->event) {
        SetEvent(*c->event);
    }
}

static void Asio_SampleRateChanged(ASIOSampleRate rate)
{
    g_asio.rate_changed = 1;
    LOGW("ASIO: driver changed sample rate to %.0f Hz; stream can no longer be bit-perfect", rate);
}

static long Asio_Message(long selector, long value, void* message, double* opt)
{
    AMB_UNUSED(message);
    AMB_UNUSED(opt);
    switch (selector) {
    case kAsioSelectorSupported:
        switch (value) {
        case kAsioEngineVersion:
        case kAsioResetRequest:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
            return 1;
        default:
            return 0;
        }
    case kAsioEngineVersion:
        return 2;
    case kAsioSupportsTimeInfo:
        return 0;
    case kAsioResetRequest:
        return 1;
    default:
        return 0;
    }
}

static ASIOTime* Asio_BufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
{
    Asio_BufferSwitch(index, direct);
    return params;
}

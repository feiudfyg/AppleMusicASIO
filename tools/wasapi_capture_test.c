#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

static const GUID LOCAL_SUB_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID LOCAL_SUB_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static const IID LOCAL_IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

static void MakeFloat(WAVEFORMATEXTENSIBLE* w, UINT32 rate)
{
    ZeroMemory(w, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = 2;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = 32;
    w->Format.nBlockAlign = 8;
    w->Format.nAvgBytesPerSec = rate * 8;
    w->Format.cbSize = 22;
    w->Samples.wValidBitsPerSample = 32;
    w->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    w->SubFormat = LOCAL_SUB_FLOAT;
}

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = NULL;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IMMDeviceEnumerator, (void**)&en);
    IMMDeviceCollection* coll = NULL;
    en->lpVtbl->EnumAudioEndpoints(en, eCapture, DEVICE_STATE_ACTIVE, &coll);
    UINT count = 0;
    coll->lpVtbl->GetCount(coll, &count);
    IMMDevice* target = NULL;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* d = NULL;
        if (FAILED(coll->lpVtbl->Item(coll, i, &d)) || !d) continue;
        IPropertyStore* st = NULL;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        WCHAR name[256] = L"";
        if (SUCCEEDED(d->lpVtbl->OpenPropertyStore(d, STGM_READ, &st))) {
            if (SUCCEEDED(st->lpVtbl->GetValue(st, &PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR) {
                _snwprintf_s(name, 256, _TRUNCATE, L"%ls", pv.pwszVal);
            }
            st->lpVtbl->Release(st);
        }
        PropVariantClear(&pv);
        LPWSTR id = NULL;
        d->lpVtbl->GetId(d, &id);
        printf("capture: %ls  %ls\n", name, id ? id : L"");
        if (wcsstr(name, L"LOOPBACK")) target = d;
        else d->lpVtbl->Release(d);
        if (id) CoTaskMemFree(id);
        if (target) break;
    }
    if (!target) { printf("no loopback capture endpoint\n"); return 1; }

    IAudioClient* c = NULL;
    HRESULT hr = target->lpVtbl->Activate(target, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&c);
    if (FAILED(hr)) { printf("activate 0x%08lX\n", (unsigned long)hr); return 1; }
    WAVEFORMATEX* format = NULL;
    c->lpVtbl->GetMixFormat(c, &format);
    if (!format) { printf("no mix format\n"); return 1; }
    printf("capture format: %lu Hz %u ch %u bits\n", (unsigned long)format->nSamplesPerSec,
           format->nChannels, format->wBitsPerSample);
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    REFERENCE_TIME hns = 1000000;
    hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               hns, 0, format, NULL);
    printf("initialize 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    c->lpVtbl->SetEventHandle(c, ev);
    IAudioCaptureClient* cap = NULL;
    hr = c->lpVtbl->GetService(c, &LOCAL_IID_IAudioCaptureClient, (void**)&cap);
    printf("capture service 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    UINT32 frames = 0;
    c->lpVtbl->GetBufferSize(c, &frames);
    printf("buffer %lu frames\n", (unsigned long)frames);
    c->lpVtbl->Start(c);

    double sum = 0.0;
    ULONGLONG samples = 0;
    float peak = 0.0f;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    while (1) {
        QueryPerformanceCounter(&t);
        double elapsed = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (elapsed >= 4.0) break;
        WaitForSingleObject(ev, 100);
        UINT32 packet = 0;
        while (SUCCEEDED(cap->lpVtbl->GetNextPacketSize(cap, &packet)) && packet > 0) {
            BYTE* data = NULL;
            UINT32 n = 0;
            DWORD flags = 0;
            if (FAILED(cap->lpVtbl->GetBuffer(cap, &data, &n, &flags, NULL, NULL))) break;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                if (format->wBitsPerSample == 32 && format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    const WAVEFORMATEXTENSIBLE* w = (const WAVEFORMATEXTENSIBLE*)format;
                    BOOL flt = IsEqualGUID(&w->SubFormat, &LOCAL_SUB_FLOAT);
                    size_t total = (size_t)n * format->nChannels;
                    if (flt) {
                        const float* s = (const float*)data;
                        for (size_t i = 0; i < total; ++i) {
                            double v = s[i];
                            sum += v * v;
                            float a = (float)fabs(v);
                            if (a > peak) peak = a;
                        }
                    } else {
                        const int32_t* s = (const int32_t*)data;
                        for (size_t i = 0; i < total; ++i) {
                            double v = (double)s[i] / 2147483648.0;
                            sum += v * v;
                            float a = (float)fabs(v);
                            if (a > peak) peak = a;
                        }
                    }
                    samples += total;
                }
            }
            cap->lpVtbl->ReleaseBuffer(cap, n);
        }
    }
    c->lpVtbl->Stop(c);
    printf("samples=%llu peak=%.6f rms=%.6f\n", (unsigned long long)samples, peak,
           samples ? sqrt(sum / (double)samples) : 0.0);
    cap->lpVtbl->Release(cap);
    c->lpVtbl->Release(c);
    target->lpVtbl->Release(target);
    coll->lpVtbl->Release(coll);
    en->lpVtbl->Release(en);
    CoUninitialize();
    return 0;
}


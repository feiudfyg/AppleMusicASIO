#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <stdio.h>

static const GUID SUB_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static void MakePcm(WAVEFORMATEXTENSIBLE* w, UINT32 rate, UINT32 bits, UINT32 valid)
{
    ZeroMemory(w, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = 2;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = (WORD)bits;
    w->Format.nBlockAlign = (WORD)(2 * bits / 8);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    w->Format.cbSize = 22;
    w->Samples.wValidBitsPerSample = (WORD)valid;
    w->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    w->SubFormat = SUB_PCM;
}

static void Hr(const char* tag, HRESULT hr)
{
    printf("  %-36s 0x%08lX\n", tag, (unsigned long)hr);
}

static HRESULT TryInit(IMMDevice* dev, const WAVEFORMATEX* wf, DWORD flags,
                       REFERENCE_TIME hns, REFERENCE_TIME period)
{
    IAudioClient* c = NULL;
    HRESULT hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&c);
    if (FAILED(hr)) return hr;

    WAVEFORMATEX* closest = NULL;
    hr = c->lpVtbl->IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE, wf, &closest);
    if (closest) CoTaskMemFree(closest);
    if (hr != S_OK) { c->lpVtbl->Release(c); return hr; }

    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_EXCLUSIVE, flags, hns, period, wf, NULL);
    if (SUCCEEDED(hr)) {
        UINT32 frames = 0;
        HRESULT h2 = c->lpVtbl->GetBufferSize(c, &frames);
        printf("      OK: frames=%lu GetBufferSize=0x%08lX", (unsigned long)frames, (unsigned long)h2);
        if (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
            printf(" SetEventHandle=0x%08lX", (unsigned long)c->lpVtbl->SetEventHandle(c, ev));
        }
        printf("\n");
        c->lpVtbl->Stop(c);
        c->lpVtbl->Reset(c);
    }
    if (ev) CloseHandle(ev);
    c->lpVtbl->Release(c);
    return hr;
}

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = NULL;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IMMDeviceEnumerator, (void**)&en);
    IMMDevice* dev = NULL;
    HRESULT hr = en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("default endpoint 0x%08lX\n", (unsigned long)hr); return 1; }

    IAudioClient* c = NULL;
    dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&c);
    REFERENCE_TIME def = 0, minp = 0;
    c->lpVtbl->GetDevicePeriod(c, &def, &minp);
    printf("device period: default=%lld min=%lld\n", (long long)def, (long long)minp);
    c->lpVtbl->Release(c);

    static const UINT32 rates[] = { 44100, 48000, 96000 };
    static const REFERENCE_TIME durations[] = { 0, 30000, 90000, 100000, 200000, 210000, 300000, 400000, 1000000 };
    for (int r = 0; r < 3; ++r) {
        WAVEFORMATEXTENSIBLE w;
        MakePcm(&w, rates[r], 32, 24);
        printf("\nrate %lu, pcm 32/24\n", (unsigned long)rates[r]);
        for (int d = 0; d < (int)(sizeof(durations) / sizeof(durations[0])); ++d) {
            printf(" hns=%lld\n", (long long)durations[d]);
            Hr("event flags, periodicity 0",
               TryInit(dev, &w.Format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, durations[d], 0));
            Hr("poll flags, periodicity 0",
               TryInit(dev, &w.Format, 0, durations[d], 0));
            Hr("event flags, periodicity=hns",
               TryInit(dev, &w.Format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, durations[d], durations[d]));
        }
    }

    WAVEFORMATEXTENSIBLE w;
    MakePcm(&w, 96000, 16, 16);
    printf("\nrate 96000, pcm16\n");
    Hr("event 0", TryInit(dev, &w.Format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0));
    Hr("event 100000", TryInit(dev, &w.Format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0));
    Hr("event 200000", TryInit(dev, &w.Format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0));

    dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
    CoUninitialize();
    return 0;
}

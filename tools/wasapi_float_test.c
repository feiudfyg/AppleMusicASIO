#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

static const GUID SUB_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID SUB_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static void Print(const char* what, HRESULT hr)
{
    printf("  %-48s 0x%08lX\n", what, (unsigned long)hr);
}

static HRESULT Check(IMMDevice* dev, const WAVEFORMATEX* wf, int init)
{
    IAudioClient* c = NULL;
    HRESULT hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&c);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* closest = NULL;
    hr = c->lpVtbl->IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE, wf, &closest);
    if (closest) CoTaskMemFree(closest);
    if (hr == S_OK && init) {
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        REFERENCE_TIME hns = 200000;
        hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   hns, hns, wf, NULL);
        if (SUCCEEDED(hr)) {
            UINT32 frames = 0;
            c->lpVtbl->GetBufferSize(c, &frames);
            printf("      init OK, %lu frames; SetEventHandle=0x%08lX\n",
                   (unsigned long)frames, (unsigned long)c->lpVtbl->SetEventHandle(c, ev));
            c->lpVtbl->Reset(c);
        }
        CloseHandle(ev);
    }
    c->lpVtbl->Release(c);
    return hr;
}

static void MakeExt(WAVEFORMATEXTENSIBLE* w, UINT32 rate, UINT32 bits, UINT32 valid,
                    BOOL is_float, DWORD mask)
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
    w->dwChannelMask = mask;
    w->SubFormat = is_float ? SUB_FLOAT : SUB_PCM;
}

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = NULL;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IMMDeviceEnumerator, (void**)&en);
    IMMDevice* dev = NULL;
    en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);

    static const UINT32 rates[] = { 44100, 48000, 96000, 192000 };
    for (int r = 0; r < 4; ++r) {
        printf("rate %lu\n", (unsigned long)rates[r]);
        WAVEFORMATEX plain;
        ZeroMemory(&plain, sizeof(plain));
        plain.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        plain.nChannels = 2;
        plain.nSamplesPerSec = rates[r];
        plain.wBitsPerSample = 32;
        plain.nBlockAlign = 8;
        plain.nAvgBytesPerSec = rates[r] * 8;
        printf(" plain IEEE_FLOAT 32\n");
        Print("IsFormatSupported", Check(dev, &plain, 1));

        WAVEFORMATEXTENSIBLE w;
        MakeExt(&w, rates[r], 32, 32, TRUE, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        printf(" extensible float 32/32 mask=3\n");
        Print("IsFormatSupported", Check(dev, &w.Format, 1));
        MakeExt(&w, rates[r], 32, 32, TRUE, 0);
        printf(" extensible float 32/32 mask=0\n");
        Print("IsFormatSupported", Check(dev, &w.Format, 1));
        MakeExt(&w, rates[r], 32, 32, TRUE, 0x3);
        printf(" extensible float 32/32 mask=STEREO\n");
        Print("IsFormatSupported", Check(dev, &w.Format, 1));
        MakeExt(&w, rates[r], 32, 24, FALSE, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        printf(" extensible pcm 32/24 mask=3\n");
        Print("IsFormatSupported", Check(dev, &w.Format, 1));
        MakeExt(&w, rates[r], 32, 24, FALSE, 0);
        printf(" extensible pcm 32/24 mask=0\n");
        Print("IsFormatSupported", Check(dev, &w.Format, 1));
        MakeExt(&w, rates[r], 24, 24, FALSE, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        printf(" extensible pcm 24/24 mask=3\n");
        Print("IsFormatSupported", Check(dev, &w.Format, 1));
    }

    dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
    CoUninitialize();
    return 0;
}

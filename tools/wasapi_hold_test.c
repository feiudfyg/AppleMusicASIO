#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

static const GUID SUB_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static void Make(WAVEFORMATEXTENSIBLE* w, UINT32 rate)
{
    ZeroMemory(w, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = 2;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = 32;
    w->Format.nBlockAlign = 8;
    w->Format.nAvgBytesPerSec = rate * 8;
    w->Format.cbSize = 22;
    w->Samples.wValidBitsPerSample = 24;
    w->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    w->SubFormat = SUB_PCM;
}

static HRESULT Open(IMMDevice* dev, const WAVEFORMATEX* wf, IAudioClient** out)
{
    IAudioClient* c = NULL;
    HRESULT hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&c);
    if (FAILED(hr)) return hr;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = c->lpVtbl->IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE, wf, NULL);
    if (SUCCEEDED(hr)) {
        hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 200000, wf, NULL);
        if (SUCCEEDED(hr)) {
            c->lpVtbl->SetEventHandle(c, ev);
            c->lpVtbl->Start(c);
        }
    }
    CloseHandle(ev);
    if (FAILED(hr)) { c->lpVtbl->Release(c); return hr; }
    *out = c;
    return S_OK;
}

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = NULL;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IMMDeviceEnumerator, (void**)&en);
    IMMDevice* dev = NULL;
    en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);

    WAVEFORMATEXTENSIBLE w;
    Make(&w, 96000);

    IAudioClient* a = NULL;
    HRESULT hr = Open(dev, &w.Format, &a);
    printf("A open: 0x%08lX\n", (unsigned long)hr);
    if (a) { a->lpVtbl->Stop(a); a->lpVtbl->Reset(a); printf("A stopped+reset (still held)\n"); }

    IAudioClient* b = NULL;
    hr = Open(dev, &w.Format, &b);
    printf("B open while A held: 0x%08lX\n", (unsigned long)hr);
    if (b) { b->lpVtbl->Stop(b); b->lpVtbl->Reset(b); b->lpVtbl->Release(b); b = NULL; }

    if (a) { a->lpVtbl->Release(a); a = NULL; printf("A released\n"); }
    IAudioClient* c = NULL;
    hr = Open(dev, &w.Format, &c);
    printf("C open after A release: 0x%08lX\n", (unsigned long)hr);
    if (c) { c->lpVtbl->Stop(c); c->lpVtbl->Reset(c); c->lpVtbl->Release(c); }

    dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
    CoUninitialize();
    return 0;
}

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <ksmedia.h>
#include <stdio.h>
#include <wchar.h>

static const GUID SUB_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID SUB_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static const char* HrName(HRESULT hr)
{
    switch (hr) {
    case S_OK: return "S_OK";
    case S_FALSE: return "S_FALSE";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
    case AUDCLNT_E_DEVICE_IN_USE: return "AUDCLNT_E_DEVICE_IN_USE";
    case AUDCLNT_E_ALREADY_INITIALIZED: return "AUDCLNT_E_ALREADY_INITIALIZED";
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_POINTER: return "E_POINTER";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    default: return "?";
    }
}

static void PrintHr(const char* what, HRESULT hr)
{
    printf("    %-42s 0x%08lX %s\n", what, (unsigned long)hr, HrName(hr));
}

static void MakeFormat(WAVEFORMATEXTENSIBLE* w, UINT32 rate, UINT32 channels,
                       UINT32 bits, UINT32 valid, BOOL is_float, DWORD mask)
{
    ZeroMemory(w, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = (WORD)channels;
    w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = (WORD)bits;
    w->Format.nBlockAlign = (WORD)(channels * bits / 8);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    w->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = (WORD)valid;
    w->dwChannelMask = mask;
    w->SubFormat = is_float ? SUB_FLOAT : SUB_PCM;
}

static const char* FormatName(const WAVEFORMATEX* wf, char* buf, size_t cb)
{
    const WAVEFORMATEXTENSIBLE* w = (const WAVEFORMATEXTENSIBLE*)wf;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const char* kind = "?";
        if (IsEqualGUID(&w->SubFormat, &SUB_FLOAT)) kind = "float";
        else if (IsEqualGUID(&w->SubFormat, &SUB_PCM)) kind = "pcm";
        _snprintf_s(buf, cb, _TRUNCATE, "%lu Hz %u ch %s %u/%u mask=0x%lx cb=%u",
                    (unsigned long)wf->nSamplesPerSec, wf->nChannels, kind,
                    wf->wBitsPerSample, w->Samples.wValidBitsPerSample,
                    (unsigned long)w->dwChannelMask, wf->cbSize);
    } else {
        _snprintf_s(buf, cb, _TRUNCATE, "%lu Hz %u ch tag=%u %u bit cb=%u",
                    (unsigned long)wf->nSamplesPerSec, wf->nChannels,
                    wf->wFormatTag, wf->wBitsPerSample, wf->cbSize);
    }
    return buf;
}

static void DumpProps(IMMDevice* dev)
{
    IPropertyStore* store = NULL;
    HRESULT hr = dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &store);
    if (FAILED(hr)) { PrintHr("OpenPropertyStore", hr); return; }

    DWORD count = 0;
    hr = store->lpVtbl->GetCount(store, &count);
    if (SUCCEEDED(hr)) {
        printf("    property count: %lu\n", (unsigned long)count);
        for (DWORD i = 0; i < count; ++i) {
            PROPERTYKEY key;
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->lpVtbl->GetAt(store, i, &key)) &&
                SUCCEEDED(store->lpVtbl->GetValue(store, &key, &pv))) {
                WCHAR v[512] = L"";
                if (pv.vt == VT_LPWSTR) _snwprintf_s(v, 512, _TRUNCATE, L"%ls", pv.pwszVal);
                else if (pv.vt == VT_UI4) _snwprintf_s(v, 512, _TRUNCATE, L"%lu", pv.ulVal);
                else if (pv.vt == VT_CLSID) _snwprintf_s(v, 512, _TRUNCATE, L"%08lx-%04x-%04x",
                                                        pv.puuid->Data1, pv.puuid->Data2, pv.puuid->Data3);
                else if (pv.vt == VT_BLOB) _snwprintf_s(v, 512, _TRUNCATE, L"blob %lu bytes", pv.blob.cbSize);
                else _snwprintf_s(v, 512, _TRUNCATE, L"vt=%u", pv.vt);
                printf("    {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x},%lu = %ls\n",
                       key.fmtid.Data1, key.fmtid.Data2, key.fmtid.Data3,
                       key.fmtid.Data4[0], key.fmtid.Data4[1], key.fmtid.Data4[2], key.fmtid.Data4[3],
                       key.fmtid.Data4[4], key.fmtid.Data4[5], key.fmtid.Data4[6], key.fmtid.Data4[7],
                       (unsigned long)key.pid, v);
            }
            PropVariantClear(&pv);
        }
    }
    store->lpVtbl->Release(store);
}

static HRESULT TryExclusive(IMMDevice* dev, const WAVEFORMATEX* wf, int event_mode,
                            REFERENCE_TIME hns)
{
    IAudioClient* client = NULL;
    HRESULT hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL,
                                       (void**)&client);
    if (FAILED(hr)) { PrintHr("Activate", hr); return hr; }

    WAVEFORMATEX* closest = NULL;
    hr = client->lpVtbl->IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, wf, &closest);
    PrintHr("IsFormatSupported(exclusive)", hr);
    if (closest) {
        char b[256];
        printf("    closest: %s\n", FormatName(closest, b, sizeof(b)));
        CoTaskMemFree(closest);
    }
    if (hr != S_OK) {
        client->lpVtbl->Release(client);
        return hr;
    }

    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    DWORD flags = event_mode ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
    hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE, flags, hns, 0, wf, NULL);
    PrintHr(event_mode ? "Initialize(exclusive,event,20ms)" : "Initialize(exclusive,poll,20ms)", hr);

    if (SUCCEEDED(hr)) {
        UINT32 frames = 0;
        HRESULT h2 = client->lpVtbl->GetBufferSize(client, &frames);
        PrintHr("GetBufferSize", h2);
        printf("    buffer frames: %lu\n", (unsigned long)frames);
        if (event_mode) PrintHr("SetEventHandle", client->lpVtbl->SetEventHandle(client, ev));
        IAudioRenderClient* render = NULL;
        h2 = client->lpVtbl->GetService(client, &IID_IAudioRenderClient, (void**)&render);
        PrintHr("GetService(IAudioRenderClient)", h2);
        if (render) {
            BYTE* p = NULL;
            h2 = render->lpVtbl->GetBuffer(render, frames, &p);
            if (SUCCEEDED(h2)) {
                memset(p, 0, (size_t)frames * wf->nBlockAlign);
                render->lpVtbl->ReleaseBuffer(render, frames, 0);
            }
            render->lpVtbl->Release(render);
        }
        h2 = client->lpVtbl->Start(client);
        PrintHr("Start", h2);
        if (SUCCEEDED(h2)) Sleep(100);
        client->lpVtbl->Stop(client);
        client->lpVtbl->Reset(client);
    }
    if (ev) CloseHandle(ev);
    client->lpVtbl->Release(client);
    return hr;
}

static void TestDevice(IMMDevice* dev, const wchar_t* label)
{
    LPWSTR id = NULL;
    IPropertyStore* store = NULL;
    PROPVARIANT name;
    PropVariantInit(&name);

    printf("\n=== %ls ===\n", label);
    if (SUCCEEDED(dev->lpVtbl->GetId(dev, &id)) && id) {
        printf("id: %ls\n", id);
        CoTaskMemFree(id);
    }
    if (SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &store))) {
        if (SUCCEEDED(store->lpVtbl->GetValue(store, &PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR) {
            printf("name: %ls\n", name.pwszVal);
        }
        store->lpVtbl->Release(store);
    }
    PropVariantClear(&name);

    IAudioClient* client = NULL;
    HRESULT hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL,
                                       (void**)&client);
    if (FAILED(hr)) { PrintHr("Activate", hr); return; }

    WAVEFORMATEX* mix = NULL;
    hr = client->lpVtbl->GetMixFormat(client, &mix);
    if (SUCCEEDED(hr) && mix) {
        char b[256];
        printf("mix: %s\n", FormatName(mix, b, sizeof(b)));
        REFERENCE_TIME def = 0, minp = 0;
        hr = client->lpVtbl->GetDevicePeriod(client, &def, &minp);
        printf("period: default=%lld min=%lld hr=0x%08lx\n",
               (long long)def, (long long)minp, (unsigned long)hr);
        CoTaskMemFree(mix);
    } else {
        PrintHr("GetMixFormat", hr);
    }
    client->lpVtbl->Release(client);

    DumpProps(dev);

    static const UINT32 rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    static const UINT32 masks[] = { SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0 };
    for (int m = 0; m < 2; ++m) {
        for (int r = 0; r < (int)(sizeof(rates) / sizeof(rates[0])); ++r) {
            for (int kind = 0; kind < 5; ++kind) {
                WAVEFORMATEXTENSIBLE w;
                UINT32 bits, valid;
                BOOL is_float = FALSE;
                switch (kind) {
                case 0: bits = 32; valid = 32; is_float = TRUE; break;
                case 1: bits = 32; valid = 32; break;
                case 2: bits = 32; valid = 24; break;
                case 3: bits = 24; valid = 24; break;
                default: bits = 16; valid = 16; break;
                }
                MakeFormat(&w, rates[r], 2, bits, valid, is_float, masks[m]);
                char b[256];
                printf("  candidate %s\n", FormatName(&w.Format, b, sizeof(b)));
                HRESULT h = TryExclusive(dev, &w.Format, 1, 200000);
                if (h == S_OK) {
                    printf("  *** exclusive OK ***\n");
                }
            }
        }
    }
}

int wmain(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator* en = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) { PrintHr("CoCreateInstance(MMDeviceEnumerator)", hr); return 1; }

    IMMDevice* def = NULL;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &def)) && def) {
        TestDevice(def, L"default eConsole render");
        def->lpVtbl->Release(def);
    }

    IMMDeviceCollection* coll = NULL;
    hr = en->lpVtbl->EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &coll);
    if (SUCCEEDED(hr) && coll) {
        UINT count = 0;
        coll->lpVtbl->GetCount(coll, &count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = NULL;
            if (SUCCEEDED(coll->lpVtbl->Item(coll, i, &d)) && d) {
                wchar_t label[64];
                _snwprintf_s(label, 64, _TRUNCATE, L"render endpoint %u", i);
                TestDevice(d, label);
                d->lpVtbl->Release(d);
            }
        }
        coll->lpVtbl->Release(coll);
    }

    en->lpVtbl->Release(en);
    CoUninitialize();
    return 0;
}

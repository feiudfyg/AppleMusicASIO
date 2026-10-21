#include "common.h"
#include "format.h"

#include <math.h>

static void PrintFormat(const char* tag, const WAVEFORMATEX* wf)
{
    AmbAudioFormat f;
    char text[128] = "?";
    if (wf && Format_Parse(&f, wf)) Format_ToString(&f, text, sizeof(text));
    printf("%s: %s\n", tag, text);
}

static int Run(int argc, wchar_t** argv)
{
    const wchar_t* dll_path = NULL;
    int seconds = 3;
    double amplitude = 0.03;
    int silent = 0;
    int use_exclusive = 0;

    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--load") && i + 1 < argc) dll_path = argv[++i];
        else if (!_wcsicmp(argv[i], L"--seconds") && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (!_wcsicmp(argv[i], L"--amplitude") && i + 1 < argc) amplitude = _wtof(argv[++i]);
        else if (!_wcsicmp(argv[i], L"--silent")) silent = 1;
        else if (!_wcsicmp(argv[i], L"--exclusive")) use_exclusive = 1;
        else {
            printf("usage: AMAudioProbe [--load dll] [--seconds n] [--amplitude x] [--silent] [--exclusive]\n");
            return 2;
        }
    }

    if (dll_path) {
        HMODULE mod = LoadLibraryW(dll_path);
        if (!mod) {
            printf("LoadLibrary(%ls) failed: %lu\n", dll_path, (unsigned long)GetLastError());
            return 1;
        }
        printf("loaded %ls, waiting for hooks...\n", dll_path);
        Sleep(600);
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* device = NULL;
    IAudioClient* client = NULL;
    IAudioRenderClient* render = NULL;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) {
        printf("GetDefaultAudioEndpoint failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    LPWSTR id = NULL;
    if (SUCCEEDED(device->lpVtbl->GetId(device, &id)) && id) {
        printf("endpoint: %ls\n", id);
        CoTaskMemFree(id);
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL,
                                  (void**)&client);
    if (FAILED(hr)) {
        printf("Activate(IAudioClient) failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    WAVEFORMATEX* mix = NULL;
    hr = client->lpVtbl->GetMixFormat(client, &mix);
    if (FAILED(hr) || !mix) {
        printf("GetMixFormat failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    PrintFormat("mix format", mix);

    AmbAudioFormat fmt;
    if (!Format_Parse(&fmt, mix)) {
        printf("unsupported mix format\n");
        return 1;
    }

    REFERENCE_TIME def_period = 0, min_period = 0;
    client->lpVtbl->GetDevicePeriod(client, &def_period, &min_period);
    printf("device period: default=%lld min=%lld (100ns)\n", (long long)def_period,
           (long long)min_period);

    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    AUDCLNT_SHAREMODE share = use_exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                            : AUDCLNT_SHAREMODE_SHARED;
    hr = client->lpVtbl->Initialize(client, share, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    10000000, 0, mix, NULL);
    printf("Initialize(share=%d): 0x%08lx\n", (int)share, (unsigned long)hr);
    if (FAILED(hr)) {
        return 1;
    }

    UINT32 buffer_frames = 0;
    hr = client->lpVtbl->GetBufferSize(client, &buffer_frames);
    printf("buffer size: %lu frames (0x%08lx)\n", (unsigned long)buffer_frames,
           (unsigned long)hr);
    if (FAILED(hr) || buffer_frames == 0) return 1;

    hr = client->lpVtbl->SetEventHandle(client, event);
    printf("SetEventHandle: 0x%08lx\n", (unsigned long)hr);

    hr = client->lpVtbl->GetService(client, &IID_IAudioRenderClient, (void**)&render);
    printf("GetService(render): 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !render) return 1;

    IAudioClock* clock = NULL;
    hr = client->lpVtbl->GetService(client, &IID_IAudioClock, (void**)&clock);
    printf("GetService(clock): 0x%08lx\n", (unsigned long)hr);

    ISimpleAudioVolume* volume = NULL;
    hr = client->lpVtbl->GetService(client, &IID_ISimpleAudioVolume, (void**)&volume);
    printf("GetService(volume): 0x%08lx\n", (unsigned long)hr);

    hr = client->lpVtbl->Start(client);
    printf("Start: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    UINT32 channels = fmt.channels;
    UINT32 frame_bytes = fmt.frame_bytes;
    float* scratch = (float*)calloc(buffer_frames * channels, sizeof(float));
    if (!scratch) return 1;

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    double phase = 0.0;
    double step = 2.0 * 3.14159265358979323846 * 440.0 / (double)fmt.rate;
    LONG64 frames_written = 0;
    UINT32 iterations = 0;

    for (;;) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (elapsed >= (double)seconds) break;

        DWORD wait = WaitForSingleObject(event, 50);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) break;

        UINT32 padding = 0;
        if (FAILED(client->lpVtbl->GetCurrentPadding(client, &padding))) break;
        if (padding > buffer_frames) padding = buffer_frames;
        UINT32 avail = buffer_frames - padding;
        if (avail == 0) continue;

        BYTE* data = NULL;
        hr = render->lpVtbl->GetBuffer(render, avail, &data);
        if (FAILED(hr) || !data) {
            printf("GetBuffer failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }

        if (silent) {
            memset(data, 0, (size_t)avail * frame_bytes);
        } else if (fmt.is_float && fmt.bits == 32) {
            for (UINT32 i = 0; i < avail; ++i) {
                float v = (float)(sin(phase) * amplitude);
                phase += step;
                if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
                for (UINT32 c = 0; c < channels; ++c) scratch[i * channels + c] = v;
            }
            memcpy(data, scratch, (size_t)avail * frame_bytes);
        } else {
            /* Integer formats are not exercised by this simple probe. */
            memset(data, 0, (size_t)avail * frame_bytes);
        }

        hr = render->lpVtbl->ReleaseBuffer(render, avail, 0);
        if (FAILED(hr)) {
            printf("ReleaseBuffer failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }
        frames_written += avail;
        iterations++;
    }

    client->lpVtbl->Stop(client);
    printf("played %lld frames in %lu iterations\n", (long long)frames_written,
           (unsigned long)iterations);

    if (clock) {
        UINT64 f = 0, p = 0;
        if (SUCCEEDED(clock->lpVtbl->GetFrequency(clock, &f)) &&
            SUCCEEDED(clock->lpVtbl->GetPosition(clock, &p, NULL))) {
            printf("clock: freq=%llu position=%llu\n", (unsigned long long)f,
                   (unsigned long long)p);
        }
        clock->lpVtbl->Release(clock);
    }
    if (volume) volume->lpVtbl->Release(volume);
    if (render) render->lpVtbl->Release(render);
    if (event) CloseHandle(event);
    if (mix) CoTaskMemFree(mix);
    client->lpVtbl->Release(client);
    device->lpVtbl->Release(device);
    enumerator->lpVtbl->Release(enumerator);
    free(scratch);
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    int rc = Run(argc, argv);
    return rc;
}

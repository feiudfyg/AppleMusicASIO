#include "comshim_common.h"

static std::wstring BridgePath()
{
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    if (n == 0 || n >= ARRAYSIZE(local)) return {};
    return std::wstring(local) + L"\\AppleMusicASIO\\AMAudioBridge.dll";
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("PreStartHelper start");
    CoLogPackageIdentity();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("PreStartHelper CoInitializeEx hr=0x%08lx", hr);
    if (FAILED(hr)) return 1;

    IUnknown* library = nullptr;
    hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IUnknown, reinterpret_cast<void**>(&library));
    ComLog("PreStartHelper CoCreateInstance hr=0x%08lx ptr=%p", hr, library);

    DWORD pid = 0;
    for (int i = 0; i < 100; ++i) {
        pid = FindProcessByName(L"AMPLibraryAgent.exe");
        if (pid) break;
        Sleep(100);
    }
    ComLog("PreStartHelper agent pid=%lu", pid);

    std::wstring dll = BridgePath();
    ComLog("PreStartHelper bridge path=%ls", dll.c_str());
    bool injected = false;
    if (pid && !dll.empty()) {
        injected = InjectDll(pid, dll);
        ComLog("PreStartHelper inject ok=%d", injected ? 1 : 0);
    } else {
        ComLog("PreStartHelper missing pid or bridge");
    }

    if (library) library->Release();
    CoUninitialize();
    Sleep(3000);
    ComLog("PreStartHelper exit injected=%d", injected ? 1 : 0);
    return (pid && injected) ? 0 : 2;
}

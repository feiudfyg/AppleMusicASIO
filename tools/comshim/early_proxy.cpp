#include "comshim_common.h"

#include <string>

using PFN_DllGetClassObject = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using PFN_DllCanUnloadNow = HRESULT (STDAPICALLTYPE*)();

static HMODULE g_realModule = nullptr;
static LONG g_prestartStarted = 0;
static LONG g_prestartRunning = 0;

static std::wstring BridgePath()
{
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    if (n == 0 || n >= ARRAYSIZE(local)) return {};
    return std::wstring(local) + L"\\AppleMusicASIO\\AMAudioBridge.dll";
}

static DWORD WINAPI PrestartThread(void*)
{
    ComLog("early proxy prestart thread start");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("early proxy CoInitializeEx hr=0x%08lx", hr);
    if (FAILED(hr)) {
        InterlockedExchange(&g_prestartRunning, 0);
        return 0;
    }

    IUnknown* library = nullptr;
    hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IUnknown, reinterpret_cast<void**>(&library));
    ComLog("early proxy CoCreateInstance hr=0x%08lx ptr=%p", hr, library);

    DWORD pid = 0;
    for (int i = 0; i < 100; ++i) {
        pid = FindProcessByName(L"AMPLibraryAgent.exe");
        if (pid) break;
        Sleep(100);
    }
    ComLog("early proxy agent pid=%lu", pid);

    std::wstring dll = BridgePath();
    bool injected = pid && !dll.empty() && InjectDll(pid, dll);
    ComLog("early proxy inject pid=%lu ok=%d path=%ls",
           pid, injected ? 1 : 0, dll.c_str());

    if (library) library->Release();
    CoUninitialize();
    ComLog("early proxy prestart thread exit");
    InterlockedExchange(&g_prestartRunning, 0);
    return 0;
}

static void StartPrestartIfNeeded()
{
    wchar_t exe[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (n == 0 || n >= ARRAYSIZE(exe)) return;
    if (!wcsstr(exe, L"AppleMusic.exe")) return;
    if (InterlockedCompareExchange(&g_prestartStarted, 1, 0) != 0) return;
    InterlockedExchange(&g_prestartRunning, 1);
    ComLog("early proxy loaded in AppleMusic.exe");
    HANDLE thread = CreateThread(nullptr, 0, PrestartThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_prestartRunning, 0);
    }
}

static HMODULE LoadRealPropsys()
{
    if (g_realModule) return g_realModule;
    wchar_t systemDir[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir));
    if (n == 0 || n >= ARRAYSIZE(systemDir)) return nullptr;
    std::wstring path = std::wstring(systemDir) + L"\\propsys.dll";
    g_realModule = LoadLibraryExW(path.c_str(), nullptr, 0);
    ComLog("early proxy LoadLibrary propsys=%p", g_realModule);
    return g_realModule;
}

extern "C" HRESULT STDAPICALLTYPE
DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    StartPrestartIfNeeded();
    HMODULE real = LoadRealPropsys();
    if (!real) return CLASS_E_CLASSNOTAVAILABLE;
    auto fn = reinterpret_cast<PFN_DllGetClassObject>(
        GetProcAddress(real, "DllGetClassObject"));
    return fn ? fn(clsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    if (InterlockedCompareExchange(&g_prestartRunning, 0, 0) != 0) return S_FALSE;
    HMODULE real = LoadRealPropsys();
    if (!real) return S_FALSE;
    auto fn = reinterpret_cast<PFN_DllCanUnloadNow>(
        GetProcAddress(real, "DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}

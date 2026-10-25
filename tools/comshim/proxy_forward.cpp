#include "comshim_common.h"

// Apple's IAMPMusicLibrary proxy/stub. The package manifest registers this as a
// packaged activatable-class proxyStub; the real implementation lives in
// AMPLibraryAgent.Proxies.dll next to AppleMusic.exe.
static const CLSID kProxyStubClsid = {
    0xf707a913, 0xe0ce, 0x4fd4, { 0xbc, 0xe3, 0x42, 0x5d, 0xd1, 0x53, 0x28, 0x5b }
};

static const wchar_t kRealProxyPath[] =
    L"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa\\AMPLibraryAgent.Proxies.dll";

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

static HMODULE g_realModule = nullptr;
static DllGetClassObjectFn g_realGetClassObject = nullptr;
static DllCanUnloadNowFn g_realCanUnloadNow = nullptr;

static bool LoadReal()
{
    if (g_realModule) return true;
    g_realModule = LoadLibraryW(kRealProxyPath);
    if (!g_realModule) {
        ComLog("proxy forward LoadLibrary failed=%lu path=%ls", GetLastError(), kRealProxyPath);
        return false;
    }
    g_realGetClassObject = reinterpret_cast<DllGetClassObjectFn>(
        GetProcAddress(g_realModule, "DllGetClassObject"));
    g_realCanUnloadNow = reinterpret_cast<DllCanUnloadNowFn>(
        GetProcAddress(g_realModule, "DllCanUnloadNow"));
    ComLog("proxy forward loaded real base=%p get=%p unload=%p",
           g_realModule, g_realGetClassObject, g_realCanUnloadNow);
    return g_realGetClassObject != nullptr;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid,
                                                    REFIID iid, void** object)
{
    ComLog("proxy forward DllGetClassObject clsid=%08lx-%04x iid=%08lx-%04x",
           clsid.Data1, clsid.Data2, iid.Data1, iid.Data2);
    if (clsid != kProxyStubClsid) {
        ComLog("proxy forward rejecting unexpected CLSID");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    if (!LoadReal()) return CLASS_E_CLASSNOTAVAILABLE;
    HRESULT hr = g_realGetClassObject(clsid, iid, object);
    ComLog("proxy forward DllGetClassObject forwarded hr=0x%08lx", hr);
    return hr;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    if (!LoadReal()) return S_FALSE;
    return g_realCanUnloadNow ? g_realCanUnloadNow() : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        ComLog("proxy forward DllMain attach");
    }
    return TRUE;
}

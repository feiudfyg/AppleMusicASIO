#include "comshim_common.h"
#include <appmodel.h>

#include <new>

#ifdef TARGET_APPLE
static const CLSID& TargetClsid() { return kAppleMusicLibrary; }
static const CLSID& ProxyClsid() { return kAppleMusicLibrary; }
static const CLSID& RealClsid() { return kAppleMusicLibrary; }
#else
static const CLSID& TargetClsid() { return kTestProxy; }
static const CLSID& ProxyClsid() { return kTestProxy; }
static const CLSID& RealClsid() { return kTestReal; }
#endif

namespace {

thread_local bool g_inForward = false;

HRESULT ActivateReal(REFIID iid, void** object)
{
    if (g_inForward) {
        ComLog("recursive forward blocked iid=%08lx", iid.Data1);
        return E_UNEXPECTED;
    }
    g_inForward = true;
    HRESULT hr = CoCreateInstance(TargetClsid(), nullptr, CLSCTX_LOCAL_SERVER,
                                  iid, object);
    g_inForward = false;
    ComLog("activate real CLSCTX_LOCAL_SERVER hr=0x%08lx ptr=%p", hr,
           object ? *object : nullptr);
    return hr;
}

#ifdef TARGET_APPLE
void InjectAppleProbe()
{
    DWORD pid = 0;
    for (int attempt = 0; attempt < 40 && !pid; ++attempt) {
        pid = FindProcessByName(L"AMPLibraryAgent.exe");
        if (!pid) Sleep(50);
    }
    if (!pid) {
        ComLog("AMPLibraryAgent.exe not found for probe injection");
        return;
    }
    std::wstring probe = ModuleDirectory() + L"\\AMExeProbe.dll";
    bool ok = InjectDll(pid, probe);
    ComLog("probe injection pid=%lu path=%ls ok=%d", pid, probe.c_str(), ok ? 1 : 0);
}
#endif

class ForwardFactory final : public IClassFactory {
public:
    explicit ForwardFactory(IUnknown* real = nullptr) : real_(real)
    {
        if (real_) real_->AddRef();
    }

    ~ForwardFactory()
    {
        if (real_) real_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *object = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG value = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (value == 0) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
                                             void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        HRESULT hr = S_OK;
#ifdef TARGET_APPLE
        if (!real_) {
            hr = ActivateReal(iid, object);
            if (SUCCEEDED(hr)) {
                InjectAppleProbe();
            }
        } else {
            hr = real_->QueryInterface(iid, object);
            ComLog("shim QI real iid=%08lx hr=0x%08lx ptr=%p", iid.Data1, hr,
                   object ? *object : nullptr);
        }
#else
        if (real_) {
            hr = real_->QueryInterface(iid, object);
            ComLog("shim QI real iid=%08lx hr=0x%08lx ptr=%p", iid.Data1, hr,
                   object ? *object : nullptr);
        } else {
            hr = ActivateReal(iid, object);
        }
#endif
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        ComLog("LockServer lock=%d", lock ? 1 : 0);
        return S_OK;
    }

private:
    LONG refs_{1};
    IUnknown* real_{};
};

} // namespace

#ifdef BUILD_HANDLER

static ForwardFactory g_handlerFactory;

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid,
                                                    REFIID iid, void** object)
{
    ComLog("DllGetClassObject clsid=%08lx-%04x iid=%08lx-%04x",
           clsid.Data1, clsid.Data2, iid.Data1, iid.Data2);
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != TargetClsid()) return CLASS_E_CLASSNOTAVAILABLE;
    return g_handlerFactory.QueryInterface(iid, object);
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow()
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        ComLog("handler DllMain DLL_PROCESS_ATTACH");
    }
    return TRUE;
}

#else // BUILD_SHIM

#ifdef TARGET_APPLE
struct PackagedOverride {
    std::wstring executable;
    DWORD isSystemExecutable = 0;
    bool valid = false;
};

static std::wstring PackageServerKey()
{
    UINT32 length = 0;
    LONG rc = GetCurrentPackageFullName(&length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0 || length > 1024) {
        ComLog("shim GetCurrentPackageFullName probe failed rc=%ld length=%lu", rc, length);
        return {};
    }
    wchar_t fullName[1024]{};
    rc = GetCurrentPackageFullName(&length, fullName);
    if (rc != ERROR_SUCCESS) {
        ComLog("shim GetCurrentPackageFullName failed rc=%ld", rc);
        return {};
    }
    ComLog("shim package identity: %ls", fullName);
    return std::wstring(L"Software\\Classes\\PackagedCom\\Package\\") + fullName +
           L"\\Server\\0";
}

static bool TakePackagedOverride(PackagedOverride& saved)
{
    std::wstring keyPath = PackageServerKey();
    if (keyPath.empty()) return false;
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                            KEY_READ | KEY_WRITE, &key);
    if (rc != ERROR_SUCCESS) {
        ComLog("shim open PackagedCom key failed=%ld", rc);
        return false;
    }

    wchar_t value[2048]{};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    rc = RegQueryValueExW(key, L"Executable", nullptr, &type,
                          reinterpret_cast<BYTE*>(value), &bytes);
    if (rc == ERROR_SUCCESS) saved.executable = value;
    DWORD sysBytes = sizeof(saved.isSystemExecutable);
    rc = RegQueryValueExW(key, L"IsSystemExecutable", nullptr, &type,
                          reinterpret_cast<BYTE*>(&saved.isSystemExecutable), &sysBytes);
    saved.valid = !saved.executable.empty();

    const wchar_t originalExe[] = L"AMPLibraryAgent.exe";
    DWORD originalSystem = 0;
    LONG exeRc = RegSetValueExW(key, L"Executable", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(originalExe),
                                sizeof(originalExe));
    LONG sysRc = RegSetValueExW(key, L"IsSystemExecutable", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&originalSystem),
                                sizeof(originalSystem));
    RegCloseKey(key);
    ComLog("shim took PackagedCom override exe=%ls isSystem=%lu exeRc=%ld sysRc=%ld",
           saved.executable.c_str(), saved.isSystemExecutable, exeRc, sysRc);
    return exeRc == ERROR_SUCCESS && sysRc == ERROR_SUCCESS;
}

static void RestorePackagedOverride(const PackagedOverride& saved)
{
    if (!saved.valid) return;
    std::wstring keyPath = PackageServerKey();
    if (keyPath.empty()) return;
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                            KEY_WRITE, &key);
    if (rc != ERROR_SUCCESS) {
        ComLog("shim reopen PackagedCom key failed=%ld", rc);
        return;
    }
    LONG exeRc = RegSetValueExW(key, L"Executable", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(saved.executable.c_str()),
                                static_cast<DWORD>((saved.executable.size() + 1) * sizeof(wchar_t)));
    LONG sysRc = RegSetValueExW(key, L"IsSystemExecutable", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&saved.isSystemExecutable),
                                sizeof(saved.isSystemExecutable));
    RegCloseKey(key);
    ComLog("shim restored PackagedCom override exeRc=%ld sysRc=%ld", exeRc, sysRc);
}
#endif

static IUnknown* ActivateShimReal()
{
#ifdef TARGET_APPLE
    // The shim itself is the packaged server. A nested CoCreateInstance would
    // recurse back into this CLSID. CoCreateInstanceFromApp asks the package
    // graph for the class and can bypass the registry server override.
    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIOAgentShim_Activating");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("shim recursive activation blocked");
        CloseHandle(guard);
        return nullptr;
    }

    IUnknown* real = nullptr;
    MULTI_QI result{&IID_IUnknown, nullptr, S_OK};
    HRESULT hr = CoCreateInstanceFromApp(RealClsid(), nullptr, CLSCTX_LOCAL_SERVER,
                                         nullptr, 1, &result);
    ComLog("shim CoCreateInstanceFromApp hr=0x%08lx qi=0x%08lx ptr=%p",
           hr, result.hr, result.pItf);
    if (SUCCEEDED(hr) && SUCCEEDED(result.hr) && result.pItf) {
        real = result.pItf;
    } else {
        PackagedOverride saved;
        bool removed = TakePackagedOverride(saved);
        hr = CoCreateInstance(RealClsid(), nullptr, CLSCTX_LOCAL_SERVER,
                              IID_IUnknown, reinterpret_cast<void**>(&real));
        RestorePackagedOverride(saved);
        ComLog("shim activate packaged real removed=%d hr=0x%08lx ptr=%p",
               removed ? 1 : 0, hr, real);
    }
    if (guard) CloseHandle(guard);
#else
    IUnknown* real = nullptr;
    HRESULT hr = CoCreateInstance(RealClsid(), nullptr, CLSCTX_LOCAL_SERVER,
                                  IID_IUnknown, reinterpret_cast<void**>(&real));
    ComLog("shim activate test real hr=0x%08lx ptr=%p", hr, real);
#endif
    return real;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("shim start");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("shim CoInitializeEx hr=0x%08lx", hr);

    IUnknown* real = SUCCEEDED(hr) ? ActivateShimReal() : nullptr;
    auto* factory = new (std::nothrow) ForwardFactory(real);
    if (real) real->Release();

    DWORD cookie = 0;
    if (factory) {
        hr = CoRegisterClassObject(ProxyClsid(), factory, CLSCTX_LOCAL_SERVER,
                                   REGCLS_MULTIPLEUSE, &cookie);
        ComLog("shim CoRegisterClassObject hr=0x%08lx cookie=%lu", hr, cookie);
        factory->Release();
    }

    Sleep(INFINITE);
    return 0;
}

#endif

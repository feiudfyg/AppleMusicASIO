#include "comshim_common.h"

static const wchar_t kServerKey[] =
    L"Software\\Classes\\PackagedCom\\Package\\"
    L"AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa"
    L"\\Server\\0";

#ifdef TEST_FACTORY
class DummyObject : public IUnknown
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        ComLog("DummyObject QI data1=%08lx failed", riid.Data1);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = InterlockedDecrement(&ref_);
        if (!n) delete this;
        return n;
    }
private:
    LONG ref_ = 1;
};

class DummyFactory : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = InterlockedDecrement(&ref_);
        if (!n) delete this;
        return n;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        ComLog("DummyFactory CreateInstance data1=%08lx", riid.Data1);
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(new DummyObject());
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
private:
    LONG ref_ = 1;
};
#endif

struct SavedValue
{
    DWORD type = 0;
    DWORD size = 0;
    BYTE data[1024]{};
};

static bool ReadValue(HKEY key, const wchar_t* name, SavedValue* saved)
{
    saved->size = sizeof(saved->data);
    LONG rc = RegQueryValueExW(key, name, nullptr, &saved->type,
                               saved->data, &saved->size);
    if (rc != ERROR_SUCCESS) {
        ComLog("real restore read %ls failed=%ld", name, rc);
        return false;
    }
    return true;
}

static bool WriteValue(HKEY key, const wchar_t* name, const SavedValue& saved)
{
    LONG rc = RegSetValueExW(key, name, 0, saved.type, saved.data, saved.size);
    if (rc != ERROR_SUCCESS) {
        ComLog("real restore write %ls failed=%ld", name, rc);
        return false;
    }
    return true;
}

static bool WriteOriginal(HKEY key)
{
    const wchar_t exe[] = L"AMPLibraryAgent.exe";
    DWORD system = 0;
    LONG exeRc = RegSetValueExW(key, L"Executable", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(exe), sizeof(exe));
    LONG sysRc = RegSetValueExW(key, L"IsSystemExecutable", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&system), sizeof(system));
    ComLog("real restore set original exeRc=%ld sysRc=%ld", exeRc, sysRc);
    return exeRc == ERROR_SUCCESS && sysRc == ERROR_SUCCESS;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("RealRestoreProbe start");
    CoLogPackageIdentity();

    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIORealRestoreProbe");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("RealRestoreProbe recursive activation blocked");
        CloseHandle(guard);
        return 0;
    }

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0,
                            KEY_READ | KEY_SET_VALUE, &key);
    ComLog("real restore open server key rc=%ld", rc);
    if (rc != ERROR_SUCCESS) {
        if (guard) CloseHandle(guard);
        return 1;
    }

    SavedValue exe{};
    SavedValue system{};
    bool saved = ReadValue(key, L"Executable", &exe) &&
                 ReadValue(key, L"IsSystemExecutable", &system);
    bool activated = false;

    if (saved) {
        if (WriteOriginal(key)) {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComLog("real restore CoInitializeEx hr=0x%08lx", hr);
            if (SUCCEEDED(hr)) {
#ifdef TEST_FACTORY
                DummyFactory* factory = new DummyFactory();
                DWORD cookie = 0;
                HRESULT reg = CoRegisterClassObject(
                    kAppleMusicLibrary, factory, CLSCTX_LOCAL_SERVER,
                    REGCLS_MULTIPLEUSE, &cookie);
                ComLog("dummy CoRegisterClassObject hr=0x%08lx cookie=%lu", reg, cookie);
#endif
                IUnknown* real = nullptr;
                hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                                      IID_IUnknown, reinterpret_cast<void**>(&real));
                ComLog("real restore CoCreateInstance hr=0x%08lx ptr=%p", hr, real);
                if (real) {
                    real->Release();
                    activated = hr == S_OK;
                }
#ifdef TEST_FACTORY
                if (cookie) CoRevokeClassObject(cookie);
                factory->Release();
#endif
                CoUninitialize();
            }
        }

        WriteValue(key, L"Executable", exe);
        WriteValue(key, L"IsSystemExecutable", system);
    }

    RegCloseKey(key);
    if (guard) CloseHandle(guard);
    Sleep(5000);
    ComLog("RealRestoreProbe exit activated=%d", activated ? 1 : 0);
    return activated ? 0 : 2;
}

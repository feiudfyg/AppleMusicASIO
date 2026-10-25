#include "comshim_common.h"

#include <shellapi.h>
#include <vector>

static const wchar_t kServerKey[] =
    L"Software\\Classes\\PackagedCom\\Package\\"
    L"AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa"
    L"\\Server\\0";
static const wchar_t kProxyFile[] =
    L"C:\\Users\\Public\\AppleMusicASIO-RE\\proxy.bin";
static const wchar_t kHelperAumid[] =
    L"shell:AppsFolder\\A1.AppleMusicASIO.ClientProbe_zmt4ax4d9f4ta!Client";

class ProxyFactory : public IClassFactory
{
public:
    ProxyFactory()
    {
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    void SetProxy(IUnknown* proxy)
    {
        if (proxy_) proxy_->AddRef();
        proxy_ = proxy;
        if (ready_) SetEvent(ready_);
    }

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

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&ref_);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = InterlockedDecrement(&ref_);
        if (!n) delete this;
        return n;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        ComLog("ProxyFactory CreateInstance data1=%08lx", riid.Data1);
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (ready_) {
            DWORD wait = WaitForSingleObject(ready_, 15000);
            ComLog("ProxyFactory wait proxy wait=%lu", wait);
        }
        if (!proxy_) return E_UNEXPECTED;
        HRESULT hr = proxy_->QueryInterface(riid, ppv);
        ComLog("ProxyFactory QueryInterface data1=%08lx hr=0x%08lx ptr=%p",
               riid.Data1, hr, *ppv);
        return hr;
    }

    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    ~ProxyFactory()
    {
        if (proxy_) proxy_->Release();
        if (ready_) CloseHandle(ready_);
    }

    LONG ref_ = 1;
    IUnknown* proxy_ = nullptr;
    HANDLE ready_ = nullptr;
};

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
        ComLog("proxy shim read %ls failed=%ld", name, rc);
        return false;
    }
    return true;
}

static bool WriteValue(HKEY key, const wchar_t* name, const SavedValue& saved)
{
    LONG rc = RegSetValueExW(key, name, 0, saved.type, saved.data, saved.size);
    ComLog("proxy shim restore %ls rc=%ld", name, rc);
    return rc == ERROR_SUCCESS;
}

static bool WriteOriginal(HKEY key)
{
    const wchar_t exe[] = L"AMPLibraryAgent.exe";
    DWORD system = 0;
    LONG exeRc = RegSetValueExW(key, L"Executable", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(exe), sizeof(exe));
    LONG sysRc = RegSetValueExW(key, L"IsSystemExecutable", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&system), sizeof(system));
    ComLog("proxy shim set original exeRc=%ld sysRc=%ld", exeRc, sysRc);
    return exeRc == ERROR_SUCCESS && sysRc == ERROR_SUCCESS;
}

static IUnknown* ReadProxyFromFile()
{
    HANDLE file = CreateFileW(kProxyFile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ComLog("proxy shim open proxy file failed=%lu", GetLastError());
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart == 0 || size.QuadPart > 65536) {
        ComLog("proxy shim bad proxy size=%lld", static_cast<long long>(size.QuadPart));
        CloseHandle(file);
        return nullptr;
    }

    std::vector<BYTE> data(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    ComLog("proxy shim read proxy ok=%d read=%lu", ok, read);
    if (!ok || read != data.size()) return nullptr;

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) return nullptr;
    ULONG written = 0;
    hr = stream->Write(data.data(), static_cast<ULONG>(data.size()), &written);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    IUnknown* proxy = nullptr;
    hr = CoUnmarshalInterface(stream, IID_IUnknown, reinterpret_cast<void**>(&proxy));
    ComLog("proxy shim CoUnmarshalInterface hr=0x%08lx written=%lu ptr=%p",
           hr, written, proxy);
    stream->Release();
    return proxy;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("ProxyShim start");
    CoLogPackageIdentity();

    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIOProxyShim");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("ProxyShim recursive activation blocked");
        CloseHandle(guard);
        return 0;
    }

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0,
                            KEY_READ | KEY_SET_VALUE, &key);
    ComLog("proxy shim open server key rc=%ld", rc);
    if (rc != ERROR_SUCCESS) {
        if (guard) CloseHandle(guard);
        return 1;
    }

    SavedValue shimExe{};
    SavedValue shimSystem{};
    bool saved = ReadValue(key, L"Executable", &shimExe) &&
                 ReadValue(key, L"IsSystemExecutable", &shimSystem);
    if (!saved || !WriteOriginal(key)) {
        RegCloseKey(key);
        if (guard) CloseHandle(guard);
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("proxy shim CoInitializeEx hr=0x%08lx", hr);
    if (FAILED(hr)) {
        WriteValue(key, L"Executable", shimExe);
        WriteValue(key, L"IsSystemExecutable", shimSystem);
        RegCloseKey(key);
        if (guard) CloseHandle(guard);
        return 3;
    }

    ProxyFactory* factory = new ProxyFactory();
    DWORD cookie = 0;
    hr = CoRegisterClassObject(kAppleMusicLibrary, factory, CLSCTX_LOCAL_SERVER,
                               REGCLS_SUSPENDED | REGCLS_MULTIPLEUSE, &cookie);
    ComLog("proxy shim CoRegisterClassObject suspended hr=0x%08lx cookie=%lu",
           hr, cookie);

    DeleteFileW(kProxyFile);
    HINSTANCE shell = ShellExecuteW(nullptr, L"open", kHelperAumid,
                                   nullptr, nullptr, SW_SHOWNORMAL);
    ComLog("proxy shim ShellExecute helper=%p", shell);

    IUnknown* proxy = nullptr;
    for (int i = 0; i < 200; ++i) {
        proxy = ReadProxyFromFile();
        if (proxy) break;
        Sleep(100);
    }
    if (!proxy) {
        ComLog("proxy shim no proxy after wait");
        if (cookie) CoRevokeClassObject(cookie);
        factory->Release();
        WriteValue(key, L"Executable", shimExe);
        WriteValue(key, L"IsSystemExecutable", shimSystem);
        CoUninitialize();
        RegCloseKey(key);
        if (guard) CloseHandle(guard);
        return 4;
    }

    factory->SetProxy(proxy);
    hr = CoResumeClassObjects();
    ComLog("proxy shim CoResumeClassObjects hr=0x%08lx", hr);

    WriteValue(key, L"Executable", shimExe);
    WriteValue(key, L"IsSystemExecutable", shimSystem);

    Sleep(15000);
    if (cookie) CoRevokeClassObject(cookie);
    factory->Release();
    proxy->Release();
    CoUninitialize();
    RegCloseKey(key);
    if (guard) CloseHandle(guard);
    ComLog("ProxyShim exit");
    return 0;
}

#include "comshim_common.h"

#include <new>

namespace {

class EnumString final : public IEnumString {
public:
    EnumString() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumString) {
            *object = static_cast<IEnumString*>(this);
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

    HRESULT STDMETHODCALLTYPE Next(ULONG count, LPOLESTR* strings,
                                   ULONG* fetched) override
    {
        ComLog("EnumString::Next count=%lu index=%lu", count, index_);
        if (fetched) *fetched = 0;
        if (!strings || count == 0) return S_OK;
        for (ULONG i = 0; i < count; ++i) strings[i] = nullptr;
        if (index_ != 0) return S_FALSE;

        const wchar_t text[] = L"hello from real COM server";
        size_t bytes = sizeof(text);
        auto* copy = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
        if (!copy) return E_OUTOFMEMORY;
        memcpy(copy, text, bytes);
        strings[0] = copy;
        index_ = 1;
        if (fetched) *fetched = 1;
        return count == 1 ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
    {
        index_ += count;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumString** copy) override
    {
        if (!copy) return E_POINTER;
        auto* clone = new (std::nothrow) EnumString();
        if (!clone) return E_OUTOFMEMORY;
        clone->index_ = index_;
        *copy = clone;
        return S_OK;
    }

private:
    LONG refs_{1};
    ULONG index_{0};
};

class Factory final : public IClassFactory {
public:
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
        ComLog("Factory::CreateInstance iid=%08lx-%04x-%04x", iid.Data1, iid.Data2, iid.Data3);
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!object) return E_POINTER;
        *object = nullptr;
        auto* instance = new (std::nothrow) EnumString();
        if (!instance) return E_OUTOFMEMORY;
        HRESULT hr = instance->QueryInterface(iid, object);
        instance->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        ComLog("Factory::LockServer lock=%d", lock ? 1 : 0);
        return S_OK;
    }

private:
    LONG refs_{1};
};

} // namespace

int wmain(int argc, wchar_t**)
{
    ComLog("TestServer start argc=%d", argc);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("CoInitializeEx hr=0x%08lx", hr);
    if (FAILED(hr)) return 1;

    auto* factory = new (std::nothrow) Factory();
    if (!factory) return 2;

    DWORD cookieProxy = 0;
    DWORD cookieReal = 0;
    hr = CoRegisterClassObject(kTestProxy, factory, CLSCTX_LOCAL_SERVER,
                               REGCLS_MULTIPLEUSE, &cookieProxy);
    ComLog("CoRegisterClassObject(proxy) hr=0x%08lx cookie=%lu", hr, cookieProxy);
    HRESULT hrReal = CoRegisterClassObject(kTestReal, factory, CLSCTX_LOCAL_SERVER,
                                           REGCLS_MULTIPLEUSE, &cookieReal);
    ComLog("CoRegisterClassObject(real) hr=0x%08lx cookie=%lu", hrReal, cookieReal);

    Sleep(INFINITE);
    return 0;
}

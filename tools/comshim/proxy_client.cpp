#include "comshim_common.h"

#include <vector>

static const wchar_t kProxyFile[] =
    L"C:\\Users\\Public\\AppleMusicASIO-RE\\proxy.bin";

static bool WriteProxy(IUnknown* object)
{
    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    ComLog("proxy client CreateStreamOnHGlobal hr=0x%08lx", hr);
    if (FAILED(hr)) return false;

    hr = CoMarshalInterface(stream, IID_IUnknown, object, MSHCTX_LOCAL,
                            nullptr, MSHLFLAGS_NORMAL);
    ComLog("proxy client CoMarshalInterface hr=0x%08lx", hr);
    if (FAILED(hr)) {
        stream->Release();
        return false;
    }

    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 65536) {
        ComLog("proxy client stream stat hr=0x%08lx size=%llu", hr,
               static_cast<unsigned long long>(stat.cbSize.QuadPart));
        stream->Release();
        return false;
    }

    std::vector<BYTE> data(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    hr = stream->Read(data.data(), static_cast<ULONG>(data.size()), &read);
    stream->Release();
    ComLog("proxy client stream read hr=0x%08lx read=%lu", hr, read);
    if (FAILED(hr) || read != data.size()) return false;

    HANDLE file = CreateFileW(kProxyFile, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ComLog("proxy client create file failed=%lu", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()),
                        &written, nullptr);
    CloseHandle(file);
    ComLog("proxy client write file ok=%d written=%lu", ok, written);
    return ok && written == data.size();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("ProxyClient start");
    CoLogPackageIdentity();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("ProxyClient CoInitializeEx hr=0x%08lx", hr);
    if (FAILED(hr)) return 1;

    IUnknown* object = nullptr;
    hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IUnknown, reinterpret_cast<void**>(&object));
    ComLog("ProxyClient CoCreateInstance hr=0x%08lx ptr=%p", hr, object);
    if (SUCCEEDED(hr) && object) {
        WriteProxy(object);
        object->Release();
    }

    CoUninitialize();
    ComLog("ProxyClient exit");
    return SUCCEEDED(hr) ? 0 : 2;
}

#include "comshim_common.h"

#include <cstdio>
#include <cstring>

int wmain(int argc, wchar_t** argv)
{
    DWORD flags = CLSCTX_ALL;
    if (argc > 1 && _wcsicmp(argv[1], L"local") == 0) {
        flags = CLSCTX_LOCAL_SERVER;
    } else if (argc > 1 && _wcsicmp(argv[1], L"all") == 0) {
        flags = CLSCTX_ALL;
    } else {
        wprintf(L"usage: TestClient [all|local]\n");
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        wprintf(L"CoInitializeEx failed 0x%08lx\n", hr);
        return 1;
    }

    IEnumString* strings = nullptr;
    hr = CoCreateInstance(kTestProxy, nullptr, flags, IID_IEnumString,
                          reinterpret_cast<void**>(&strings));
    wprintf(L"CoCreateInstance flags=0x%lx hr=0x%08lx ptr=%p\n",
            flags, hr, strings);
    if (FAILED(hr) || !strings) {
        CoUninitialize();
        return 1;
    }

    LPOLESTR value = nullptr;
    ULONG fetched = 0;
    hr = strings->Next(1, &value, &fetched);
    wprintf(L"Next hr=0x%08lx fetched=%lu value=%ls\n", hr, fetched,
            value ? value : L"<null>");
    bool ok = SUCCEEDED(hr) && fetched == 1 && value &&
              wcscmp(value, L"hello from real COM server") == 0;
    if (value) CoTaskMemFree(value);
    strings->Release();
    CoUninitialize();
    wprintf(L"result=%s\n", ok ? L"PASS" : L"FAIL");
    return ok ? 0 : 1;
}

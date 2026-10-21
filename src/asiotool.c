#include "common.h"
#include "asio_iface.h"

static int EnumDrivers(wchar_t keys[][128], wchar_t descs[][256], wchar_t clsids[][64], int max)
{
    HKEY root = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return 0;
    }
    int count = 0;
    for (DWORD i = 0; count < max; ++i) {
        wchar_t key[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, key, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY sub = NULL;
        if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &sub) != ERROR_SUCCESS) continue;
        DWORD type = 0, size = 256;
        RegQueryValueExW(sub, L"Description", NULL, &type, (BYTE*)descs[count], &size);
        size = sizeof(clsids[count]);
        RegQueryValueExW(sub, L"CLSID", NULL, &type, (BYTE*)clsids[count], &size);
        RegCloseKey(sub);
        wcsncpy_s(keys[count], 128, key, _TRUNCATE);
        if (clsids[count][0]) count++;
    }
    RegCloseKey(root);
    return count;
}

int wmain(void)
{
    wchar_t keys[16][128] = {0}, descs[16][256] = {0}, clsids[16][64] = {0};
    int n = EnumDrivers(keys, descs, clsids, 16);
    printf("drivers: %d\n", n);
    for (int i = 0; i < n; ++i) {
        printf("[%d] %ls | %ls | %ls\n", i, keys[i], descs[i], clsids[i]);
    }
    if (n == 0) return 1;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CLSID clsid;
    HRESULT hr = CLSIDFromString(clsids[0], &clsid);
    printf("CLSIDFromString: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    IASIO* asio = NULL;
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid, (void**)&asio);
    printf("CoCreateInstance(clsid as iid): 0x%08lx ptr=%p\n", (unsigned long)hr, (void*)asio);
    if (FAILED(hr)) {
        hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void**)&asio);
        printf("CoCreateInstance(IUnknown): 0x%08lx ptr=%p\n", (unsigned long)hr, (void*)asio);
    }
    if (FAILED(hr) || !asio) return 1;

    printf("init: %ld\n", (long)IASIO_CALL(asio, init, NULL));
    char name[64] = {0};
    char err[128] = {0};
    IASIO_CALL(asio, getDriverName, name);
    IASIO_CALL(asio, getErrorMessage, err);
    printf("name=%s version=%ld error=%s\n", name, IASIO_CALLO(asio, getDriverVersion), err);

    long ins = 0, outs = 0;
    printf("getChannels: %ld in=%ld out=%ld\n",
           (long)IASIO_CALL(asio, getChannels, &ins, &outs), ins, outs);

    long mn = 0, mx = 0, pref = 0, gran = 0;
    printf("getBufferSize: %ld min=%ld max=%ld pref=%ld gran=%ld\n",
           (long)IASIO_CALL(asio, getBufferSize, &mn, &mx, &pref, &gran), mn, mx, pref, gran);

    ASIOSampleRate rate = 0;
    printf("getSampleRate: %ld rate=%.0f\n",
           (long)IASIO_CALL(asio, getSampleRate, &rate), rate);

    const double rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    for (int i = 0; i < 6; ++i) {
        printf("canSampleRate(%.0f): %ld\n", rates[i],
               (long)IASIO_CALL(asio, canSampleRate, rates[i]));
    }

    long all = ins > outs ? ins : outs;
    for (long c = 0; c < all; ++c) {
        for (int in = 0; in < 2; ++in) {
            if (in == 0 && c >= outs) continue;
            if (in == 1 && c >= ins) continue;
            ASIOChannelInfo ci;
            ZeroMemory(&ci, sizeof(ci));
            ci.channel = c;
            ci.isInput = in ? ASIOTrue : ASIOFalse;
            ASIOError e = IASIO_CALL(asio, getChannelInfo, &ci);
            printf("ch %ld %s: e=%ld type=%ld group=%ld name=%s\n", c, in ? "in " : "out",
                   (long)e, (long)ci.type, ci.channelGroup, ci.name);
        }
    }

    IASIO_CALLO(asio, Release);
    CoUninitialize();
    return 0;
}

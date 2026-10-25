#include <windows.h>
#include <stdio.h>

static void Log(const char* text)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TEMP", path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, L"\\am_com_proxy.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL);
    WriteFile(h, "\r\n", 2, &wrote, NULL);
    CloseHandle(h);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        Log("DllMain process attach");
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    Log("DllGetClassObject enter");
    HMODULE real = LoadLibraryW(L"C:\\Windows\\System32\\MMDevApi.dll");
    if (!real) {
        Log("LoadLibrary(MMDevApi) failed");
        return E_FAIL;
    }
    typedef HRESULT (WINAPI* PFN)(REFCLSID, REFIID, void**);
    PFN fn = (PFN)GetProcAddress(real, "DllGetClassObject");
    if (!fn) {
        Log("GetProcAddress failed");
        return E_FAIL;
    }
    HRESULT hr = fn(rclsid, riid, ppv);
    char line[128];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "forward hr=0x%08lx", (unsigned long)hr);
    Log(line);
    return hr;
}

STDAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

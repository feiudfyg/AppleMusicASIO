#include <windows.h>
#include <objbase.h>
#include <appmodel.h>
#include <stdio.h>
#include <stdint.h>

#define DLL_PROCESS_VERIFIER 4

typedef VOID (NTAPI* RTL_VERIFIER_DLL_LOAD_CALLBACK)(PWSTR DllName, PVOID DllBase,
                                                      SIZE_T DllSize, PVOID Reserved);
typedef VOID (NTAPI* RTL_VERIFIER_DLL_UNLOAD_CALLBACK)(PWSTR DllName, PVOID DllBase,
                                                        SIZE_T DllSize, PVOID Reserved);
typedef VOID (NTAPI* RTL_VERIFIER_NTDLLHEAPFREE_CALLBACK)(PVOID AllocationBase,
                                                           SIZE_T AllocationSize);

typedef struct _RTL_VERIFIER_THUNK_DESCRIPTOR {
    PCHAR ThunkName;
    PVOID ThunkOldAddress;
    PVOID ThunkNewAddress;
} RTL_VERIFIER_THUNK_DESCRIPTOR, *PRTL_VERIFIER_THUNK_DESCRIPTOR;

typedef struct _RTL_VERIFIER_DLL_DESCRIPTOR {
    PWCHAR DllName;
    DWORD DllFlags;
    PVOID DllAddress;
    PRTL_VERIFIER_THUNK_DESCRIPTOR DllThunks;
} RTL_VERIFIER_DLL_DESCRIPTOR, *PRTL_VERIFIER_DLL_DESCRIPTOR;

typedef struct _RTL_VERIFIER_PROVIDER_DESCRIPTOR {
    DWORD Length;
    PRTL_VERIFIER_DLL_DESCRIPTOR ProviderDlls;
    RTL_VERIFIER_DLL_LOAD_CALLBACK ProviderDllLoadCallback;
    RTL_VERIFIER_DLL_UNLOAD_CALLBACK ProviderDllUnloadCallback;

    PWSTR VerifierImage;
    DWORD VerifierFlags;
    DWORD VerifierDebug;
    PVOID RtlpGetStackTraceAddress;
    PVOID RtlpDebugPageHeapCreate;
    PVOID RtlpDebugPageHeapDestroy;

    RTL_VERIFIER_NTDLLHEAPFREE_CALLBACK ProviderNtdllHeapFreeCallback;
} RTL_VERIFIER_PROVIDER_DESCRIPTOR, *PRTL_VERIFIER_PROVIDER_DESCRIPTOR;

static const CLSID kAppleMusicLibrary = {
    0x68e7097c, 0xf969, 0x4006,
    { 0xaa, 0xc3, 0x95, 0x11, 0x5f, 0x0e, 0xd1, 0xc4 }
};

static const wchar_t kPackageFamily[] = L"AppleInc.AppleMusicWin_nzyj5cx40ttqa";
static RTL_VERIFIER_DLL_DESCRIPTOR g_emptyDlls[] = {
    { NULL, 0, NULL, NULL }
};
static RTL_VERIFIER_PROVIDER_DESCRIPTOR g_descriptor;
static volatile LONG g_started;

static void NTAPI NoopLoad(PWSTR name, PVOID base, SIZE_T size, PVOID reserved)
{
    (void)name;
    (void)base;
    (void)size;
    (void)reserved;
}

static void NTAPI NoopUnload(PWSTR name, PVOID base, SIZE_T size, PVOID reserved)
{
    (void)name;
    (void)base;
    (void)size;
    (void)reserved;
}

static void NTAPI NoopHeapFree(PVOID base, SIZE_T size)
{
    (void)base;
    (void)size;
}

static void ShimLog(const wchar_t* text)
{
    wchar_t local[MAX_PATH];
    DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!count || count >= MAX_PATH) return;

    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\AppleMusicASIO", local);
    CreateDirectoryW(dir, NULL);

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\AMVerifierShim.log", dir);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    size_t chars = wcslen(text);
    WriteFile(file, text, (DWORD)(chars * sizeof(wchar_t)), &written, NULL);
    WriteFile(file, L"\r\n", 2 * sizeof(wchar_t), &written, NULL);
    CloseHandle(file);
}

static BOOL IsAppleMusicPackage(void)
{
    UINT32 length = 0;
    LONG rc = GetCurrentPackageFamilyName(&length, NULL);
    if (rc != ERROR_INSUFFICIENT_BUFFER || !length || length > 256) return FALSE;

    wchar_t family[256];
    rc = GetCurrentPackageFamilyName(&length, family);
    if (rc != ERROR_SUCCESS) return FALSE;
    return _wcsicmp(family, kPackageFamily) == 0;
}

static DWORD WINAPI PrestartThread(void* unused)
{
    (void)unused;

    if (!IsAppleMusicPackage()) {
        ShimLog(L"AMVerifierShim: not Apple Music package; skipped");
        InterlockedExchange(&g_started, 0);
        return 0;
    }

    Sleep(100);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        ShimLog(L"AMVerifierShim: CoInitializeEx failed");
        InterlockedExchange(&g_started, 0);
        return 0;
    }

    IUnknown* library = NULL;
    hr = CoCreateInstance(&kAppleMusicLibrary, NULL, CLSCTX_LOCAL_SERVER,
                          &IID_IUnknown, (void**)&library);
    if (SUCCEEDED(hr) && library) library->lpVtbl->Release(library);

    if (SUCCEEDED(hr)) {
        ShimLog(L"AMVerifierShim: Apple library activation succeeded");
    } else {
        ShimLog(L"AMVerifierShim: Apple library activation failed");
    }

    CoUninitialize();
    return 0;
}

static void StartPrestart(void)
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    HANDLE thread = CreateThread(NULL, 0, PrestartThread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_started, 0);
    }
}

static void InitializeDescriptor(void)
{
    g_descriptor.Length = sizeof(g_descriptor);
    g_descriptor.ProviderDlls = g_emptyDlls;
    g_descriptor.ProviderDllLoadCallback = NoopLoad;
    g_descriptor.ProviderDllUnloadCallback = NoopUnload;
    g_descriptor.ProviderNtdllHeapFreeCallback = NoopHeapFree;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;

    if (reason == DLL_PROCESS_VERIFIER) {
        InitializeDescriptor();
        if (reserved) *(PVOID*)reserved = &g_descriptor;
        return TRUE;
    }

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        StartPrestart();
    }

    return TRUE;
}

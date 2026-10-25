#include <windows.h>
#include <appmodel.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>

#include "bridge_hash.h"

#define DLL_PROCESS_VERIFIER 4
#define MACHINE_KEY L"Software\\AppleMusicASIO\\Machine"

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

static BOOL IsProcessNamed(const wchar_t* name)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;

    const wchar_t* base = wcsrchr(exe, L'\\');
    base = base ? base + 1 : exe;
    return _wcsicmp(base, name) == 0;
}

static BOOL HashFile(const wchar_t* path, wchar_t* hex, DWORD hexCount)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR hashObject = NULL;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD result = 0;
    BYTE digest[64];
    HANDLE file = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) goto cleanup;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objectSize,
                                         sizeof(objectSize), &result, 0))) goto cleanup;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashSize,
                                         sizeof(hashSize), &result, 0))) goto cleanup;
    if (objectSize == 0 || hashSize > sizeof(digest)) goto cleanup;

    hashObject = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, objectSize);
    if (!hashObject) goto cleanup;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, hashObject, objectSize, NULL, 0, 0))) goto cleanup;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                       NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;

    BYTE buffer[65536];
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) goto cleanup;
        if (read == 0) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0))) goto cleanup;
    }
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, hashSize, 0))) goto cleanup;

    for (DWORD i = 0; i < hashSize; ++i) {
        if (_snwprintf_s(hex + i * 2, hexCount - i * 2, _TRUNCATE, L"%02x", digest[i]) < 0) goto cleanup;
    }
    ok = TRUE;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (hashObject) HeapFree(GetProcessHeap(), 0, hashObject);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static BOOL GetMachineDir(wchar_t* out, DWORD count)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        DWORD bytes = count * sizeof(wchar_t);
        DWORD type = 0;
        LONG rc = RegQueryValueExW(key, L"MachineDir", NULL, &type,
                                   (BYTE*)out, &bytes);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && out[0]) {
            return TRUE;
        }
    }

    wchar_t programFiles[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\AppleMusicASIO", programFiles);
    return TRUE;
}

static void LoadAgentBridge(void)
{
    wchar_t dir[MAX_PATH];
    if (!GetMachineDir(dir, MAX_PATH)) {
        ShimLog(L"AMVerifierShim: machine install directory unavailable");
        return;
    }

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\AMAudioBridge.dll", dir);

    wchar_t actual[65];
    if (!HashFile(path, actual, 65) || _wcsicmp(actual, AMB_BRIDGE_SHA256) != 0) {
        wchar_t text[MAX_PATH + 96];
        _snwprintf_s(text, MAX_PATH + 96, _TRUNCATE,
                     L"AMVerifierShim: bridge hash mismatch or unreadable path=%ls", path);
        ShimLog(text);
        return;
    }

    HMODULE bridge = LoadLibraryW(path);
    if (!bridge) {
        wchar_t text[MAX_PATH + 80];
        _snwprintf_s(text, MAX_PATH + 80, _TRUNCATE,
                     L"AMVerifierShim: bridge load failed err=%lu path=%ls",
                     (unsigned long)GetLastError(), path);
        ShimLog(text);
        return;
    }

    wchar_t text[MAX_PATH + 48];
    _snwprintf_s(text, MAX_PATH + 48, _TRUNCATE,
                 L"AMVerifierShim: bridge loaded %ls", path);
    ShimLog(text);
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

    if (!IsProcessNamed(L"AMPLibraryAgent.exe")) {
        ShimLog(L"AMVerifierShim: not the library agent; skipped");
        InterlockedExchange(&g_started, 0);
        return 0;
    }

    LoadAgentBridge();
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

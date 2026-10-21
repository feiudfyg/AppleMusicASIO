#include "common.h"
#include <tlhelp32.h>
#include <shobjidl.h>

static const CLSID kClsidApplicationActivationManager =
    { 0x45BA127D, 0x10A8, 0x46EA, { 0x8A, 0xB7, 0x56, 0xEA, 0x90, 0x78, 0x94, 0x3C } };
static const IID kIidApplicationActivationManager =
    { 0x2E941141, 0x7F97, 0x4756, { 0xBA, 0x1D, 0x9D, 0xEC, 0xDE, 0x89, 0x4A, 0x3D } };

static void EnableDebugPrivilege(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return;
    }
    TOKEN_PRIVILEGES tp;
    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(token);
}

static DWORD FindProcessByName(const wchar_t* name, DWORD skip_pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != skip_pid && !_wcsicmp(pe.szExeFile, name)) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static BOOL IsModuleLoaded(DWORD pid, const wchar_t* module)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    MODULEENTRY32W me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);
    BOOL found = FALSE;
    if (Module32FirstW(snap, &me)) {
        do {
            if (!_wcsicmp(me.szModule, module)) {
                found = TRUE;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static BOOL g_force_inject = FALSE;

static BOOL IsBridgePresent(DWORD pid)
{
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE, L"Local\\AMAudioBridge_%lu", (unsigned long)pid);
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (event) {
        CloseHandle(event);
        return TRUE;
    }
    /* Older builds or a process that is still initialising. */
    return IsModuleLoaded(pid, L"AMAudioBridge.dll");
}

static DWORD64 RemoteModuleBase(DWORD pid, const wchar_t* module)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);
    DWORD64 base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (!_wcsicmp(me.szModule, module)) {
                base = (DWORD64)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

static BOOL IsTargetX64(HANDLE process)
{
    USHORT process_machine = 0, native_machine = 0;
    if (!IsWow64Process2(process, &process_machine, &native_machine)) {
        BOOL wow64 = FALSE;
        if (IsWow64Process(process, &wow64)) return !wow64;
        return TRUE;
    }
    return process_machine == IMAGE_FILE_MACHINE_UNKNOWN;
}

static BOOL InjectLibrary(DWORD pid, const wchar_t* dll_path)
{
    if (!g_force_inject && IsBridgePresent(pid)) {
        wprintf(L"[inject] bridge already loaded in pid %lu\n", pid);
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        wprintf(L"[inject] OpenProcess(%lu) failed: %lu (run elevated?)\n",
                pid, (unsigned long)GetLastError());
        return FALSE;
    }

    if (!IsTargetX64(process)) {
        wprintf(L"[inject] pid %lu is not x64\n", pid);
        CloseHandle(process);
        return FALSE;
    }

    HMODULE local_kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC local_load = GetProcAddress(local_kernel32, "LoadLibraryW");
    if (!local_load) {
        wprintf(L"[inject] cannot resolve LoadLibraryW\n");
        CloseHandle(process);
        return FALSE;
    }
    /* kernel32.dll is mapped at the same system-wide base in every process. */
    BYTE* remote_load = (BYTE*)local_load;

    SIZE_T path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(process, NULL, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        wprintf(L"[inject] VirtualAllocEx failed: %lu\n", (unsigned long)GetLastError());
        CloseHandle(process);
        return FALSE;
    }

    BOOL ok = FALSE;
    if (WriteProcessMemory(process, remote_path, dll_path, path_bytes, NULL)) {
        HANDLE thread = CreateRemoteThread(process, NULL, 0,
                                           (LPTHREAD_START_ROUTINE)remote_load,
                                           remote_path, 0, NULL);
        if (thread) {
            DWORD wait = WaitForSingleObject(thread, 15000);
            DWORD exit_code = 0;
            GetExitCodeThread(thread, &exit_code);
            CloseHandle(thread);
            wprintf(L"[inject] pid %lu LoadLibraryW -> 0x%lx (wait=%lu)\n",
                    pid, (unsigned long)exit_code, (unsigned long)wait);
            ok = exit_code != 0;
            if (!ok) {
                wprintf(L"[inject] injection blocked or DLL failed to load "
                        L"(code integrity guard?)\n");
            }
        } else {
            wprintf(L"[inject] CreateRemoteThread failed: %lu\n",
                    (unsigned long)GetLastError());
        }
    } else {
        wprintf(L"[inject] WriteProcessMemory failed: %lu\n", (unsigned long)GetLastError());
    }

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);

    if (ok) {
        Sleep(250);
        if (!IsBridgePresent(pid)) {
            wprintf(L"[inject] module list does not show AMAudioBridge.dll "
                    L"(check the bridge log)\n");
        }
    }
    return ok;
}

static DWORD ActivateAppleMusic(void)
{
    IApplicationActivationManager* manager = NULL;
    HRESULT hr = CoCreateInstance(&kClsidApplicationActivationManager, NULL,
                                  CLSCTX_LOCAL_SERVER, &kIidApplicationActivationManager,
                                  (void**)&manager);
    if (FAILED(hr) || !manager) {
        wprintf(L"[launch] ApplicationActivationManager failed: 0x%08lx\n", (unsigned long)hr);
        return 0;
    }

    DWORD pid = 0;
    hr = manager->lpVtbl->ActivateApplication(
        manager, L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!App", NULL, AO_NONE, &pid);
    manager->lpVtbl->Release(manager);
    if (FAILED(hr)) {
        wprintf(L"[launch] ActivateApplication failed: 0x%08lx\n", (unsigned long)hr);
        return 0;
    }
    wprintf(L"[launch] Apple Music activated, pid %lu\n", pid);
    return pid;
}

static void ListAsio(void)
{
    HKEY root = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        wprintf(L"no ASIO drivers registered\n");
        return;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t key[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, key, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY sub = NULL;
        if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &sub) != ERROR_SUCCESS) continue;
        wchar_t desc[256] = L"", clsid[128] = L"";
        DWORD type = 0, size = sizeof(desc);
        RegQueryValueExW(sub, L"Description", NULL, &type, (BYTE*)desc, &size);
        size = sizeof(clsid);
        RegQueryValueExW(sub, L"CLSID", NULL, &type, (BYTE*)clsid, &size);
        RegCloseKey(sub);
        wprintf(L"%-32ls %ls  %ls\n", key, desc, clsid);
    }
    RegCloseKey(root);
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* dll_path = NULL;
    const wchar_t* process_name = L"AppleMusic.exe";
    DWORD pid = 0;
    BOOL launch = FALSE;
    BOOL watch = FALSE;

    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--dll") && i + 1 < argc) dll_path = argv[++i];
        else if (!_wcsicmp(argv[i], L"--pid") && i + 1 < argc) pid = (DWORD)_wtoi(argv[++i]);
        else if (!_wcsicmp(argv[i], L"--name") && i + 1 < argc) process_name = argv[++i];
        else if (!_wcsicmp(argv[i], L"--launch")) launch = TRUE;
        else if (!_wcsicmp(argv[i], L"--watch")) watch = TRUE;
        else if (!_wcsicmp(argv[i], L"--force")) g_force_inject = TRUE;
        else if (!_wcsicmp(argv[i], L"--list-asio")) { ListAsio(); return 0; }
        else {
            wprintf(L"usage: AMAudioBridgeInjector [--dll path] [--pid n | --name exe] "
                    L"[--launch] [--watch] [--list-asio]\n");
            return 2;
        }
    }

    wchar_t default_dll[MAX_PATH];
    if (!dll_path) {
        GetModuleFileNameW(NULL, default_dll, MAX_PATH);
        wchar_t* slash = wcsrchr(default_dll, L'\\');
        if (slash) slash[1] = 0;
        wcscat_s(default_dll, MAX_PATH, L"AMAudioBridge.dll");
        dll_path = default_dll;
    }

    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"bridge DLL not found: %ls\n", dll_path);
        return 1;
    }

    wchar_t full_dll[MAX_PATH];
    DWORD full_len = GetFullPathNameW(dll_path, MAX_PATH, full_dll, NULL);
    if (full_len == 0 || full_len >= MAX_PATH) {
        wprintf(L"cannot resolve bridge DLL path: %ls\n", dll_path);
        return 1;
    }
    dll_path = full_dll;

    EnableDebugPrivilege();
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        wprintf(L"CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    if (!pid) pid = FindProcessByName(process_name, GetCurrentProcessId());
    if (!pid && launch) {
        DWORD activated = ActivateAppleMusic();
        if (activated) {
            pid = activated;
        } else {
            for (int i = 0; i < 100 && !pid; ++i) {
                Sleep(100);
                pid = FindProcessByName(process_name, GetCurrentProcessId());
            }
        }
    }
    if (!pid && !watch) {
        wprintf(L"Apple Music is not running. Start it, or use --launch.\n");
        return 1;
    }

    if (watch) {
        wprintf(L"watching for %ls...\n", process_name);
        for (;;) {
            DWORD p = FindProcessByName(process_name, GetCurrentProcessId());
            if (p && !IsBridgePresent(p)) {
                InjectLibrary(p, dll_path);
            }
            Sleep(100);
        }
    }

    BOOL ok = InjectLibrary(pid, dll_path);
    CoUninitialize();
    return ok ? 0 : 1;
}

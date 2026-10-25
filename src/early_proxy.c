#include "common.h"
#include "log.h"

#include <tlhelp32.h>

typedef HRESULT(STDAPICALLTYPE* PFN_DllGetClassObject)(REFCLSID, REFIID, void**);

static const CLSID kAppleMusicLibrary = {
    0x68e7097c, 0xf969, 0x4006,
    { 0xaa, 0xc3, 0x95, 0x11, 0x5f, 0x0e, 0xd1, 0xc4 }
};

static HMODULE g_module;
static INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_propsys_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_propsys;
static LONG g_prestart_started;
static LONG g_prestart_running;

static BOOL CALLBACK InitLogOnce(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    AMB_UNUSED(once);
    AMB_UNUSED(parameter);
    AMB_UNUSED(context);
    Log_Init(g_module);
    Log_SetLevel(2);
    LOGI("AMEarlyProxy loaded");
    return TRUE;
}

static void EnsureLog(void)
{
    InitOnceExecuteOnce(&g_log_once, InitLogOnce, NULL, NULL);
}

static BOOL CALLBACK LoadPropsysOnce(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    AMB_UNUSED(once);
    AMB_UNUSED(parameter);
    AMB_UNUSED(context);

    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\propsys.dll", dir);
    g_propsys = LoadLibraryExW(path, NULL, 0);
    LOGI("AMEarlyProxy: LoadLibrary propsys=%p", g_propsys);
    return TRUE;
}

static DWORD FindProcessByName(const wchar_t* name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

static BOOL InjectBridge(DWORD pid, const wchar_t* dll)
{
    if (!pid || !dll || !*dll) return FALSE;

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        LOGW("AMEarlyProxy: OpenProcess pid=%lu failed=%lu", pid, GetLastError());
        return FALSE;
    }

    FARPROC load_library = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    BOOL ok = FALSE;
    if (load_library && remote &&
        WriteProcessMemory(process, remote, dll, bytes, NULL)) {
        HANDLE thread = CreateRemoteThread(process, NULL, 0,
                                           (LPTHREAD_START_ROUTINE)load_library,
                                           remote, 0, NULL);
        if (thread) {
            DWORD wait = WaitForSingleObject(thread, 15000);
            DWORD exit_code = 0;
            GetExitCodeThread(thread, &exit_code);
            CloseHandle(thread);
            ok = wait == WAIT_OBJECT_0 && exit_code != 0;
            LOGI("AMEarlyProxy: inject pid=%lu wait=%lu exit=%lu ok=%d",
                 pid, wait, exit_code, ok ? 1 : 0);
        } else {
            LOGW("AMEarlyProxy: CreateRemoteThread failed=%lu", GetLastError());
        }
    } else {
        LOGW("AMEarlyProxy: injection setup failed=%lu", GetLastError());
    }

    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

static BOOL BridgePath(wchar_t* out, DWORD count)
{
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\AppleMusicASIO\\AMAudioBridge.dll", local);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI PrestartThread(void* parameter)
{
    AMB_UNUSED(parameter);
    LOGI("AMEarlyProxy: prestart thread start");

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    LOGI("AMEarlyProxy: CoInitializeEx hr=0x%08lx", hr);
    if (FAILED(hr)) {
        InterlockedExchange(&g_prestart_running, 0);
        return 0;
    }

    IUnknown* library = NULL;
    hr = CoCreateInstance(&kAppleMusicLibrary, NULL, CLSCTX_LOCAL_SERVER,
                          &IID_IUnknown, (void**)&library);
    LOGI("AMEarlyProxy: CoCreateInstance hr=0x%08lx ptr=%p", hr, library);

    DWORD pid = 0;
    for (int i = 0; i < 100; ++i) {
        pid = FindProcessByName(L"AMPLibraryAgent.exe");
        if (pid) break;
        Sleep(100);
    }
    LOGI("AMEarlyProxy: agent pid=%lu", pid);

    wchar_t bridge[MAX_PATH];
    if (pid && BridgePath(bridge, MAX_PATH)) {
        InjectBridge(pid, bridge);
    } else {
        LOGW("AMEarlyProxy: bridge path unavailable");
    }

    if (library) library->lpVtbl->Release(library);
    CoUninitialize();
    LOGI("AMEarlyProxy: prestart thread exit");
    InterlockedExchange(&g_prestart_running, 0);
    return 0;
}

static void StartPrestartIfNeeded(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;

    const wchar_t* base = wcsrchr(exe, L'\\');
    base = base ? base + 1 : exe;
    if (_wcsicmp(base, L"AppleMusic.exe") != 0) return;
    if (InterlockedCompareExchange(&g_prestart_started, 1, 0) != 0) return;

    InterlockedExchange(&g_prestart_running, 1);
    LOGI("AMEarlyProxy: AppleMusic trigger");
    HANDLE thread = CreateThread(NULL, 0, PrestartThread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_prestart_running, 0);
    }
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    EnsureLog();
    StartPrestartIfNeeded();
    InitOnceExecuteOnce(&g_propsys_once, LoadPropsysOnce, NULL, NULL);
    if (!g_propsys) return CLASS_E_CLASSNOTAVAILABLE;

    PFN_DllGetClassObject get_class =
        (PFN_DllGetClassObject)GetProcAddress(g_propsys, "DllGetClassObject");
    return get_class ? get_class(clsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void)
{
    if (InterlockedCompareExchange(&g_prestart_running, 0, 0) != 0) return S_FALSE;
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    AMB_UNUSED(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = (HMODULE)instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

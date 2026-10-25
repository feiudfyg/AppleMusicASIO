#include "common.h"
#include "log.h"
#include "config.h"
#include "hooks.h"
#include "app_hooks.h"
#include "setup_common.h"

static HMODULE g_module = NULL;
static HANDLE g_instance_event = NULL;
static HANDLE g_init_done = NULL;
static volatile LONG g_init_thread_id = 0;
static BOOL g_target_process = FALSE;

static BOOL ProcessNameIs(const wchar_t* wanted)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return !_wcsicmp(name, wanted);
}

static BOOL IsBridgeTarget(void)
{
    return ProcessNameIs(L"AMPLibraryAgent.exe") || ProcessNameIs(L"AMAudioProbe.exe");
}

static void LoadPersistHelper(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = 0;
    wcscat_s(path, MAX_PATH, L"AMPrefsPersist.dll");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        LoadLibraryW(path);
    }
}

static DWORD WINAPI BridgeInitThread(void* param)
{
    AMB_UNUSED(param);
    InterlockedExchange(&g_init_thread_id, (LONG)GetCurrentThreadId());
    Log_Init(g_module);
    Config_Load();

    wchar_t event_name[64];
    _snwprintf_s(event_name, 64, _TRUNCATE, L"Local\\AMAudioBridge_%lu",
                 (unsigned long)GetCurrentProcessId());
    g_instance_event = CreateEventW(NULL, TRUE, FALSE, event_name);

    wchar_t junction[512];
    if (Setup_EnsurePreferencesJunction(FALSE, junction, 512)) {
        LOGI("preferences: %ls", junction);
    } else {
        LOGW("preferences: %ls", junction);
    }

    if (g_cfg.mode == AMB_MODE_OFF) {
        LOGI("bridge disabled by configuration");
        if (g_init_done) SetEvent(g_init_done);
        return 0;
    }

    LoadPersistHelper();

    if (!Hooks_Install()) {
        LOGE("hook installation failed");
        if (g_init_done) SetEvent(g_init_done);
        return 0;
    }

    AppHooks_Start(g_module);

    LOGI("bridge ready (%u.%u.%u)",
         AMB_VERSION_MAJOR, AMB_VERSION_MINOR, AMB_VERSION_PATCH);
    if (g_init_done) SetEvent(g_init_done);
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    AMB_UNUSED(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        g_target_process = IsBridgeTarget();
        if (g_target_process) {
            g_init_done = CreateEventW(NULL, TRUE, FALSE, NULL);
            HANDLE thread = CreateThread(NULL, 0, BridgeInitThread, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
        }
    }
    return TRUE;
}

static HRESULT ForwardClassObject(REFCLSID clsid, REFIID iid, void** ppv)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n + 16 >= MAX_PATH) return REGDB_E_CLASSNOTREG;
    wcscat_s(path, MAX_PATH, L"\\MMDevApi.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return REGDB_E_CLASSNOTREG;
    typedef HRESULT (WINAPI* PFN_GET_CLASS)(REFCLSID, REFIID, void**);
    PFN_GET_CLASS fn = (PFN_GET_CLASS)GetProcAddress(real, "DllGetClassObject");
    if (!fn) return REGDB_E_CLASSNOTREG;
    return fn(clsid, iid, ppv);
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** ppv)
{
    if (g_target_process && g_init_done &&
        (DWORD)InterlockedCompareExchange(&g_init_thread_id, 0, 0) != GetCurrentThreadId()) {
        WaitForSingleObject(g_init_done, 5000);
    }
    return ForwardClassObject(clsid, iid, ppv);
}

STDAPI DllCanUnloadNow(void)
{
    return g_target_process ? S_FALSE : S_OK;
}

__declspec(dllexport) DWORD AMAudioBridge_GetVersion(void)
{
    return ((DWORD)AMB_VERSION_MAJOR << 16) |
           ((DWORD)AMB_VERSION_MINOR << 8) |
           (DWORD)AMB_VERSION_PATCH;
}

__declspec(dllexport) BOOL AMAudioBridge_Ping(void)
{
    return TRUE;
}

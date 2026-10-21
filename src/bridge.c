#include "common.h"
#include "log.h"
#include "config.h"
#include "hooks.h"
#include "app_hooks.h"

static HMODULE g_module = NULL;
static HANDLE g_instance_event = NULL;

static DWORD WINAPI BridgeInitThread(void* param)
{
    AMB_UNUSED(param);
    Log_Init(g_module);
    Config_Load();

    wchar_t event_name[64];
    _snwprintf_s(event_name, 64, _TRUNCATE, L"Local\\AMAudioBridge_%lu",
                 (unsigned long)GetCurrentProcessId());
    g_instance_event = CreateEventW(NULL, TRUE, FALSE, event_name);

    if (g_cfg.mode == AMB_MODE_OFF) {
        LOGI("bridge disabled by configuration");
        return 0;
    }

    if (!Hooks_Install()) {
        LOGE("hook installation failed");
        return 0;
    }

    AppHooks_Start(g_module);

    LOGI("bridge ready (%u.%u.%u)",
         AMB_VERSION_MAJOR, AMB_VERSION_MINOR, AMB_VERSION_PATCH);
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    AMB_UNUSED(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(NULL, 0, BridgeInitThread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
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

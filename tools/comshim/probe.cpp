#include "comshim_common.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        ComLog("AMExeProbe loaded");
        wchar_t eventName[128];
        _snwprintf_s(eventName, ARRAYSIZE(eventName), _TRUNCATE,
                     L"Local\\AMExeProbe_%lu", GetCurrentProcessId());
        HANDLE event = CreateEventW(nullptr, TRUE, TRUE, eventName);
        if (event) CloseHandle(event);
    }
    return TRUE;
}

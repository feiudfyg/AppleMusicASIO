#include "comshim_common.h"
#include <appmodel.h>

static const wchar_t kAgentPath[] =
    L"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa\\AMPLibraryAgent.exe";

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("AgentStartProbe start");
    CoLogPackageIdentity();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("AgentStartProbe CoInitializeEx hr=0x%08lx", hr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    wchar_t commandLine[] =
        L"\"C:\\Program Files\\WindowsApps\\AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa\\AMPLibraryAgent.exe\" -Embedding";
    BOOL started = CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE,
                                  0, nullptr, nullptr, &startup, &process);
    ComLog("AgentStartProbe CreateProcess started=%d error=%lu pid=%lu",
           started ? 1 : 0, GetLastError(), process.dwProcessId);
    if (started) {
        DWORD wait = WaitForSingleObject(process.hProcess, 3000);
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        ComLog("AgentStartProbe process wait=%lu exit=%lu", wait, exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }

    Sleep(1500);
    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIOAgentStartProbe");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("AgentStartProbe recursive activation blocked");
        CloseHandle(guard);
        return 0;
    }
    IUnknown* real = nullptr;
    hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IUnknown, reinterpret_cast<void**>(&real));
    ComLog("AgentStartProbe CoCreateInstance hr=0x%08lx ptr=%p", hr, real);
    if (real) real->Release();
    if (guard) CloseHandle(guard);
    Sleep(5000);
    ComLog("AgentStartProbe exit");
    return 0;
}

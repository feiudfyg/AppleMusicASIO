#include "comshim_common.h"

static const wchar_t kServerKey[] =
    L"Software\\Classes\\PackagedCom\\Package\\"
    L"AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa"
    L"\\Server\\0";

static bool WriteOriginal(HKEY key)
{
    const wchar_t exe[] = L"AMPLibraryAgent.exe";
    DWORD system = 0;
    LONG exeRc = RegSetValueExW(key, L"Executable", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(exe), sizeof(exe));
    LONG sysRc = RegSetValueExW(key, L"IsSystemExecutable", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&system), sizeof(system));
    ComLog("prestart shim set original exeRc=%ld sysRc=%ld", exeRc, sysRc);
    return exeRc == ERROR_SUCCESS && sysRc == ERROR_SUCCESS;
}

static std::wstring BridgePath()
{
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    if (n == 0 || n >= ARRAYSIZE(local)) return {};
    return std::wstring(local) + L"\\AppleMusicASIO\\AMAudioBridge.dll";
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("PreStartShim start");
    CoLogPackageIdentity();

    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIOPreStartShim");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("PreStartShim recursive activation blocked");
        CloseHandle(guard);
        return 0;
    }

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServerKey, 0,
                            KEY_READ | KEY_SET_VALUE, &key);
    ComLog("prestart shim open key rc=%ld", rc);
    if (rc != ERROR_SUCCESS) {
        if (guard) CloseHandle(guard);
        return 1;
    }
    if (!WriteOriginal(key)) {
        RegCloseKey(key);
        if (guard) CloseHandle(guard);
        return 2;
    }

    std::wstring exe = ModulePath();
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring agent = (slash == std::wstring::npos)
        ? L"AMPLibraryAgent.exe"
        : exe.substr(0, slash + 1) + L"AMPLibraryAgent.exe";
    std::wstring command = L"\"" + agent + L"\" -Embedding";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(agent.c_str(), command.data(), nullptr, nullptr,
                                  FALSE, 0, nullptr, nullptr, &si, &pi);
    ComLog("prestart shim CreateProcess agent=%ls created=%d err=%lu",
           agent.c_str(), created, GetLastError());
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);

    DWORD pid = 0;
    for (int i = 0; i < 100; ++i) {
        pid = FindProcessByName(L"AMPLibraryAgent.exe");
        if (pid) break;
        Sleep(100);
    }
    ComLog("prestart shim agent pid=%lu", pid);

    std::wstring dll = BridgePath();
    bool injected = pid && !dll.empty() && InjectDll(pid, dll);
    ComLog("prestart shim inject pid=%lu ok=%d path=%ls",
           pid, injected ? 1 : 0, dll.c_str());

    Sleep(3000);
    RegCloseKey(key);
    if (guard) CloseHandle(guard);
    ComLog("PreStartShim exit injected=%d", injected ? 1 : 0);
    return injected ? 0 : 3;
}

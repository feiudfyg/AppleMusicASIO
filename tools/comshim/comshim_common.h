#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// Test-only CLSIDs. The real test server registers both; the proxy CLSID is
// used by the client, the real CLSID is what the local-server shim activates.
static const CLSID kTestProxy = {
    0x7a6c0b2e, 0x3f4a, 0x4b7c, { 0x9d, 0x1e, 0x2a, 0x5b, 0x8c, 0x6d, 0x0e, 0x11 }
};
static const CLSID kTestReal = {
    0x7a6c0b2e, 0x3f4a, 0x4b7c, { 0x9d, 0x1e, 0x2a, 0x5b, 0x8c, 0x6d, 0x0e, 0x12 }
};

// Apple's packaged ExeServer: AMP.Core.IAMPMusicLibrary / AMPLibraryAgent.exe.
static const CLSID kAppleMusicLibrary = {
    0x68e7097c, 0xf969, 0x4006, { 0xaa, 0xc3, 0x95, 0x11, 0x5f, 0x0e, 0xd1, 0xc4 }
};

inline constexpr wchar_t kLogPath[] = L"C:\\Users\\Public\\AMComTest.log";

inline void ComLog(const char* format, ...)
{
    char message[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);

    wchar_t exe[MAX_PATH] = L"?";
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1400];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%02u:%02u:%02u.%03u pid=%lu tid=%lu [%ls] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        GetCurrentProcessId(), GetCurrentThreadId(), exe, message);
    if (n <= 0) return;

    HANDLE file = CreateFileW(kLogPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, (DWORD)n, &written, nullptr);
    CloseHandle(file);
}

inline void CoLogPackageIdentity()
{
    UINT32 length = 0;
    LONG rc = GetCurrentPackageFullName(&length, nullptr);
    ComLog("package identity probe rc=%ld length=%lu", rc, length);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0 || length > 1024) return;
    wchar_t fullName[1024]{};
    rc = GetCurrentPackageFullName(&length, fullName);
    ComLog("package identity rc=%ld name=%ls", rc, fullName);
}

inline std::wstring ModulePath()
{
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return {};
    return path;
}

inline std::wstring ModuleDirectory()
{
    std::wstring path = ModulePath();
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash);
}

inline DWORD FindProcessByName(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

inline bool InjectDll(DWORD pid, const std::wstring& dllPath)
{
    if (!pid || dllPath.empty()) return false;
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        ComLog("inject OpenProcess pid=%lu failed=%lu", pid, GetLastError());
        return false;
    }

    FARPROC loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    bool ok = false;
    if (loadLibrary && remote &&
        WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                           reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
                                           remote, 0, nullptr);
        if (thread) {
            DWORD wait = WaitForSingleObject(thread, 15000);
            DWORD exitCode = 0;
            GetExitCodeThread(thread, &exitCode);
            CloseHandle(thread);
            ok = wait == WAIT_OBJECT_0 && exitCode != 0;
            ComLog("inject pid=%lu LoadLibraryW wait=%lu exit=%lu ok=%d",
                   pid, wait, exitCode, ok ? 1 : 0);
        } else {
            ComLog("inject CreateRemoteThread pid=%lu failed=%lu", pid, GetLastError());
        }
    } else {
        ComLog("inject setup pid=%lu failed=%lu", pid, GetLastError());
    }

    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

#include "comshim_common.h"
#include <appmodel.h>

static const wchar_t kPackageName[] =
    L"AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa";
static const wchar_t kPrivateHklm[] = L"Software\\AppleMusicASIO\\FakeHKLM";
static const wchar_t kPrivateHkcr[] = L"Software\\AppleMusicASIO\\FakeHKCR";

static bool CopyKey(HKEY parent, const wchar_t* sourceSubKey,
                    const wchar_t* destinationSubKey)
{
    HKEY source = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sourceSubKey, 0, KEY_READ, &source);
    if (rc != ERROR_SUCCESS) {
        ComLog("reg override open source %ls failed=%ld", sourceSubKey, rc);
        return false;
    }
    HKEY destination = nullptr;
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, destinationSubKey, 0, nullptr, 0,
                         KEY_WRITE, nullptr, &destination, nullptr);
    if (rc != ERROR_SUCCESS) {
        ComLog("reg override create dest %ls failed=%ld", destinationSubKey, rc);
        RegCloseKey(source);
        return false;
    }
    rc = RegCopyTreeW(source, nullptr, destination);
    ComLog("reg override copy %ls -> %ls rc=%ld", sourceSubKey, destinationSubKey, rc);
    RegCloseKey(destination);
    RegCloseKey(source);
    return rc == ERROR_SUCCESS;
}

static bool BuildFakeHklm()
{
    HKEY root = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kPrivateHklm, 0, nullptr, 0,
                              KEY_READ | KEY_WRITE, nullptr, &root, nullptr);
    if (rc != ERROR_SUCCESS) {
        ComLog("reg override create fake root failed=%ld", rc);
        return false;
    }
    RegCloseKey(root);

    std::wstring packageSource =
        std::wstring(L"Software\\Classes\\PackagedCom\\Package\\") + kPackageName;
    std::wstring packageDest =
        std::wstring(kPrivateHklm) + L"\\Software\\Classes\\PackagedCom\\Package\\" + kPackageName;
    std::wstring classIndexSource =
        L"Software\\Classes\\PackagedCom\\ClassIndex\\{68E7097C-F969-4006-AAC3-95115F0ED1C4}";
    std::wstring classIndexDest =
        std::wstring(kPrivateHklm) +
        L"\\Software\\Classes\\PackagedCom\\ClassIndex\\{68E7097C-F969-4006-AAC3-95115F0ED1C4}";

    if (!CopyKey(HKEY_LOCAL_MACHINE, packageSource.c_str(), packageDest.c_str())) return false;
    if (!CopyKey(HKEY_LOCAL_MACHINE, classIndexSource.c_str(), classIndexDest.c_str())) return false;

    std::wstring serverDest = packageDest + L"\\Server\\0";
    HKEY server = nullptr;
    rc = RegOpenKeyExW(HKEY_CURRENT_USER, serverDest.c_str(), 0, KEY_WRITE, &server);
    if (rc != ERROR_SUCCESS) {
        ComLog("reg override open fake server failed=%ld", rc);
        return false;
    }
    const wchar_t original[] = L"AMPLibraryAgent.exe";
    DWORD originalSystem = 0;
    LONG exeRc = RegSetValueExW(server, L"Executable", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(original), sizeof(original));
    LONG sysRc = RegSetValueExW(server, L"IsSystemExecutable", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&originalSystem),
                                sizeof(originalSystem));
    RegCloseKey(server);
    ComLog("reg override fake server exeRc=%ld sysRc=%ld", exeRc, sysRc);
    return exeRc == ERROR_SUCCESS && sysRc == ERROR_SUCCESS;
}

static void LogHkcrServerExecutable(const wchar_t* label)
{
    const wchar_t path[] =
        L"PackagedCom\\Package\\"
        L"AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa"
        L"\\Server\\0";
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        ComLog("%ls HKCR server open failed=%ld", label, rc);
        return;
    }
    wchar_t value[1024]{};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    rc = RegQueryValueExW(key, L"Executable", nullptr, &type,
                          reinterpret_cast<BYTE*>(value), &bytes);
    ComLog("%ls HKCR server executable rc=%ld value=%ls", label, rc, value);
    RegCloseKey(key);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("RegOverrideProbe start");
    CoLogPackageIdentity();

    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIORegOverrideProbe");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("RegOverrideProbe recursive activation blocked");
        CloseHandle(guard);
        return 0;
    }

    HKEY fakeRoot = nullptr;
    HKEY fakeHkcr = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPrivateHklm, 0, KEY_READ, &fakeRoot);
    LONG rcHkcr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPrivateHkcr, 0, KEY_READ, &fakeHkcr);
    LONG overrideRc = ERROR_INVALID_HANDLE;
    LONG overrideHkcrRc = ERROR_INVALID_HANDLE;
    if (rc == ERROR_SUCCESS) {
        overrideRc = RegOverridePredefKey(HKEY_LOCAL_MACHINE, fakeRoot);
        ComLog("RegOverridePredefKey(HKLM) rc=%ld", overrideRc);
    } else {
        ComLog("RegOverrideProbe open prebuilt fake HKLM failed=%ld", rc);
    }
    if (rcHkcr == ERROR_SUCCESS) {
        overrideHkcrRc = RegOverridePredefKey(HKEY_CLASSES_ROOT, fakeHkcr);
        ComLog("RegOverridePredefKey(HKCR) rc=%ld", overrideHkcrRc);
    } else {
        ComLog("RegOverrideProbe open prebuilt fake HKCR failed=%ld", rcHkcr);
    }
    LogHkcrServerExecutable(L"before CoInitialize");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("RegOverrideProbe CoInitializeEx hr=0x%08lx", hr);
    LogHkcrServerExecutable(L"after CoInitialize");

    IUnknown* real = nullptr;
    if (overrideRc == ERROR_SUCCESS && overrideHkcrRc == ERROR_SUCCESS) {
        hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                              IID_IUnknown, reinterpret_cast<void**>(&real));
        ComLog("RegOverrideProbe CoCreateInstance hr=0x%08lx ptr=%p", hr, real);
        RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    }
    if (real) real->Release();
    if (fakeHkcr) RegCloseKey(fakeHkcr);
    if (fakeRoot) RegCloseKey(fakeRoot);
    if (guard) CloseHandle(guard);
    Sleep(5000);
    ComLog("RegOverrideProbe exit");
    return 0;
}

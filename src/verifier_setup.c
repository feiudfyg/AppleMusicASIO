#include <windows.h>
#include <shlobj.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <wchar.h>
#include <fcntl.h>
#include <io.h>

#define SHIM_NAME L"AMVerifierShim.dll"
#define MACHINE_KEY L"Software\\AppleMusicASIO\\Machine"
#define IFEO_KEY L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\AppleMusic.exe"
#define APP_VERIFIER_FLAG 0x100u

static void Print(const wchar_t* text)
{
    wprintf(L"%ls\n", text);
}

static void PrintError(const wchar_t* action, DWORD error)
{
    wprintf(L"%ls failed (error %lu)\n", action, (unsigned long)error);
}

static BOOL IsElevated(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    BOOL elevated = FALSE;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

static BOOL GetProgramFiles(wchar_t* path, DWORD count)
{
    return SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES | CSIDL_FLAG_CREATE,
                                      NULL, SHGFP_TYPE_CURRENT, path)) &&
           path[0] != 0 && wcslen(path) < count;
}

static BOOL GetSystem32(wchar_t* path, DWORD count)
{
    UINT n = GetSystemDirectoryW(path, count);
    return n > 0 && n < count;
}

static BOOL JoinPath(wchar_t* out, DWORD count, const wchar_t* left, const wchar_t* right)
{
    if (!left || !right) return FALSE;
    int n = _snwprintf_s(out, count, _TRUNCATE, L"%ls\\%ls", left, right);
    return n >= 0 && (DWORD)n < count;
}

static BOOL EnsureDirectory(const wchar_t* path)
{
    if (CreateDirectoryW(path, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL SetProtectedAcl(const wchar_t* path, BOOL directory)
{
    PSID users = NULL;
    PSID admins = NULL;
    PSID system = NULL;
    PACL acl = NULL;
    BYTE usersBuf[SECURITY_MAX_SID_SIZE];
    BYTE adminsBuf[SECURITY_MAX_SID_SIZE];
    BYTE systemBuf[SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    BOOL ok = FALSE;

    users = (PSID)usersBuf;
    admins = (PSID)adminsBuf;
    system = (PSID)systemBuf;
    size = sizeof(usersBuf);
    if (!CreateWellKnownSid(WinBuiltinUsersSid, NULL, users, &size)) goto cleanup;
    size = sizeof(adminsBuf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins, &size)) goto cleanup;
    size = sizeof(systemBuf);
    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, system, &size)) goto cleanup;

    EXPLICIT_ACCESSW entries[3];
    ZeroMemory(entries, sizeof(entries));

    DWORD inheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;

    entries[0].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    entries[0].grfAccessMode = SET_ACCESS;
    entries[0].grfInheritance = inheritance;
    entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    entries[0].Trustee.ptstrName = (LPWSTR)users;

    entries[1].grfAccessPermissions = GENERIC_ALL;
    entries[1].grfAccessMode = SET_ACCESS;
    entries[1].grfInheritance = inheritance;
    entries[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    entries[1].Trustee.ptstrName = (LPWSTR)admins;

    entries[2].grfAccessPermissions = GENERIC_ALL;
    entries[2].grfAccessMode = SET_ACCESS;
    entries[2].grfInheritance = inheritance;
    entries[2].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entries[2].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entries[2].Trustee.ptstrName = (LPWSTR)system;

    if (SetEntriesInAclW(3, entries, NULL, &acl) != ERROR_SUCCESS) goto cleanup;
    ok = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                               NULL, NULL, acl, NULL) == ERROR_SUCCESS;

cleanup:
    if (acl) LocalFree(acl);
    return ok;
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

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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

static BOOL SameFile(const wchar_t* a, const wchar_t* b)
{
    HANDLE left = CreateFileW(a, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
                              FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (left == INVALID_HANDLE_VALUE) return FALSE;
    HANDLE right = CreateFileW(b, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (right == INVALID_HANDLE_VALUE) {
        CloseHandle(left);
        return FALSE;
    }

    BY_HANDLE_FILE_INFORMATION li, ri;
    BOOL ok = GetFileInformationByHandle(left, &li) && GetFileInformationByHandle(right, &ri) &&
              li.dwVolumeSerialNumber == ri.dwVolumeSerialNumber &&
              li.nFileIndexHigh == ri.nFileIndexHigh &&
              li.nFileIndexLow == ri.nFileIndexLow;
    CloseHandle(right);
    CloseHandle(left);
    return ok;
}

static LONG ReadStringValue(HKEY key, const wchar_t* name, wchar_t* out, DWORD count,
                            DWORD* type, BOOL* present)
{
    DWORD bytes = count * sizeof(wchar_t);
    DWORD valueType = 0;
    LONG rc = RegQueryValueExW(key, name, NULL, &valueType, (BYTE*)out, &bytes);
    if (rc == ERROR_SUCCESS) {
        out[count - 1] = 0;
        if (type) *type = valueType;
        if (present) *present = TRUE;
        return ERROR_SUCCESS;
    }
    if (rc == ERROR_FILE_NOT_FOUND) {
        out[0] = 0;
        if (type) *type = 0;
        if (present) *present = FALSE;
        return ERROR_SUCCESS;
    }
    return rc;
}

static LONG ReadDwordValue(HKEY key, const wchar_t* name, DWORD* value, BOOL* present)
{
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    LONG rc = RegQueryValueExW(key, name, NULL, &type, (BYTE*)value, &bytes);
    if (rc == ERROR_SUCCESS && type == REG_DWORD) {
        if (present) *present = TRUE;
        return ERROR_SUCCESS;
    }
    if (rc == ERROR_FILE_NOT_FOUND) {
        *value = 0;
        if (present) *present = FALSE;
        return ERROR_SUCCESS;
    }
    return rc;
}

static BOOL WriteStringValue(HKEY key, const wchar_t* name, const wchar_t* value)
{
    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, bytes) == ERROR_SUCCESS;
}

static BOOL WriteDwordValue(HKEY key, const wchar_t* name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
}

static BOOL BackupIfeo(HKEY ifeo, HKEY machine, BOOL existingInstall)
{
    if (existingInstall) return TRUE;

    wchar_t verifier[1024];
    DWORD verifierType = 0;
    BOOL hadVerifier = FALSE;
    if (ReadStringValue(ifeo, L"VerifierDlls", verifier, 1024, &verifierType, &hadVerifier) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (hadVerifier && verifier[0] && _wcsicmp(verifier, SHIM_NAME) != 0) {
        Print(L"Refusing to install: another VerifierDlls value exists.");
        return FALSE;
    }
    if (hadVerifier && verifierType != REG_SZ) {
        Print(L"Refusing to install: VerifierDlls is not REG_SZ.");
        return FALSE;
    }

    DWORD globalFlag = 0;
    BOOL hadGlobalFlag = FALSE;
    if (ReadDwordValue(ifeo, L"GlobalFlag", &globalFlag, &hadGlobalFlag) != ERROR_SUCCESS) {
        return FALSE;
    }

    if (!WriteDwordValue(machine, L"HadVerifierDlls", hadVerifier ? 1 : 0)) return FALSE;
    if (hadVerifier && !WriteStringValue(machine, L"OriginalVerifierDlls", verifier)) return FALSE;
    if (!WriteDwordValue(machine, L"HadGlobalFlag", hadGlobalFlag ? 1 : 0)) return FALSE;
    if (hadGlobalFlag && !WriteDwordValue(machine, L"OriginalGlobalFlag", globalFlag)) return FALSE;
    return TRUE;
}

static BOOL RestoreIfeo(HKEY ifeo, HKEY machine)
{
    DWORD hadVerifier = 0;
    DWORD hadGlobal = 0;
    if (ReadDwordValue(machine, L"HadVerifierDlls", &hadVerifier, NULL) != ERROR_SUCCESS) return FALSE;
    if (ReadDwordValue(machine, L"HadGlobalFlag", &hadGlobal, NULL) != ERROR_SUCCESS) return FALSE;

    if (hadVerifier) {
        wchar_t original[1024];
        if (ReadStringValue(machine, L"OriginalVerifierDlls", original, 1024, NULL, NULL) != ERROR_SUCCESS) {
            return FALSE;
        }
        if (!WriteStringValue(ifeo, L"VerifierDlls", original)) return FALSE;
    } else {
        LONG rc = RegDeleteValueW(ifeo, L"VerifierDlls");
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) return FALSE;
    }

    if (hadGlobal) {
        DWORD original = 0;
        if (ReadDwordValue(machine, L"OriginalGlobalFlag", &original, NULL) != ERROR_SUCCESS) return FALSE;
        if (!WriteDwordValue(ifeo, L"GlobalFlag", original)) return FALSE;
    } else {
        LONG rc = RegDeleteValueW(ifeo, L"GlobalFlag");
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) return FALSE;
    }
    return TRUE;
}

static BOOL CheckIfeoConflict(void)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, IFEO_KEY, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return TRUE;
    }

    wchar_t value[1024];
    DWORD type = 0;
    BOOL present = FALSE;
    LONG rc = ReadStringValue(key, L"VerifierDlls", value, 1024, &type, &present);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        Print(L"Could not read the existing VerifierDlls value.");
        return FALSE;
    }
    if (present && value[0] && _wcsicmp(value, SHIM_NAME) != 0) {
        Print(L"Refusing to install: another VerifierDlls value exists.");
        return FALSE;
    }
    if (present && type != REG_SZ) {
        Print(L"Refusing to install: VerifierDlls is not REG_SZ.");
        return FALSE;
    }
    return TRUE;
}

static BOOL InstallShim(const wchar_t* sourceDir)
{
    if (!CheckIfeoConflict()) return FALSE;

    wchar_t source[MAX_PATH];
    if (!JoinPath(source, MAX_PATH, sourceDir, SHIM_NAME)) return FALSE;
    if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
        Print(L"AMVerifierShim.dll was not found in the source directory.");
        return FALSE;
    }

    wchar_t programFiles[MAX_PATH];
    if (!GetProgramFiles(programFiles, MAX_PATH)) return FALSE;
    wchar_t machineDir[MAX_PATH];
    if (!JoinPath(machineDir, MAX_PATH, programFiles, L"AppleMusicASIO")) return FALSE;
    if (!EnsureDirectory(machineDir)) return FALSE;
    if (!SetProtectedAcl(machineDir, TRUE)) {
        Print(L"Could not set the protected ACL on the machine directory.");
        return FALSE;
    }

    wchar_t target[MAX_PATH];
    if (!JoinPath(target, MAX_PATH, machineDir, SHIM_NAME)) return FALSE;
    if (!CopyFileW(source, target, FALSE)) {
        if (GetLastError() != ERROR_FILE_EXISTS &&
            !(GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES && SameFile(source, target))) {
            PrintError(L"CopyFileW", GetLastError());
            return FALSE;
        }
    }

    if (!SetProtectedAcl(target, FALSE)) {
        Print(L"Could not set the protected ACL on AMVerifierShim.dll.");
        return FALSE;
    }

    wchar_t hash[65];
    if (!HashFile(target, hash, 65)) {
        Print(L"Could not hash AMVerifierShim.dll.");
        return FALSE;
    }

    wchar_t system32[MAX_PATH];
    if (!GetSystem32(system32, MAX_PATH)) return FALSE;
    wchar_t link[MAX_PATH];
    if (!JoinPath(link, MAX_PATH, system32, SHIM_NAME)) return FALSE;

    DWORD attrs = GetFileAttributesW(link);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            Print(L"Refusing to use an existing reparse point in System32.");
            return FALSE;
        }
        if (!SameFile(link, target)) {
            Print(L"Refusing to overwrite an unrelated System32 file.");
            return FALSE;
        }
    } else {
        if (!CreateHardLinkW(link, target, NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_NOT_SAME_DEVICE) {
                PrintError(L"CreateHardLinkW", error);
                return FALSE;
            }
            if (!CopyFileW(target, link, FALSE)) {
                PrintError(L"CopyFileW to System32", GetLastError());
                return FALSE;
            }
        }
    }

    HKEY ifeo = NULL;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, IFEO_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &ifeo, &disposition) != ERROR_SUCCESS) {
        Print(L"Could not open the Apple Music IFEO key.");
        return FALSE;
    }

    HKEY machine = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &machine, &disposition) != ERROR_SUCCESS) {
        RegCloseKey(ifeo);
        Print(L"Could not open the machine state key.");
        return FALSE;
    }

    DWORD alreadyInstalled = 0;
    ReadDwordValue(machine, L"Installed", &alreadyInstalled, NULL);
    if (!BackupIfeo(ifeo, machine, alreadyInstalled != 0)) {
        RegCloseKey(machine);
        RegCloseKey(ifeo);
        return FALSE;
    }

    if (!WriteStringValue(ifeo, L"VerifierDlls", SHIM_NAME)) {
        Print(L"Could not write VerifierDlls.");
        RegCloseKey(machine);
        RegCloseKey(ifeo);
        return FALSE;
    }

    DWORD globalFlag = 0;
    BOOL hadGlobal = FALSE;
    ReadDwordValue(ifeo, L"GlobalFlag", &globalFlag, &hadGlobal);
    globalFlag |= APP_VERIFIER_FLAG;
    if (!WriteDwordValue(ifeo, L"GlobalFlag", globalFlag)) {
        Print(L"Could not write GlobalFlag.");
        RegCloseKey(machine);
        RegCloseKey(ifeo);
        return FALSE;
    }

    BOOL ok = WriteDwordValue(machine, L"Installed", 1) &&
              WriteStringValue(machine, L"MachineDir", machineDir) &&
              WriteStringValue(machine, L"TargetPath", target) &&
              WriteStringValue(machine, L"System32Path", link) &&
              WriteStringValue(machine, L"ShimHash", hash);

    RegCloseKey(machine);
    RegCloseKey(ifeo);
    if (!ok) {
        Print(L"Install completed but state could not be recorded.");
        return FALSE;
    }

    wprintf(L"Installed verifier shim: %ls\n", link);
    return TRUE;
}

static BOOL UninstallShim(void)
{
    HKEY machine = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                      &machine) != ERROR_SUCCESS) {
        Print(L"The verifier shim is not installed.");
        return TRUE;
    }

    wchar_t expectedHash[65] = L"";
    wchar_t target[MAX_PATH] = L"";
    wchar_t link[MAX_PATH] = L"";
    wchar_t machineDir[MAX_PATH] = L"";
    ReadStringValue(machine, L"ShimHash", expectedHash, 65, NULL, NULL);
    ReadStringValue(machine, L"TargetPath", target, MAX_PATH, NULL, NULL);
    ReadStringValue(machine, L"System32Path", link, MAX_PATH, NULL, NULL);
    ReadStringValue(machine, L"MachineDir", machineDir, MAX_PATH, NULL, NULL);

    HKEY ifeo = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, IFEO_KEY, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                      &ifeo) != ERROR_SUCCESS) {
        Print(L"Could not open the Apple Music IFEO key.");
        RegCloseKey(machine);
        return FALSE;
    }

    wchar_t currentVerifier[1024] = L"";
    ReadStringValue(ifeo, L"VerifierDlls", currentVerifier, 1024, NULL, NULL);
    if (_wcsicmp(currentVerifier, SHIM_NAME) != 0) {
        Print(L"VerifierDlls no longer points to this shim; refusing to touch it.");
        RegCloseKey(ifeo);
        RegCloseKey(machine);
        return FALSE;
    }

    if (!RestoreIfeo(ifeo, machine)) {
        Print(L"Could not restore the original IFEO values.");
        RegCloseKey(ifeo);
        RegCloseKey(machine);
        return FALSE;
    }
    RegCloseKey(ifeo);

    if (link[0]) {
        wchar_t actual[65];
        if (GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES &&
            HashFile(link, actual, 65) && _wcsicmp(actual, expectedHash) == 0) {
            if (!DeleteFileW(link)) {
                wprintf(L"Could not remove System32 file (error %lu).\n", (unsigned long)GetLastError());
            }
        } else {
            wprintf(L"Leaving System32 file in place (hash mismatch or missing): %ls\n", link);
        }
    }
    if (target[0]) {
        wchar_t actual[65];
        if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES &&
            HashFile(target, actual, 65) && _wcsicmp(actual, expectedHash) == 0) {
            if (!DeleteFileW(target)) {
                DWORD error = GetLastError();
                wprintf(L"Could not remove target file (error %lu); scheduling deletion.\n",
                        (unsigned long)error);
                MoveFileExW(target, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        } else {
            wprintf(L"Leaving target file in place (hash mismatch or missing): %ls\n", target);
        }
    }
    if (machineDir[0]) RemoveDirectoryW(machineDir);

    RegCloseKey(machine);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, KEY_WOW64_64KEY, 0);
    Print(L"Uninstalled verifier shim.");
    return TRUE;
}

static void PrintStatus(void)
{
    HKEY machine = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0, KEY_READ | KEY_WOW64_64KEY,
                      &machine) != ERROR_SUCCESS) {
        Print(L"Verifier shim: not installed");
        return;
    }

    DWORD installed = 0;
    ReadDwordValue(machine, L"Installed", &installed, NULL);
    wchar_t link[MAX_PATH] = L"";
    ReadStringValue(machine, L"System32Path", link, MAX_PATH, NULL, NULL);
    wprintf(L"Verifier shim: %ls\n", installed ? L"installed" : L"not installed");
    if (link[0]) wprintf(L"System32 path: %ls\n", link);
    RegCloseKey(machine);
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);

    if (argc < 2) {
        Print(L"Usage: AMVerifierSetup.exe install [source-dir] | uninstall | status");
        return 2;
    }

    if (!_wcsicmp(argv[1], L"status")) {
        PrintStatus();
        return 0;
    }

    if (!IsElevated()) {
        Print(L"This command requires administrator privileges.");
        return 5;
    }

    if (!_wcsicmp(argv[1], L"install")) {
        wchar_t source[MAX_PATH];
        if (argc >= 3) {
            if (!GetFullPathNameW(argv[2], MAX_PATH, source, NULL)) return 2;
        } else {
            if (!GetCurrentDirectoryW(MAX_PATH, source)) return 2;
        }
        return InstallShim(source) ? 0 : 1;
    }

    if (!_wcsicmp(argv[1], L"uninstall")) {
        return UninstallShim() ? 0 : 1;
    }

    Print(L"Unknown command.");
    return 2;
}

#include "verifier_install.h"

#include <shlobj.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>

#define SHIM_NAME L"AMVerifierShim.dll"
#define MACHINE_KEY L"Software\\AppleMusicASIO\\Machine"
#define IFEO_ROOT L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\"
#define APP_VERIFIER_FLAG 0x100u

static const wchar_t* kTargets[] = {
    L"AMPLibraryAgent.exe"
};

static const wchar_t* kLegacyTargets[] = {
    L"AppleMusic.exe"
};

static wchar_t g_machineDir[MAX_PATH];

static void SetMessage(wchar_t* message, size_t count, const wchar_t* text)
{
    if (message && count) wcsncpy_s(message, count, text, _TRUNCATE);
}

static void SetError(wchar_t* message, size_t count, const wchar_t* action, DWORD error)
{
    if (message && count) {
        _snwprintf_s(message, count, _TRUNCATE, L"%ls failed (error %lu)", action,
                     (unsigned long)error);
    }
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

BOOL Verifier_GetMachineDir(wchar_t* out, DWORD count)
{
    if (g_machineDir[0]) {
        if (!out || !count) return FALSE;
        wcsncpy_s(out, count, g_machineDir, _TRUNCATE);
        return TRUE;
    }

    wchar_t programFiles[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES | CSIDL_FLAG_CREATE,
                                NULL, SHGFP_TYPE_CURRENT, programFiles))) {
        return FALSE;
    }
    _snwprintf_s(g_machineDir, MAX_PATH, _TRUNCATE, L"%s\\AppleMusicASIO", programFiles);
    if (out && count) wcsncpy_s(out, count, g_machineDir, _TRUNCATE);
    return TRUE;
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
    if (rc == ERROR_SUCCESS) {
        if (type != REG_DWORD) return ERROR_INVALID_DATATYPE;
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

static BOOL IfeoKeyName(const wchar_t* exe, wchar_t* out, DWORD count)
{
    return _snwprintf_s(out, count, _TRUNCATE, L"%ls%ls", IFEO_ROOT, exe) >= 0;
}

static BOOL StateKeyName(const wchar_t* exe, wchar_t* out, DWORD count)
{
    return _snwprintf_s(out, count, _TRUNCATE, L"%ls\\Ifeo\\%ls", MACHINE_KEY, exe) >= 0;
}

static BOOL BackupTarget(const wchar_t* exe, HKEY machine, BOOL legacy,
                         wchar_t* message, size_t count)
{
    wchar_t ifeoName[MAX_PATH];
    wchar_t stateName[MAX_PATH];
    if (!IfeoKeyName(exe, ifeoName, MAX_PATH) ||
        !StateKeyName(exe, stateName, MAX_PATH)) {
        SetMessage(message, count, L"Registry path is too long");
        return FALSE;
    }

    HKEY ifeo = NULL;
    HKEY state = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ifeoName, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &ifeo, NULL) != ERROR_SUCCESS) {
        SetMessage(message, count, L"Could not open the IFEO key");
        return FALSE;
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, stateName, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &state, NULL) != ERROR_SUCCESS) {
        RegCloseKey(ifeo);
        SetMessage(message, count, L"Could not create the verifier state key");
        return FALSE;
    }

    DWORD done = 0;
    BOOL donePresent = FALSE;
    if (ReadDwordValue(state, L"BackupDone", &done, &donePresent) != ERROR_SUCCESS) {
        RegCloseKey(state);
        RegCloseKey(ifeo);
        SetMessage(message, count, L"Could not read the verifier backup state");
        return FALSE;
    }
    if (donePresent && done) {
        RegCloseKey(state);
        RegCloseKey(ifeo);
        return TRUE;
    }

    wchar_t verifier[1024];
    DWORD verifierType = 0;
    BOOL hadVerifier = FALSE;
    if (ReadStringValue(ifeo, L"VerifierDlls", verifier, 1024, &verifierType, &hadVerifier) != ERROR_SUCCESS) {
        SetMessage(message, count, L"Could not read VerifierDlls");
        goto fail;
    }
    if (hadVerifier && verifier[0] && _wcsicmp(verifier, SHIM_NAME) != 0) {
        SetMessage(message, count, L"Another VerifierDlls value is already installed");
        goto fail;
    }
    if (hadVerifier && verifierType != REG_SZ) {
        SetMessage(message, count, L"VerifierDlls is not REG_SZ");
        goto fail;
    }

    DWORD globalFlag = 0;
    BOOL hadGlobal = FALSE;
    if (ReadDwordValue(ifeo, L"GlobalFlag", &globalFlag, &hadGlobal) != ERROR_SUCCESS) {
        SetMessage(message, count, L"Could not read GlobalFlag");
        goto fail;
    }

    if (hadVerifier && !_wcsicmp(verifier, SHIM_NAME)) {
        hadVerifier = FALSE;
        verifier[0] = 0;
    }

    if (legacy && !_wcsicmp(exe, L"AppleMusic.exe")) {
        DWORD oldHad = 0;
        BOOL oldPresent = FALSE;
        if (ReadDwordValue(machine, L"HadVerifierDlls", &oldHad, &oldPresent) == ERROR_SUCCESS &&
            oldPresent) {
            hadVerifier = oldHad != 0;
            if (hadVerifier) {
                ReadStringValue(machine, L"OriginalVerifierDlls", verifier, 1024, NULL, NULL);
            }
        }
        BOOL oldGlobalPresent = FALSE;
        DWORD oldGlobal = 0;
        if (ReadDwordValue(machine, L"HadGlobalFlag", &oldGlobal, &oldGlobalPresent) == ERROR_SUCCESS &&
            oldGlobalPresent) {
            hadGlobal = oldGlobal != 0;
            if (hadGlobal) {
                ReadDwordValue(machine, L"OriginalGlobalFlag", &globalFlag, NULL);
            }
        }
    }

    if (!WriteDwordValue(state, L"HadVerifierDlls", hadVerifier ? 1 : 0) ||
        (hadVerifier && !WriteStringValue(state, L"OriginalVerifierDlls", verifier)) ||
        !WriteDwordValue(state, L"HadGlobalFlag", hadGlobal ? 1 : 0) ||
        (hadGlobal && !WriteDwordValue(state, L"OriginalGlobalFlag", globalFlag)) ||
        !WriteDwordValue(state, L"BackupDone", 1)) {
        SetMessage(message, count, L"Could not save the original IFEO values");
        goto fail;
    }

    RegCloseKey(state);
    RegCloseKey(ifeo);
    return TRUE;

fail:
    RegCloseKey(state);
    RegCloseKey(ifeo);
    return FALSE;
}

static BOOL RemoveLegacyTarget(const wchar_t* exe, HKEY machine,
                               wchar_t* message, size_t count);

static BOOL SetIfeoTarget(const wchar_t* exe, wchar_t* message, size_t count)
{
    wchar_t keyName[MAX_PATH];
    if (!IfeoKeyName(exe, keyName, MAX_PATH)) {
        SetMessage(message, count, L"IFEO key path is too long");
        return FALSE;
    }

    HKEY ifeo = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyName, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &ifeo, NULL) != ERROR_SUCCESS) {
        SetMessage(message, count, L"Could not open the IFEO key");
        return FALSE;
    }

    BOOL ok = WriteStringValue(ifeo, L"VerifierDlls", SHIM_NAME);
    DWORD globalFlag = 0;
    BOOL hadGlobal = FALSE;
    if (ok) ok = ReadDwordValue(ifeo, L"GlobalFlag", &globalFlag, &hadGlobal) == ERROR_SUCCESS;
    if (ok) {
        globalFlag |= APP_VERIFIER_FLAG;
        ok = WriteDwordValue(ifeo, L"GlobalFlag", globalFlag);
    }
    RegCloseKey(ifeo);
    if (!ok) SetMessage(message, count, L"Could not update the IFEO values");
    return ok;
}

static BOOL ReplaceShim(const wchar_t* source, const wchar_t* target,
                        wchar_t* message, size_t count)
{
    wchar_t sourceHash[65];
    wchar_t targetHash[65];
    BOOL same = HashFile(source, sourceHash, 65) &&
                HashFile(target, targetHash, 65) &&
                _wcsicmp(sourceHash, targetHash) == 0;
    if (same) return TRUE;

    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(source, target, FALSE)) {
            SetError(message, count, L"CopyFileW", GetLastError());
            return FALSE;
        }
        return TRUE;
    }

    wchar_t backup[MAX_PATH];
    _snwprintf_s(backup, MAX_PATH, _TRUNCATE, L"%s.old.%lu", target,
                 (unsigned long)GetTickCount());
    if (!MoveFileExW(target, backup, MOVEFILE_REPLACE_EXISTING)) {
        SetError(message, count, L"Could not replace the loaded shim", GetLastError());
        return FALSE;
    }
    if (!CopyFileW(source, target, FALSE)) {
        DWORD error = GetLastError();
        MoveFileExW(backup, target, MOVEFILE_REPLACE_EXISTING);
        SetError(message, count, L"CopyFileW", error);
        return FALSE;
    }
    MoveFileExW(backup, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return TRUE;
}

static BOOL EnsureSystemLink(const wchar_t* link, const wchar_t* target,
                             HKEY machine, wchar_t* message, size_t count)
{
    wchar_t oldLink[MAX_PATH] = L"";
    ReadStringValue(machine, L"System32Path", oldLink, MAX_PATH, NULL, NULL);

    DWORD attrs = GetFileAttributesW(link);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            SetMessage(message, count, L"Refusing to use a reparse point in System32");
            return FALSE;
        }
        if (SameFile(link, target)) return TRUE;
        if (oldLink[0] && !_wcsicmp(oldLink, link)) {
            if (!DeleteFileW(link)) {
                SetError(message, count, L"Could not replace the old System32 link", GetLastError());
                return FALSE;
            }
        } else {
            SetMessage(message, count, L"Refusing to overwrite an unrelated System32 file");
            return FALSE;
        }
    }

    if (!CreateHardLinkW(link, target, NULL)) {
        SetError(message, count, L"CreateHardLinkW", GetLastError());
        return FALSE;
    }
    return TRUE;
}

BOOL Verifier_IsInstalled(void)
{
    HKEY machine = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0,
                      KEY_READ | KEY_WOW64_64KEY, &machine) != ERROR_SUCCESS) {
        return FALSE;
    }

    DWORD installed = 0;
    wchar_t link[MAX_PATH] = L"";
    wchar_t target[MAX_PATH] = L"";
    BOOL ok = ReadDwordValue(machine, L"Installed", &installed, NULL) == ERROR_SUCCESS &&
              installed != 0 &&
              ReadStringValue(machine, L"System32Path", link, MAX_PATH, NULL, NULL) == ERROR_SUCCESS &&
              ReadStringValue(machine, L"TargetPath", target, MAX_PATH, NULL, NULL) == ERROR_SUCCESS &&
              GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES;
    RegCloseKey(machine);
    if (!ok) return FALSE;

    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        wchar_t keyName[MAX_PATH];
        HKEY ifeo = NULL;
        if (!IfeoKeyName(kTargets[i], keyName, MAX_PATH) ||
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyName, 0,
                          KEY_READ | KEY_WOW64_64KEY, &ifeo) != ERROR_SUCCESS) {
            return FALSE;
        }
        wchar_t verifier[1024] = L"";
        ReadStringValue(ifeo, L"VerifierDlls", verifier, 1024, NULL, NULL);
        RegCloseKey(ifeo);
        if (_wcsicmp(verifier, SHIM_NAME) != 0) return FALSE;
    }
    return TRUE;
}

BOOL Verifier_Install(const wchar_t* sourceDir, wchar_t* message, size_t count)
{
    if (!IsElevated()) {
        SetMessage(message, count, L"Administrator privileges are required");
        return FALSE;
    }

    wchar_t source[MAX_PATH];
    if (!JoinPath(source, MAX_PATH, sourceDir, SHIM_NAME) ||
        GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
        SetMessage(message, count, L"AMVerifierShim.dll is missing from the release folder");
        return FALSE;
    }

    wchar_t machineDir[MAX_PATH];
    if (!Verifier_GetMachineDir(machineDir, MAX_PATH)) {
        SetMessage(message, count, L"Could not resolve Program Files");
        return FALSE;
    }
    if (!EnsureDirectory(machineDir) || !SetProtectedAcl(machineDir, TRUE)) {
        SetMessage(message, count, L"Could not protect the machine installation folder");
        return FALSE;
    }

    wchar_t target[MAX_PATH];
    if (!JoinPath(target, MAX_PATH, machineDir, SHIM_NAME)) {
        SetMessage(message, count, L"Installation path is too long");
        return FALSE;
    }
    if (!ReplaceShim(source, target, message, count) ||
        !SetProtectedAcl(target, FALSE)) {
        if (message && !message[0]) SetMessage(message, count, L"Could not install AMVerifierShim.dll");
        return FALSE;
    }

    wchar_t hash[65];
    if (!HashFile(target, hash, 65)) {
        SetMessage(message, count, L"Could not hash AMVerifierShim.dll");
        return FALSE;
    }

    wchar_t system32[MAX_PATH];
    if (!GetSystemDirectoryW(system32, MAX_PATH)) {
        SetMessage(message, count, L"Could not resolve System32");
        return FALSE;
    }
    wchar_t link[MAX_PATH];
    if (!JoinPath(link, MAX_PATH, system32, SHIM_NAME)) {
        SetMessage(message, count, L"System32 path is too long");
        return FALSE;
    }

    HKEY machine = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &machine, NULL) != ERROR_SUCCESS) {
        SetMessage(message, count, L"Could not create the verifier state key");
        return FALSE;
    }

    DWORD already = 0;
    ReadDwordValue(machine, L"Installed", &already, NULL);
    BOOL legacy = already != 0;

    if (!EnsureSystemLink(link, target, machine, message, count)) {
        RegCloseKey(machine);
        return FALSE;
    }

    for (size_t i = 0; i < sizeof(kLegacyTargets) / sizeof(kLegacyTargets[0]); ++i) {
        if (!RemoveLegacyTarget(kLegacyTargets[i], machine, message, count)) {
            RegCloseKey(machine);
            return FALSE;
        }
    }

    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        if (!BackupTarget(kTargets[i], machine, legacy, message, count)) {
            RegCloseKey(machine);
            return FALSE;
        }
    }
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        if (!SetIfeoTarget(kTargets[i], message, count)) {
            RegCloseKey(machine);
            return FALSE;
        }
    }

    BOOL ok = WriteDwordValue(machine, L"Installed", 1) &&
              WriteStringValue(machine, L"MachineDir", machineDir) &&
              WriteStringValue(machine, L"TargetPath", target) &&
              WriteStringValue(machine, L"System32Path", link) &&
              WriteStringValue(machine, L"ShimHash", hash);
    RegCloseKey(machine);
    if (!ok) {
        SetMessage(message, count, L"Could not save the verifier installation state");
        return FALSE;
    }
    return TRUE;
}

static BOOL RestoreTarget(const wchar_t* exe, HKEY machine, wchar_t* message, size_t count)
{
    wchar_t ifeoName[MAX_PATH];
    wchar_t stateName[MAX_PATH];
    if (!IfeoKeyName(exe, ifeoName, MAX_PATH) ||
        !StateKeyName(exe, stateName, MAX_PATH)) return FALSE;

    HKEY ifeo = NULL;
    HKEY state = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ifeoName, 0,
                      KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &ifeo) != ERROR_SUCCESS ||
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, stateName, 0,
                      KEY_READ | KEY_WOW64_64KEY, &state) != ERROR_SUCCESS) {
        if (ifeo) RegCloseKey(ifeo);
        if (state) RegCloseKey(state);
        SetMessage(message, count, L"Could not open the verifier state");
        return FALSE;
    }

    wchar_t verifier[1024] = L"";
    ReadStringValue(ifeo, L"VerifierDlls", verifier, 1024, NULL, NULL);
    if (_wcsicmp(verifier, SHIM_NAME) != 0) {
        RegCloseKey(state);
        RegCloseKey(ifeo);
        SetMessage(message, count, L"VerifierDlls no longer points to this shim; refusing to change it");
        return FALSE;
    }

    DWORD hadVerifier = 0;
    DWORD hadGlobal = 0;
    BOOL ok = ReadDwordValue(state, L"HadVerifierDlls", &hadVerifier, NULL) == ERROR_SUCCESS &&
              ReadDwordValue(state, L"HadGlobalFlag", &hadGlobal, NULL) == ERROR_SUCCESS;
    if (ok) {
        if (hadVerifier) {
            wchar_t original[1024] = L"";
            ok = ReadStringValue(state, L"OriginalVerifierDlls", original, 1024, NULL, NULL) &&
                 WriteStringValue(ifeo, L"VerifierDlls", original);
        } else {
            LONG rc = RegDeleteValueW(ifeo, L"VerifierDlls");
            ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
        }
    }
    if (ok) {
        if (hadGlobal) {
            DWORD original = 0;
            ok = ReadDwordValue(state, L"OriginalGlobalFlag", &original, NULL) == ERROR_SUCCESS &&
                 WriteDwordValue(ifeo, L"GlobalFlag", original);
        } else {
            LONG rc = RegDeleteValueW(ifeo, L"GlobalFlag");
            ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
        }
    }

    RegCloseKey(state);
    RegCloseKey(ifeo);
    if (!ok) SetMessage(message, count, L"Could not restore the original IFEO values");
    return ok;
}

static BOOL RemoveLegacyTarget(const wchar_t* exe, HKEY machine,
                               wchar_t* message, size_t count)
{
    wchar_t ifeoName[MAX_PATH];
    if (!IfeoKeyName(exe, ifeoName, MAX_PATH)) return TRUE;

    HKEY ifeo = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ifeoName, 0,
                      KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &ifeo) != ERROR_SUCCESS) {
        return TRUE;
    }

    wchar_t verifier[1024] = L"";
    ReadStringValue(ifeo, L"VerifierDlls", verifier, 1024, NULL, NULL);
    if (_wcsicmp(verifier, SHIM_NAME) != 0) {
        RegCloseKey(ifeo);
        return TRUE;
    }

    wchar_t stateName[MAX_PATH];
    HKEY state = NULL;
    if (StateKeyName(exe, stateName, MAX_PATH) &&
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, stateName, 0,
                      KEY_READ | KEY_WOW64_64KEY, &state) == ERROR_SUCCESS) {
        RegCloseKey(state);
        RegCloseKey(ifeo);
        return RestoreTarget(exe, machine, message, count);
    }

    LONG rc = RegDeleteValueW(ifeo, L"VerifierDlls");
    BOOL ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    DWORD globalFlag = 0;
    BOOL hadGlobal = FALSE;
    if (ok) {
        ok = ReadDwordValue(ifeo, L"GlobalFlag", &globalFlag, &hadGlobal) == ERROR_SUCCESS;
    }
    if (ok && hadGlobal) {
        if (globalFlag == APP_VERIFIER_FLAG) {
            rc = RegDeleteValueW(ifeo, L"GlobalFlag");
            ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
        } else if (globalFlag & APP_VERIFIER_FLAG) {
            ok = WriteDwordValue(ifeo, L"GlobalFlag", globalFlag & ~APP_VERIFIER_FLAG);
        }
    }
    RegCloseKey(ifeo);
    if (!ok) SetMessage(message, count, L"Could not remove the old AppleMusic verifier entry");
    return ok;
}

BOOL Verifier_Uninstall(wchar_t* message, size_t count)
{
    if (!IsElevated()) {
        SetMessage(message, count, L"Administrator privileges are required");
        return FALSE;
    }

    HKEY machine = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MACHINE_KEY, 0,
                      KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &machine) != ERROR_SUCCESS) {
        SetMessage(message, count, L"Verifier shim is not installed");
        return TRUE;
    }

    wchar_t link[MAX_PATH] = L"";
    wchar_t target[MAX_PATH] = L"";
    wchar_t dir[MAX_PATH] = L"";
    wchar_t expectedHash[65] = L"";
    ReadStringValue(machine, L"System32Path", link, MAX_PATH, NULL, NULL);
    ReadStringValue(machine, L"TargetPath", target, MAX_PATH, NULL, NULL);
    ReadStringValue(machine, L"MachineDir", dir, MAX_PATH, NULL, NULL);
    ReadStringValue(machine, L"ShimHash", expectedHash, 65, NULL, NULL);

    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        wchar_t ifeoName[MAX_PATH];
        HKEY ifeo = NULL;
        if (!IfeoKeyName(kTargets[i], ifeoName, MAX_PATH) ||
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, ifeoName, 0,
                          KEY_READ | KEY_WOW64_64KEY, &ifeo) != ERROR_SUCCESS) {
            RegCloseKey(machine);
            SetMessage(message, count, L"Could not open the IFEO key");
            return FALSE;
        }
        wchar_t verifier[1024] = L"";
        ReadStringValue(ifeo, L"VerifierDlls", verifier, 1024, NULL, NULL);
        RegCloseKey(ifeo);
        if (_wcsicmp(verifier, SHIM_NAME) != 0) {
            RegCloseKey(machine);
            SetMessage(message, count, L"VerifierDlls no longer points to this shim; refusing to uninstall");
            return FALSE;
        }
    }

    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); ++i) {
        if (!RestoreTarget(kTargets[i], machine, message, count)) {
            RegCloseKey(machine);
            return FALSE;
        }
    }
    for (size_t i = 0; i < sizeof(kLegacyTargets) / sizeof(kLegacyTargets[0]); ++i) {
        if (!RemoveLegacyTarget(kLegacyTargets[i], machine, message, count)) {
            RegCloseKey(machine);
            return FALSE;
        }
    }

    if (link[0] && GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES) {
        wchar_t actual[65];
        if (HashFile(link, actual, 65) && _wcsicmp(actual, expectedHash) == 0) {
            DeleteFileW(link);
        }
    }
    if (target[0] && GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
        wchar_t actual[65];
        if (HashFile(target, actual, 65) && _wcsicmp(actual, expectedHash) == 0) {
            if (!DeleteFileW(target)) {
                MoveFileExW(target, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
    }
    if (dir[0]) RemoveDirectoryW(dir);

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, MACHINE_KEY);
    RegCloseKey(machine);
    SetMessage(message, count, L"Verifier shim uninstalled");
    return TRUE;
}

#include "setup_common.h"

#include <shlobj.h>
#include <shellapi.h>
#include <winioctl.h>
#include <stddef.h>

#define AMB_SETUP_KEY L"Software\\AppleMusicASIO"
#define AMB_PACKAGE_FAMILY L"AppleInc.AppleMusicWin_nzyj5cx40ttqa"
#define AMB_ENUM_CLSID L"{BCDE0395-E52F-467C-8E3D-C4579291692E}"
#define AMB_ENUM_CLSID_KEY L"Software\\Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32"
#define AMB_EARLY_CLSID L"{76765B11-3F95-4AF2-AC9D-EA55D8994F1A}"
#define AMB_EARLY_CLSID_KEY L"Software\\Classes\\CLSID\\{76765B11-3F95-4AF2-AC9D-EA55D8994F1A}\\InprocServer32"
#define AMB_EARLY_CLSID_ROOT L"Software\\Classes\\CLSID\\{76765B11-3F95-4AF2-AC9D-EA55D8994F1A}"
#define AMB_INSTALL_DIR_NAME L"AppleMusicASIO"
#define AMB_LAUNCHER_NAME L"AMAudioBridge.dll"
#define AMB_EARLY_NAME L"AMEarlyProxy.dll"

typedef struct MyMountPoint {
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
} MyMountPoint;

typedef struct MyReparseBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    MyMountPoint MountPointReparseBuffer;
} MyReparseBuffer;

static wchar_t g_install_dir[MAX_PATH];

static void SetMessage(wchar_t* message, size_t count, const wchar_t* text)
{
    if (message && count) {
        wcsncpy_s(message, count, text, _TRUNCATE);
    }
}

static BOOL ReadRegString(HKEY root, const wchar_t* key, const wchar_t* value,
                          wchar_t* out, DWORD count, DWORD* type)
{
    HKEY h = NULL;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) {
        return FALSE;
    }
    DWORD bytes = count * sizeof(wchar_t);
    DWORD t = 0;
    LONG rc = RegQueryValueExW(h, value, NULL, &t, (BYTE*)out, &bytes);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || (t != REG_SZ && t != REG_EXPAND_SZ)) {
        return FALSE;
    }
    out[count - 1] = 0;
    if (type) *type = t;
    return TRUE;
}

static BOOL WriteRegString(HKEY root, const wchar_t* key, const wchar_t* value,
                           const wchar_t* text)
{
    HKEY h = NULL;
    DWORD disp = 0;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY,
                        NULL, &h, &disp) != ERROR_SUCCESS) {
        return FALSE;
    }
    DWORD bytes = (DWORD)((wcslen(text) + 1) * sizeof(wchar_t));
    LONG rc = RegSetValueExW(h, value, 0, REG_SZ, (const BYTE*)text, bytes);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

static BOOL DeleteRegValue(HKEY root, const wchar_t* key, const wchar_t* value)
{
    HKEY h = NULL;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) {
        return FALSE;
    }
    LONG rc = RegDeleteValueW(h, value);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

static BOOL PathEqual(const wchar_t* a, const wchar_t* b)
{
    wchar_t left[MAX_PATH * 2], right[MAX_PATH * 2];
    wcsncpy_s(left, MAX_PATH * 2, a, _TRUNCATE);
    wcsncpy_s(right, MAX_PATH * 2, b, _TRUNCATE);
    if (left[0] && left[wcslen(left) - 1] == L'\\') left[wcslen(left) - 1] = 0;
    if (right[0] && right[wcslen(right) - 1] == L'\\') right[wcslen(right) - 1] = 0;
    return !_wcsicmp(left, right);
}

static BOOL CopyTree(const wchar_t* source, const wchar_t* target)
{
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", source);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    SHCreateDirectoryExW(NULL, target, NULL);
    BOOL ok = TRUE;
    do {
        if (!_wcsicmp(fd.cFileName, L".") || !_wcsicmp(fd.cFileName, L"..")) continue;
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        _snwprintf_s(src, MAX_PATH, _TRUNCATE, L"%s\\%s", source, fd.cFileName);
        _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%s\\%s", target, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyTree(src, dst)) ok = FALSE;
        } else if (!CopyFileExW(src, dst, NULL, NULL, NULL,
                               COPY_FILE_ALLOW_DECRYPTED_DESTINATION) &&
                   GetLastError() != ERROR_FILE_EXISTS) {
            ok = FALSE;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}

static BOOL GetReparseTarget(const wchar_t* path, wchar_t* out, DWORD count)
{
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BYTE buffer[32768];
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer,
                              sizeof(buffer), &returned, NULL);
    CloseHandle(h);
    if (!ok || returned < sizeof(ULONG) + 8) return FALSE;

    MyReparseBuffer* r = (MyReparseBuffer*)buffer;
    if (r->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT) return FALSE;
    USHORT offset = r->MountPointReparseBuffer.SubstituteNameOffset;
    USHORT length = r->MountPointReparseBuffer.SubstituteNameLength;
    const BYTE* base = (const BYTE*)&r->MountPointReparseBuffer +
                       offsetof(MyMountPoint, PathBuffer);
    if ((DWORD)offset + length + sizeof(wchar_t) > returned) return FALSE;
    wchar_t name[MAX_PATH * 2];
    DWORD chars = length / sizeof(wchar_t);
    if (chars >= MAX_PATH * 2) return FALSE;
    memcpy(name, base + offset, length);
    name[chars] = 0;
    const wchar_t* p = name;
    if (!wcsncmp(p, L"\\??\\", 4)) p += 4;
    wcsncpy_s(out, count, p, _TRUNCATE);
    return out[0] != 0;
}

static BOOL OpenDirectoryFinalPath(const wchar_t* path, wchar_t* out, DWORD count)
{
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD n = GetFinalPathNameByHandleW(h, out, count, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= count) return FALSE;
    if (!wcsncmp(out, L"\\\\?\\", 4)) {
        memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof(wchar_t));
    }
    return out[0] != 0;
}

static BOOL ResolveLinkTarget(const wchar_t* path, wchar_t* out, DWORD count);

static BOOL CreateJunction(const wchar_t* link, const wchar_t* target)
{
    if (!CreateDirectoryW(link, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return FALSE;
    }

    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    wchar_t substitute[MAX_PATH * 2];
    _snwprintf_s(substitute, MAX_PATH * 2, _TRUNCATE, L"\\??\\%s", target);
    DWORD sub_len = (DWORD)((wcslen(substitute) + 1) * sizeof(wchar_t));
    DWORD print_len = (DWORD)((wcslen(target) + 1) * sizeof(wchar_t));
    DWORD data_len = 8 + sub_len + print_len;
    DWORD total = (DWORD)(offsetof(MyReparseBuffer, MountPointReparseBuffer) + data_len);
    MyReparseBuffer* buf = (MyReparseBuffer*)calloc(1, total);
    if (!buf) {
        CloseHandle(h);
        return FALSE;
    }

    buf->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buf->ReparseDataLength = (USHORT)data_len;
    buf->MountPointReparseBuffer.SubstituteNameOffset = 0;
    buf->MountPointReparseBuffer.SubstituteNameLength = (USHORT)(sub_len - sizeof(wchar_t));
    buf->MountPointReparseBuffer.PrintNameOffset = (USHORT)sub_len;
    buf->MountPointReparseBuffer.PrintNameLength = (USHORT)(print_len - sizeof(wchar_t));
    memcpy(buf->MountPointReparseBuffer.PathBuffer, substitute, sub_len);
    memcpy((BYTE*)buf->MountPointReparseBuffer.PathBuffer + sub_len, target, print_len);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf, total, NULL, 0,
                              &returned, NULL);
    free(buf);
    CloseHandle(h);
    if (!ok) {
        wchar_t check[MAX_PATH];
        if (ResolveLinkTarget(link, check, MAX_PATH) && PathEqual(check, target)) {
            return TRUE;
        }
    }
    return ok;
}

static BOOL ResolveLinkTarget(const wchar_t* path, wchar_t* out, DWORD count)
{
    return GetReparseTarget(path, out, count) || OpenDirectoryFinalPath(path, out, count);
}

static BOOL GetLocalCachePath(wchar_t* out, DWORD count)
{
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;

    _snwprintf_s(out, count, _TRUNCATE, L"%s\\Packages\\%s\\LocalCache",
                 local, AMB_PACKAGE_FAMILY);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return TRUE;

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\Packages\\AppleInc.AppleMusicWin_*",
                 local);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        _snwprintf_s(out, count, _TRUNCATE, L"%s\\Packages\\%s\\LocalCache",
                     local, fd.cFileName);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

BOOL Setup_EnsurePreferencesJunction(BOOL repair, wchar_t* message, size_t count)
{
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL,
                                SHGFP_TYPE_CURRENT, appdata))) {
        SetMessage(message, count, L"Cannot resolve %APPDATA%");
        return FALSE;
    }

    wchar_t cache[MAX_PATH];
    if (!GetLocalCachePath(cache, MAX_PATH)) {
        SetMessage(message, count, L"Apple Music package data not found; start Apple Music once");
        return FALSE;
    }

    wchar_t cache_final[MAX_PATH];
    if (!GetReparseTarget(cache, cache_final, MAX_PATH) &&
        !OpenDirectoryFinalPath(cache, cache_final, MAX_PATH)) {
        wcsncpy_s(cache_final, MAX_PATH, cache, _TRUNCATE);
    }

    wchar_t target[MAX_PATH];
    _snwprintf_s(target, MAX_PATH, _TRUNCATE,
                 L"%s\\Roaming\\Apple Computer\\Preferences", cache_final);
    wchar_t link[MAX_PATH];
    _snwprintf_s(link, MAX_PATH, _TRUNCATE, L"%s\\Apple Computer\\Preferences", appdata);

    wchar_t link_vol[MAX_PATH], target_vol[MAX_PATH];
    if (!GetVolumePathNameW(appdata, link_vol, MAX_PATH) ||
        !GetVolumePathNameW(cache_final, target_vol, MAX_PATH)) {
        SetMessage(message, count, L"Cannot determine volume for preferences");
        return FALSE;
    }
    if (!_wcsicmp(link_vol, target_vol)) {
        SetMessage(message, count, L"Preferences and package data are on the same volume; no junction needed");
        return TRUE;
    }

    if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
        SHCreateDirectoryExW(NULL, target, NULL);
    }

    DWORD attrs = GetFileAttributesW(link);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            wchar_t current[MAX_PATH];
            if (ResolveLinkTarget(link, current, MAX_PATH)) {
                if (PathEqual(current, target)) {
                    SetMessage(message, count, L"Preferences junction already correct");
                    return TRUE;
                }
            } else {
                SetMessage(message, count, L"Preferences junction present (verification deferred)");
                return TRUE;
            }
            if (!repair) {
                SetMessage(message, count, L"Stale preferences junction; repair deferred");
                return TRUE;
            }
            RemoveDirectoryW(link);
        } else {
            if (!repair) {
                SetMessage(message, count, L"Existing preferences directory; junction repair deferred");
                return TRUE;
            }
            wchar_t pattern[MAX_PATH];
            _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", link);
            WIN32_FIND_DATAW fd;
            HANDLE find = FindFirstFileW(pattern, &fd);
            BOOL empty = find == INVALID_HANDLE_VALUE;
            if (!empty) {
                do {
                    if (_wcsicmp(fd.cFileName, L".") && _wcsicmp(fd.cFileName, L"..")) {
                        empty = FALSE;
                        break;
                    }
                    empty = TRUE;
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
            if (empty) {
                RemoveDirectoryW(link);
            } else {
                CopyTree(link, target);
                SYSTEMTIME st;
                GetLocalTime(&st);
                wchar_t backup[MAX_PATH];
                _snwprintf_s(backup, MAX_PATH, _TRUNCATE,
                             L"%s.bak-%04u%02u%02u%02u%02u%02u", link,
                             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                if (!MoveFileW(link, backup)) {
                    SetMessage(message, count, L"Could not back up existing preferences directory");
                    return FALSE;
                }
            }
        }
    }

    SHCreateDirectoryExW(NULL, link, NULL);
    if (!CreateJunction(link, target)) {
        wchar_t check[MAX_PATH] = L"";
        if (!(ResolveLinkTarget(link, check, MAX_PATH) && PathEqual(check, target))) {
            wchar_t detail[1024];
            _snwprintf_s(detail, 1024, _TRUNCATE,
                         L"Could not create preferences junction (err=%lu link=%ls target=%ls current=%ls)",
                         (unsigned long)GetLastError(), link, target, check);
            SetMessage(message, count, detail);
            return FALSE;
        }
    }

    wchar_t verify[MAX_PATH];
    if (!ResolveLinkTarget(link, verify, MAX_PATH)) {
        SetMessage(message, count, L"Preferences junction created (verification deferred)");
        return TRUE;
    }
    if (PathEqual(verify, target)) {
        SetMessage(message, count, L"Preferences junction created");
        return TRUE;
    }
    wchar_t detail[1024];
    _snwprintf_s(detail, 1024, _TRUNCATE,
                 L"Junction verification failed: link=%ls target=%ls", verify, target);
    SetMessage(message, count, detail);
    return FALSE;
}

const wchar_t* Setup_InstallDir(void)
{
    if (g_install_dir[0]) return g_install_dir;
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        wcsncpy_s(local, MAX_PATH, L"C:\\Users\\Default\\AppData\\Local", _TRUNCATE);
    }
    _snwprintf_s(g_install_dir, MAX_PATH, _TRUNCATE, L"%s\\%s", local,
                 AMB_INSTALL_DIR_NAME);
    return g_install_dir;
}

static void BridgePath(wchar_t* out, DWORD count)
{
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\%s", Setup_InstallDir(), AMB_LAUNCHER_NAME);
}

static void EarlyProxyPath(wchar_t* out, DWORD count)
{
    _snwprintf_s(out, count, _TRUNCATE, L"%s\\%s", Setup_InstallDir(), AMB_EARLY_NAME);
}

BOOL Setup_IsInstalled(void)
{
    wchar_t path[MAX_PATH];
    if (!ReadRegString(HKEY_CURRENT_USER, AMB_ENUM_CLSID_KEY, NULL, path, MAX_PATH, NULL)) {
        return FALSE;
    }
    wchar_t bridge[MAX_PATH];
    BridgePath(bridge, MAX_PATH);
    if (_wcsicmp(path, bridge) != 0 || GetFileAttributesW(bridge) == INVALID_FILE_ATTRIBUTES) {
        return FALSE;
    }

    wchar_t early[MAX_PATH];
    EarlyProxyPath(early, MAX_PATH);
    if (!ReadRegString(HKEY_CURRENT_USER, AMB_EARLY_CLSID_KEY, NULL, path, MAX_PATH, NULL)) {
        return FALSE;
    }
    return !_wcsicmp(path, early) && GetFileAttributesW(early) != INVALID_FILE_ATTRIBUTES;
}

BOOL Setup_GetMode(wchar_t* mode, size_t count)
{
    if (!mode || !count) return FALSE;
    if (!ReadRegString(HKEY_CURRENT_USER, AMB_SETUP_KEY, L"Mode", mode,
                       (DWORD)count, NULL)) {
        wcsncpy_s(mode, count, L"auto", _TRUNCATE);
    }
    return TRUE;
}

BOOL Setup_SetMode(const wchar_t* mode)
{
    if (!mode || !*mode) return FALSE;
    return WriteRegString(HKEY_CURRENT_USER, AMB_SETUP_KEY, L"Mode", mode);
}

static BOOL RegisterInprocServer(const wchar_t* key, const wchar_t* dll)
{
    HKEY h = NULL;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0,
                        KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, &disp) != ERROR_SUCCESS) {
        return FALSE;
    }
    DWORD bytes = (DWORD)((wcslen(dll) + 1) * sizeof(wchar_t));
    LONG rc = RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE*)dll, bytes);
    if (rc == ERROR_SUCCESS) {
        const wchar_t* both = L"Both";
        rc = RegSetValueExW(h, L"ThreadingModel", 0, REG_SZ, (const BYTE*)both,
                            (DWORD)((wcslen(both) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

static BOOL RegisterComServer(const wchar_t* bridge)
{
    return RegisterInprocServer(AMB_ENUM_CLSID_KEY, bridge);
}

static BOOL RegisterEarlyProxy(const wchar_t* early)
{
    return RegisterInprocServer(AMB_EARLY_CLSID_KEY, early);
}

static void UnregisterInprocServer(const wchar_t* key, const wchar_t* root,
                                   const wchar_t* dll)
{
    wchar_t current[MAX_PATH];
    if (ReadRegString(HKEY_CURRENT_USER, key, NULL, current, MAX_PATH, NULL) &&
        !_wcsicmp(current, dll)) {
        RegDeleteKeyW(HKEY_CURRENT_USER, key);
        RegDeleteKeyW(HKEY_CURRENT_USER, root);
    }
}

static void UnregisterComServer(void)
{
    wchar_t bridge[MAX_PATH];
    BridgePath(bridge, MAX_PATH);
    UnregisterInprocServer(AMB_ENUM_CLSID_KEY,
                           L"Software\\Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}",
                           bridge);
}

static void UnregisterEarlyProxy(void)
{
    wchar_t early[MAX_PATH];
    EarlyProxyPath(early, MAX_PATH);
    UnregisterInprocServer(AMB_EARLY_CLSID_KEY, AMB_EARLY_CLSID_ROOT, early);
}

BOOL Setup_Install(const wchar_t* mode, wchar_t* message, size_t count)
{
    wchar_t source[MAX_PATH];
    GetModuleFileNameW(NULL, source, MAX_PATH);
    wchar_t* slash = wcsrchr(source, L'\\');
    if (slash) *slash = 0;
    else source[0] = 0;

    const wchar_t* files[] = {
        L"AMAudioBridge.dll", L"AMEarlyProxy.dll", L"AMPrefsPersist.dll",
        L"AMAudioBridge.ini"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        wchar_t src[MAX_PATH];
        _snwprintf_s(src, MAX_PATH, _TRUNCATE, L"%s\\%s", source, files[i]);
        if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
            SetMessage(message, count, L"Release files are incomplete; extract the full package first.");
            return FALSE;
        }
    }

    SHCreateDirectoryExW(NULL, Setup_InstallDir(), NULL);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        _snwprintf_s(src, MAX_PATH, _TRUNCATE, L"%s\\%s", source, files[i]);
        _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%s\\%s", Setup_InstallDir(), files[i]);
        if (!_wcsicmp(src, dst)) continue;
        if (!CopyFileW(src, dst, FALSE) && GetLastError() != ERROR_FILE_EXISTS) {
            SetMessage(message, count, L"Could not copy bridge files; close Apple Music and retry");
            return FALSE;
        }
    }

    wchar_t bridge[MAX_PATH];
    BridgePath(bridge, MAX_PATH);
    if (!RegisterComServer(bridge)) {
        SetMessage(message, count, L"Could not register the COM bridge");
        return FALSE;
    }

    wchar_t early[MAX_PATH];
    EarlyProxyPath(early, MAX_PATH);
    if (GetFileAttributesW(early) == INVALID_FILE_ATTRIBUTES) {
        SetMessage(message, count, L"AMEarlyProxy.dll is missing from the setup folder");
        return FALSE;
    }
    if (!RegisterEarlyProxy(early)) {
        SetMessage(message, count, L"Could not register the early COM proxy");
        return FALSE;
    }
    WriteRegString(HKEY_CURRENT_USER, AMB_SETUP_KEY, L"InstallDir", Setup_InstallDir());
    if (mode && *mode) Setup_SetMode(mode);

    wchar_t fix[512] = L"";
    if (!Setup_EnsurePreferencesJunction(TRUE, fix, 512) && fix[0]) {
        _snwprintf_s(message, count, _TRUNCATE,
                     L"Installed. Restart Apple Music.\r\n\r\nPreferences warning: %ls", fix);
    } else {
        SetMessage(message, count, L"Installed. Restart Apple Music.");
    }
    return TRUE;
}

BOOL Setup_Uninstall(wchar_t* message, size_t count)
{
    UnregisterComServer();
    UnregisterEarlyProxy();
    wchar_t root[MAX_PATH];
    wcsncpy_s(root, MAX_PATH, Setup_InstallDir(), _TRUNCATE);

    const wchar_t* files[] = {
        L"AMAudioBridge.ini", L"AMPrefsPersist.dll", L"AMAudioBridge.dll",
        L"AMEarlyProxy.dll"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", root, files[i]);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    RemoveDirectoryW(root);
    RegDeleteKeyW(HKEY_CURRENT_USER, AMB_SETUP_KEY);
    SetMessage(message, count, L"Uninstalled. Restart Apple Music. Files in use will be removed at reboot.");
    return TRUE;
}

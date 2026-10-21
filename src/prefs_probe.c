#include "common.h"
#include <tlhelp32.h>

typedef BOOL (WINAPI* PFN_WriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef HANDLE (WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                         DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI* PFN_DeleteFileW)(LPCWSTR);

static HMODULE g_module = NULL;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static PFN_WriteFile g_write = NULL;
static PFN_CreateFileW g_create = NULL;
static PFN_DeleteFileW g_delete = NULL;
static wchar_t g_dump_dir[MAX_PATH] = L"";
static volatile LONG g_dump_counter = 0;

static uintptr_t g_seen[256];
static volatile LONG g_seen_count = 0;

static void RawLog(const char* text)
{
    if (g_log == INVALID_HANDLE_VALUE || !g_write) return;
    DWORD written = 0;
    EnterCriticalSection(&g_cs);
    g_write(g_log, text, (DWORD)strlen(text), &written, NULL);
    LeaveCriticalSection(&g_cs);
}

static void ProbeLog(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    char line[1200];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%lu] %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentThreadId(), msg);
    RawLog(line);
}

static void DumpBuffer(const wchar_t* path, const void* buffer, DWORD length)
{
    if (!g_create || !g_write || g_dump_dir[0] == 0) return;
    wchar_t base[64];
    _snwprintf_s(base, 64, _TRUNCATE, L"%lu_%04ld.plist",
                 (unsigned long)GetCurrentProcessId(),
                 InterlockedIncrement(&g_dump_counter));
    wchar_t file[MAX_PATH];
    _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%s\\%s", g_dump_dir, base);
    HANDLE h = g_create(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    g_write(h, buffer, length, &written, NULL);
    CloseHandle(h);

    wchar_t sidecar[MAX_PATH];
    _snwprintf_s(sidecar, MAX_PATH, _TRUNCATE, L"%s\\%s.txt", g_dump_dir, base);
    HANDLE t = g_create(sidecar, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (t != INVALID_HANDLE_VALUE) {
        char utf8[MAX_PATH * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
        if (n > 1) g_write(t, utf8, (DWORD)(n - 1), &written, NULL);
        CloseHandle(t);
    }
    ProbeLog("dumped %lu bytes for %ls -> %ls", (unsigned long)length, path, base);
}

static BOOL WINAPI Hook_WriteFile(HANDLE file, LPCVOID buffer, DWORD length,
                                  LPDWORD written, LPOVERLAPPED overlapped)
{
    BOOL ok = g_write ? g_write(file, buffer, length, written, overlapped) : FALSE;
    if (ok && buffer && length) {
        wchar_t path[1024];
        DWORD n = GetFinalPathNameByHandleW(file, path, 1024, FILE_NAME_NORMALIZED);
        if (n > 0 && n < 1024) {
            if (wcsstr(path, L".plist") || wcsstr(path, L"Preferences")) {
                ProbeLog("WriteFile %lu bytes: %ls", (unsigned long)length, path);
                DumpBuffer(path, buffer, length);
                const wchar_t* marker = wcsstr(path, L".plist.Xa");
                if (marker) {
                    wchar_t final_path[1024];
                    size_t prefix = (size_t)(marker - path) + 6; /* keep ".plist" */
                    if (prefix >= 1024) prefix = 1023;
                    wcsncpy_s(final_path, 1024, path, prefix);
                    final_path[prefix] = 0;
                    HANDLE out = g_create(final_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (out != INVALID_HANDLE_VALUE) {
                        DWORD wrote = 0;
                        g_write(out, buffer, length, &wrote, NULL);
                        CloseHandle(out);
                        ProbeLog("persisted final plist: %ls", final_path);
                    } else {
                        ProbeLog("persist open failed: %ls", final_path);
                    }
                }
            }
        }
    }
    return ok;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE template_file)
{
    HANDLE h = g_create ? g_create(name, access, share, sa, disposition, flags, template_file)
                        : INVALID_HANDLE_VALUE;
    if (name && (wcsstr(name, L".plist") || wcsstr(name, L"Preferences"))) {
        ProbeLog("CreateFileW disp=%lu: %ls", (unsigned long)disposition, name);
    }
    return h;
}

static BOOL WINAPI Hook_DeleteFileW(LPCWSTR name)
{
    if (name && (wcsstr(name, L".plist") || wcsstr(name, L"Preferences"))) {
        ProbeLog("DeleteFileW: %ls", name);
    }
    return g_delete ? g_delete(name) : FALSE;
}

typedef struct {
    const char* name;
    void* hook;
    void** original;
} HookSpec;

static HookSpec g_hooks[] = {
    {"WriteFile", (void*)Hook_WriteFile, (void**)&g_write},
    {"CreateFileW", (void*)Hook_CreateFileW, (void**)&g_create},
    {"DeleteFileW", (void*)Hook_DeleteFileW, (void**)&g_delete},
};

static BOOL Seen(uintptr_t base)
{
    LONG count = g_seen_count;
    for (LONG i = 0; i < count; ++i) {
        if (g_seen[i] == base) return TRUE;
    }
    return FALSE;
}

static void Mark(uintptr_t base)
{
    LONG index = InterlockedIncrement(&g_seen_count) - 1;
    if (index >= 0 && index < (LONG)(sizeof(g_seen) / sizeof(g_seen[0]))) {
        g_seen[index] = base;
    }
}

static int PatchModule(HMODULE module, const wchar_t* module_name)
{
    BYTE* base = (BYTE*)module;
    if (!base) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;

    int patched = 0;
    DWORD count = dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress);
    for (DWORD d = 0; d < count && imp->Name; ++d, ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, "KERNEL32.dll") != 0 && _stricmp(dll, "KERNELBASE.dll") != 0) {
            continue;
        }
        PIMAGE_THUNK_DATA oft = imp->OriginalFirstThunk
                                    ? (PIMAGE_THUNK_DATA)(base + imp->OriginalFirstThunk)
                                    : (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        PIMAGE_THUNK_DATA ft = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            PIMAGE_IMPORT_BY_NAME name =
                (PIMAGE_IMPORT_BY_NAME)(base + oft->u1.AddressOfData);
            for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
                if (strcmp((const char*)name->Name, g_hooks[i].name) != 0) continue;
                if (!*g_hooks[i].original) break;
                if ((void*)ft->u1.Function == g_hooks[i].hook) break;
                DWORD old_protect = 0;
                if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE,
                                    &old_protect)) {
                    break;
                }
                ft->u1.Function = (ULONG_PTR)g_hooks[i].hook;
                DWORD ignored = 0;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old_protect, &ignored);
                ProbeLog("patched %ls!%s", module_name, g_hooks[i].name);
                ++patched;
                break;
            }
        }
    }
    return patched;
}

static void PatchAll(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snap, &entry)) {
        do {
            if ((HMODULE)entry.modBaseAddr == g_module) continue;
            if (Seen((uintptr_t)entry.modBaseAddr)) continue;
            Mark((uintptr_t)entry.modBaseAddr);
            PatchModule((HMODULE)entry.modBaseAddr, entry.szModule);
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

static DWORD WINAPI Thread(void* param)
{
    AMB_UNUSED(param);
    InitializeCriticalSection(&g_cs);

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) slash[1] = 0;
        _snwprintf_s(g_dump_dir, MAX_PATH, _TRUNCATE, L"%sPrefsDumps", path);
        CreateDirectoryW(g_dump_dir, NULL);
        wcscat_s(path, MAX_PATH, L"AMPrefsProbe.log");
        g_log = CreateFileW(path, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
        *g_hooks[i].original = (void*)GetProcAddress(kernel, g_hooks[i].name);
    }
    ProbeLog("prefs probe loaded (pid %lu)", (unsigned long)GetCurrentProcessId());

    for (int pass = 0; pass < 300; ++pass) {
        PatchAll();
        Sleep(100);
    }
    ProbeLog("scan complete");
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    AMB_UNUSED(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE h = CreateThread(NULL, 0, Thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}

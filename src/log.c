#include "log.h"

static CRITICAL_SECTION g_log_cs;
static BOOL g_log_cs_ready = FALSE;
static HANDLE g_log_handle = INVALID_HANDLE_VALUE;
static int g_log_level = 2;
static wchar_t g_log_dir[MAX_PATH] = L".";

static void Log_MakeDir(const wchar_t* dir)
{
    wchar_t tmp[MAX_PATH];
    size_t len = wcslen(dir);
    if (len >= MAX_PATH) len = MAX_PATH - 1;
    wcsncpy_s(tmp, MAX_PATH, dir, len);
    for (wchar_t* p = tmp + 3; *p; ++p) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, NULL);
}

static void Log_OpenFile(const wchar_t* path)
{
    g_log_handle = CreateFileW(path, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log_handle == INVALID_HANDLE_VALUE) g_log_handle = NULL;
}

void Log_Init(HMODULE module)
{
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_ready = TRUE;

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            *slash = 0;
            wcsncpy_s(g_log_dir, MAX_PATH, path, _TRUNCATE);
        }
    }

    wchar_t log_path[MAX_PATH];
    _snwprintf_s(log_path, MAX_PATH, _TRUNCATE, L"%s\\AMAudioBridge.log", g_log_dir);
    Log_OpenFile(log_path);
    if (!g_log_handle) {
        wchar_t base[MAX_PATH] = L"";
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            wchar_t dir[MAX_PATH];
            _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\AppleMusicASIO", base);
            Log_MakeDir(dir);
            _snwprintf_s(log_path, MAX_PATH, _TRUNCATE, L"%s\\bridge.log", dir);
            Log_OpenFile(log_path);
        }
    }

    LOGI("============================================================");
    LOGI("AppleMusicASIO bridge %u.%u.%u (pid %lu)",
         AMB_VERSION_MAJOR, AMB_VERSION_MINOR, AMB_VERSION_PATCH,
         (unsigned long)GetCurrentProcessId());
    LOGI("log directory: %ls", g_log_dir);
}

void Log_Close(void)
{
    if (g_log_cs_ready) {
        EnterCriticalSection(&g_log_cs);
        if (g_log_handle) {
            CloseHandle(g_log_handle);
            g_log_handle = NULL;
        }
        LeaveCriticalSection(&g_log_cs);
        DeleteCriticalSection(&g_log_cs);
        g_log_cs_ready = FALSE;
    }
}

void Log_SetLevel(int level)
{
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    g_log_level = level;
}

int Log_Level(void)
{
    return g_log_level;
}

const wchar_t* Log_Dir(void)
{
    return g_log_dir;
}

void Log_Write(int level, const char* file, int line, const char* fmt, ...)
{
    AMB_UNUSED(file);
    AMB_UNUSED(line);
    if (!g_log_cs_ready || level > g_log_level) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int used = vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    if (used < 0) return;
    if (used > (int)sizeof(msg) - 2) used = (int)sizeof(msg) - 2;

    char linebuf[2304];
    int prefix = snprintf(linebuf, sizeof(linebuf),
                          "%02u:%02u:%02u.%03u [%5lu] %-5s ",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                          (unsigned long)GetCurrentThreadId(),
                          level == 0 ? "ERROR" : level == 1 ? "WARN" :
                          level == 2 ? "INFO" : level == 3 ? "DEBUG" : "TRACE");
    if (prefix < 0) prefix = 0;
    if (prefix > (int)sizeof(linebuf) - 2) prefix = (int)sizeof(linebuf) - 2;

    int copy = (int)strlen(msg);
    if (copy > (int)sizeof(linebuf) - prefix - 2) copy = (int)sizeof(linebuf) - prefix - 2;
    memcpy(linebuf + prefix, msg, (size_t)copy);
    linebuf[prefix + copy] = '\n';
    linebuf[prefix + copy + 1] = 0;

    if (level <= 2) {
        OutputDebugStringA(linebuf);
    }

    EnterCriticalSection(&g_log_cs);
    if (g_log_handle) {
        DWORD written = 0;
        WriteFile(g_log_handle, linebuf, (DWORD)(prefix + copy + 1), &written, NULL);
        if (level <= 1) FlushFileBuffers(g_log_handle);
    }
    LeaveCriticalSection(&g_log_cs);
}

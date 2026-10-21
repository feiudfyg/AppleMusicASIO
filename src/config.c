#include "config.h"
#include "log.h"

AmbConfig g_cfg = {
    AMB_MODE_AUTO,
    2,
    TRUE,
    FALSE,
    L"auto",
    0,
    0,
    0,
    20,
    10,
    FALSE
};

static wchar_t g_ini_path[MAX_PATH] = L"";
static FILETIME g_ini_write_time;
static DWORD g_last_reload_check = 0;
static BOOL g_ini_has_time = FALSE;

static void IniGetString(const wchar_t* section, const wchar_t* key,
                         const wchar_t* def, wchar_t* out, DWORD count)
{
    if (!g_ini_path[0]) {
        wcsncpy_s(out, count, def, _TRUNCATE);
        return;
    }
    GetPrivateProfileStringW(section, key, def, out, count, g_ini_path);
}

static int IniGetInt(const wchar_t* section, const wchar_t* key, int def)
{
    wchar_t buf[64];
    wchar_t defbuf[64];
    _snwprintf_s(defbuf, 64, _TRUNCATE, L"%d", def);
    IniGetString(section, key, defbuf, buf, 64);
    return _wtoi(buf);
}

static BOOL IniGetBool(const wchar_t* section, const wchar_t* key, BOOL def)
{
    wchar_t buf[32];
    IniGetString(section, key, def ? L"1" : L"0", buf, 32);
    if (!_wcsicmp(buf, L"1") || !_wcsicmp(buf, L"true") || !_wcsicmp(buf, L"yes") ||
        !_wcsicmp(buf, L"on")) {
        return TRUE;
    }
    if (!_wcsicmp(buf, L"0") || !_wcsicmp(buf, L"false") || !_wcsicmp(buf, L"no") ||
        !_wcsicmp(buf, L"off")) {
        return FALSE;
    }
    return def;
}

void Config_Load(void)
{
    _snwprintf_s(g_ini_path, MAX_PATH, _TRUNCATE, L"%s\\AMAudioBridge.ini", Log_Dir());

    wchar_t mode[32];
    IniGetString(L"Bridge", L"Mode", L"auto", mode, 32);
    if (!_wcsicmp(mode, L"asio")) g_cfg.mode = AMB_MODE_ASIO;
    else if (!_wcsicmp(mode, L"exclusive") || !_wcsicmp(mode, L"wasapi")) g_cfg.mode = AMB_MODE_EXCLUSIVE;
    else if (!_wcsicmp(mode, L"monitor")) g_cfg.mode = AMB_MODE_MONITOR;
    else if (!_wcsicmp(mode, L"off")) g_cfg.mode = AMB_MODE_OFF;
    else g_cfg.mode = AMB_MODE_AUTO;

    g_cfg.log_level = IniGetInt(L"Bridge", L"LogLevel", 2);
    g_cfg.strict_bitperfect = IniGetBool(L"Bridge", L"StrictBitPerfect", TRUE);
    g_cfg.follow_source_rate = IniGetBool(L"Bridge", L"FollowSourceRate", FALSE);

    wchar_t volume[32];
    IniGetString(L"Bridge", L"VolumeControl", L"unity", volume, 32);
    g_cfg.volume_control = _wcsicmp(volume, L"app") == 0;

    IniGetString(L"ASIO", L"Driver", L"auto", g_cfg.asio_driver, 128);
    g_cfg.asio_channels = IniGetInt(L"ASIO", L"Channels", 0);
    g_cfg.asio_buffer_frames = IniGetInt(L"ASIO", L"BufferSize", 0);
    g_cfg.asio_rate = IniGetInt(L"ASIO", L"SampleRate", 0);

    g_cfg.wasapi_buffer_ms = IniGetInt(L"WASAPI", L"BufferMs", 20);
    if (g_cfg.wasapi_buffer_ms < 4) g_cfg.wasapi_buffer_ms = 4;

    Log_SetLevel(g_cfg.log_level);
    LOGI("config: %ls", g_ini_path[0] ? g_ini_path : L"(defaults)");
    LOGI("config: mode=%s log=%d strict=%d volume=%s driver=%ls channels=%d buffer=%d rate=%d wasapi_buf=%dms follow_source=%d",
         Config_ModeName(g_cfg.mode), g_cfg.log_level,
         g_cfg.strict_bitperfect ? 1 : 0,
         g_cfg.volume_control ? "app" : "unity",
         g_cfg.asio_driver, g_cfg.asio_channels, g_cfg.asio_buffer_frames,
         g_cfg.asio_rate, g_cfg.wasapi_buffer_ms,
         g_cfg.follow_source_rate ? 1 : 0);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (g_ini_path[0] && GetFileAttributesExW(g_ini_path, GetFileExInfoStandard, &fad)) {
        g_ini_write_time = fad.ftLastWriteTime;
        g_ini_has_time = TRUE;
    } else {
        g_ini_has_time = FALSE;
    }
}

BOOL Config_ReloadIfChanged(void)
{
    DWORD now = GetTickCount();
    if (now - g_last_reload_check < 1000) return FALSE;
    g_last_reload_check = now;
    if (!g_ini_path[0] || !g_ini_has_time) return FALSE;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_ini_path, GetFileExInfoStandard, &fad)) return FALSE;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_ini_write_time) == 0) return FALSE;

    LOGI("config changed on disk, reloading");
    Config_Load();
    return TRUE;
}

const char* Config_ModeName(AmbMode mode)
{
    switch (mode) {
    case AMB_MODE_ASIO: return "asio";
    case AMB_MODE_EXCLUSIVE: return "exclusive";
    case AMB_MODE_MONITOR: return "monitor";
    case AMB_MODE_OFF: return "off";
    default: return "auto";
    }
}

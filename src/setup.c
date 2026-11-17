#include "common.h"
#include "format.h"
#include "setup_common.h"

#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>

#define IDC_STATUS 1001
#define IDC_DEVICES 1002
#define IDC_INSTALL 1003
#define IDC_UNINSTALL 1004
#define IDC_APPLY 1005
#define IDC_CLOSE 1006
#define IDC_REFRESH 1007

#define MAX_DEVICES 64

typedef struct OutputChoice {
    wchar_t label[320];
    wchar_t name[160];
    wchar_t id[512];
    wchar_t backend[32];
} OutputChoice;

static HWND g_status;
static HWND g_location;
static HWND g_devices;
static HWND g_install;
static HWND g_uninstall;
static HFONT g_font;
static BOOL g_installed;
static OutputChoice g_choices[MAX_DEVICES];
static int g_choice_count;

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

static BOOL RelaunchElevated(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return FALSE;
    if (info.hProcess) CloseHandle(info.hProcess);
    return TRUE;
}

static void AddChoice(const wchar_t* backend, const wchar_t* name, const wchar_t* id)
{
    if (g_choice_count >= MAX_DEVICES || !name || !id || !*id) return;
    OutputChoice* c = &g_choices[g_choice_count++];
    wcsncpy_s(c->name, 160, name, _TRUNCATE);
    wcsncpy_s(c->id, 512, id, _TRUNCATE);
    wcsncpy_s(c->backend, 32, backend, _TRUNCATE);
    _snwprintf_s(c->label, 320, _TRUNCATE, L"%ls  [%ls]",
                 c->name[0] ? c->name : id, backend);
}

static BOOL ContainsNoCase(const wchar_t* text, const wchar_t* part)
{
    if (!text || !part || !*part) return FALSE;
    size_t length = wcslen(part);
    for (; *text; ++text) {
        if (!_wcsnicmp(text, part, length)) return TRUE;
    }
    return FALSE;
}

static BOOL TestExclusive(IAudioClient* client, const WAVEFORMATEX* wf)
{
    if (!client || !wf) return FALSE;
    WAVEFORMATEX* closest = NULL;
    HRESULT hr = client->lpVtbl->IsFormatSupported(
        client, AUDCLNT_SHAREMODE_EXCLUSIVE, wf, &closest);
    if (closest) CoTaskMemFree(closest);
    return hr == S_OK;
}

static BOOL WasapiExclusiveSupported(IAudioClient* client)
{
    if (!client) return FALSE;

    WAVEFORMATEX* mix = NULL;
    if (SUCCEEDED(client->lpVtbl->GetMixFormat(client, &mix)) && mix) {
        if (TestExclusive(client, mix)) {
            CoTaskMemFree(mix);
            return TRUE;
        }
        CoTaskMemFree(mix);
    }

    static const UINT32 rates[] = {44100, 48000, 96000, 192000};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        AmbAudioFormat formats[4];
        Format_Make(&formats[0], rates[r], 2, AMB_SAMPLE_F32, 32);
        Format_Make(&formats[1], rates[r], 2, AMB_SAMPLE_S32, 32);
        Format_Make(&formats[2], rates[r], 2, AMB_SAMPLE_S32, 24);
        Format_Make(&formats[3], rates[r], 2, AMB_SAMPLE_S16, 16);
        for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
            if (TestExclusive(client, &formats[i].wfx.Format)) return TRUE;
        }
    }
    return FALSE;
}

static void EnumerateWasapi(void)
{
    IMMDeviceEnumerator* enumr = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                                CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator,
                                (void**)&enumr)) || !enumr) {
        return;
    }

    IMMDeviceCollection* collection = NULL;
    if (SUCCEEDED(enumr->lpVtbl->EnumAudioEndpoints(enumr, eRender,
                                                    DEVICE_STATE_ACTIVE,
                                                    &collection)) &&
        collection) {
        UINT count = 0;
        collection->lpVtbl->GetCount(collection, &count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = NULL;
            if (FAILED(collection->lpVtbl->Item(collection, i, &device)) || !device) continue;

            LPWSTR id = NULL;
            if (SUCCEEDED(device->lpVtbl->GetId(device, &id)) && id) {
                IPropertyStore* store = NULL;
                wchar_t name[160] = L"";
                if (SUCCEEDED(device->lpVtbl->OpenPropertyStore(device, STGM_READ, &store)) &&
                    store) {
                    PROPVARIANT value;
                    PropVariantInit(&value);
                    if (SUCCEEDED(store->lpVtbl->GetValue(store, &PKEY_Device_FriendlyName,
                                                          &value)) &&
                        value.vt == VT_LPWSTR && value.pwszVal) {
                        wcsncpy_s(name, 160, value.pwszVal, _TRUNCATE);
                    }
                    PropVariantClear(&value);
                    store->lpVtbl->Release(store);
                }

                BOOL loopback = ContainsNoCase(name, L"loopback") ||
                                ContainsNoCase(id, L"loopback");
                IAudioClient* client = NULL;
                if (!loopback &&
                    SUCCEEDED(device->lpVtbl->Activate(device, &IID_IAudioClient,
                                                       CLSCTX_INPROC_SERVER, NULL,
                                                       (void**)&client)) &&
                    client) {
                    if (WasapiExclusiveSupported(client)) {
                        AddChoice(L"WASAPI exclusive",
                                  name[0] ? name : id, id);
                    }
                    client->lpVtbl->Release(client);
                }
                CoTaskMemFree(id);
            }
            device->lpVtbl->Release(device);
        }
        collection->lpVtbl->Release(collection);
    }
    enumr->lpVtbl->Release(enumr);
}

static void EnumerateAsio(void)
{
    HKEY root = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD i = 0;; ++i) {
        wchar_t key[128];
        DWORD key_len = (DWORD)(sizeof(key) / sizeof(key[0]));
        LONG rc = RegEnumKeyExW(root, i, key, &key_len, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;

        HKEY sub = NULL;
        if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &sub) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t description[160] = L"";
        wchar_t clsid[128] = L"";
        DWORD type = 0;
        DWORD bytes = (DWORD)sizeof(description);
        RegQueryValueExW(sub, L"Description", NULL, &type, (BYTE*)description, &bytes);
        bytes = (DWORD)sizeof(clsid);
        RegQueryValueExW(sub, L"CLSID", NULL, &type, (BYTE*)clsid, &bytes);
        RegCloseKey(sub);

        if (clsid[0]) {
            AddChoice(L"ASIO", description[0] ? description : key, key);
        }
    }
    RegCloseKey(root);
}

static void EnumerateOutputs(void)
{
    g_choice_count = 0;
    EnumerateAsio();
    EnumerateWasapi();
}

static int SelectedChoice(void)
{
    if (!g_devices) return -1;
    LRESULT sel = SendMessageW(g_devices, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return -1;
    LRESULT data = SendMessageW(g_devices, LB_GETITEMDATA, (WPARAM)sel, 0);
    if (data == LB_ERR || data < 0 || data >= g_choice_count) return -1;
    return (int)data;
}

static const wchar_t* BackendMode(const OutputChoice* c)
{
    return !_wcsicmp(c->backend, L"ASIO") ? L"asio" : L"wasapi";
}

static void ChoiceDeviceValue(const OutputChoice* c, wchar_t* out, size_t count)
{
    _snwprintf_s(out, count, _TRUNCATE, L"%ls:%ls", BackendMode(c), c->id);
}

static void DefaultOutputValue(wchar_t* out, size_t count)
{
    out[0] = 0;
    IMMDeviceEnumerator* enumr = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                                CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator,
                                (void**)&enumr)) || !enumr) {
        return;
    }
    IMMDevice* device = NULL;
    if (SUCCEEDED(enumr->lpVtbl->GetDefaultAudioEndpoint(enumr, eRender, eConsole,
                                                          &device)) && device) {
        LPWSTR id = NULL;
        if (SUCCEEDED(device->lpVtbl->GetId(device, &id)) && id) {
            _snwprintf_s(out, count, _TRUNCATE, L"wasapi:%ls", id);
            CoTaskMemFree(id);
        }
        device->lpVtbl->Release(device);
    }
    enumr->lpVtbl->Release(enumr);
}

static void RefreshDeviceList(const wchar_t* selected)
{
    SendMessageW(g_devices, LB_RESETCONTENT, 0, 0);
    int select = -1;
    for (int i = 0; i < g_choice_count; ++i) {
        LRESULT row = SendMessageW(g_devices, LB_ADDSTRING, 0,
                                   (LPARAM)g_choices[i].label);
        if (row != LB_ERR) {
            SendMessageW(g_devices, LB_SETITEMDATA, (WPARAM)row, (LPARAM)i);
        }
        if (selected && selected[0]) {
            wchar_t value[768];
            ChoiceDeviceValue(&g_choices[i], value, 768);
            if (!_wcsicmp(value, selected)) select = i;
        }
    }
    if (select < 0 && g_choice_count > 0) select = 0;
    if (select >= 0) SendMessageW(g_devices, LB_SETCURSEL, (WPARAM)select, 0);
}

static void SetFont(HWND control)
{
    if (g_font) SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static void RefreshStatus(void)
{
    g_installed = Setup_IsInstalled();

    wchar_t device[768];
    if (!Setup_GetDevice(device, 768)) {
        wchar_t mode[32];
        BOOL have_mode = Setup_GetMode(mode, 32);

        if (have_mode && !_wcsicmp(mode, L"asio")) {
            for (int i = 0; i < g_choice_count; ++i) {
                if (!_wcsicmp(BackendMode(&g_choices[i]), L"asio")) {
                    ChoiceDeviceValue(&g_choices[i], device, 768);
                    break;
                }
            }
        }
        if (!device[0]) DefaultOutputValue(device, 768);
        if (!device[0] && have_mode) {
            for (int i = 0; i < g_choice_count; ++i) {
                const wchar_t* candidate = BackendMode(&g_choices[i]);
                if ((!_wcsicmp(mode, L"asio") && !_wcsicmp(candidate, L"asio")) ||
                    (_wcsicmp(mode, L"asio") && !_wcsicmp(candidate, L"wasapi"))) {
                    ChoiceDeviceValue(&g_choices[i], device, 768);
                    break;
                }
            }
        }
    }
    RefreshDeviceList(device);

    wchar_t text[512];
    _snwprintf_s(text, 512, _TRUNCATE,
                 L"Status: %ls",
                 g_installed ? L"Installed" : L"Not installed");
    SetWindowTextW(g_status, text);
    _snwprintf_s(text, 512, _TRUNCATE,
                 L"Location: %ls", Setup_InstallDir());
    SetWindowTextW(g_location, text);
    EnableWindow(g_install, TRUE);
    SetWindowTextW(g_install, g_installed ? L"Reinstall" : L"Install");
    EnableWindow(g_uninstall, g_installed);
}

static void InstallBridge(HWND hwnd)
{
    int index = SelectedChoice();
    if (index < 0) {
        MessageBoxW(hwnd, L"Select an output device first.",
                    L"Apple Music ASIO", MB_ICONERROR | MB_OK);
        return;
    }

    const OutputChoice* choice = &g_choices[index];
    wchar_t selected[768];
    ChoiceDeviceValue(choice, selected, 768);

    wchar_t message[1024];
    if (!Setup_Install(BackendMode(choice), message, 1024)) {
        MessageBoxW(hwnd, message, L"Apple Music ASIO", MB_ICONERROR | MB_OK);
        return;
    }
    Setup_SetDevice(selected);
    MessageBoxW(hwnd, message, L"Apple Music ASIO", MB_ICONINFORMATION | MB_OK);
    RefreshStatus();
}

static void UninstallBridge(HWND hwnd)
{
    if (MessageBoxW(hwnd, L"Uninstall Apple Music ASIO?", L"Apple Music ASIO",
                    MB_ICONQUESTION | MB_YESNO) != IDYES) {
        return;
    }

    wchar_t message[1024];
    if (!Setup_Uninstall(message, 1024)) {
        MessageBoxW(hwnd, message, L"Apple Music ASIO", MB_ICONERROR | MB_OK);
        return;
    }
    MessageBoxW(hwnd, message, L"Apple Music ASIO", MB_ICONINFORMATION | MB_OK);
    RefreshStatus();
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HWND title = CreateWindowW(L"STATIC", L"Apple Music ASIO",
                                   WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                   18, 14, 398, 24, hwnd, NULL, NULL, NULL);
        SetFont(title);

        g_status = CreateWindowW(L"STATIC", L"",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
                                 18, 44, 398, 20, hwnd, (HMENU)IDC_STATUS, NULL, NULL);
        SetFont(g_status);

        g_location = CreateWindowW(L"STATIC", L"",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX,
                                   18, 66, 398, 20, hwnd, NULL, NULL, NULL);
        SetFont(g_location);

        HWND group = CreateWindowW(L"BUTTON", L"Output device",
                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                   18, 98, 604, 184, hwnd, NULL, NULL, NULL);
        SetFont(group);

        g_devices = CreateWindowW(L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                  LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  30, 120, 580, 150, hwnd, (HMENU)IDC_DEVICES, NULL, NULL);
        SetFont(g_devices);

        EnumerateOutputs();

        g_install = CreateWindowW(L"BUTTON", L"Install",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  18, 294, 140, 28, hwnd, (HMENU)IDC_INSTALL, NULL, NULL);
        g_uninstall = CreateWindowW(L"BUTTON", L"Uninstall",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    168, 294, 140, 28, hwnd, (HMENU)IDC_UNINSTALL, NULL, NULL);
        HWND refresh = CreateWindowW(L"BUTTON", L"Refresh",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     318, 294, 100, 28, hwnd, (HMENU)IDC_REFRESH, NULL, NULL);
        HWND apply = CreateWindowW(L"BUTTON", L"Apply",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                   468, 294, 70, 28, hwnd, (HMENU)IDC_APPLY, NULL, NULL);
        HWND close = CreateWindowW(L"BUTTON", L"Close",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   548, 294, 74, 28, hwnd, (HMENU)IDC_CLOSE, NULL, NULL);
        SetFont(g_install);
        SetFont(g_uninstall);
        SetFont(refresh);
        SetFont(apply);
        SetFont(close);

        HWND hint = CreateWindowW(L"STATIC", L"Restart Apple Music after a change.",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
                                  18, 332, 604, 20, hwnd, NULL, NULL, NULL);
        SetFont(hint);
        RefreshStatus();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INSTALL:
            InstallBridge(hwnd);
            return 0;
        case IDC_UNINSTALL:
            UninstallBridge(hwnd);
            return 0;
        case IDC_REFRESH:
            EnumerateOutputs();
            RefreshStatus();
            return 0;
        case IDC_APPLY: {
            int index = SelectedChoice();
            if (index < 0) {
                MessageBoxW(hwnd, L"Select an output device first.",
                            L"Apple Music ASIO", MB_ICONERROR | MB_OK);
                return 0;
            }
            const OutputChoice* choice = &g_choices[index];
            wchar_t selected[768];
            ChoiceDeviceValue(choice, selected, 768);
            wchar_t current[768] = L"";
            Setup_GetDevice(current, 768);
            if (_wcsicmp(current, selected) != 0) {
                if (!Setup_SetMode(BackendMode(choice)) ||
                    !Setup_SetDevice(selected)) {
                    MessageBoxW(hwnd, L"Could not save the output device.",
                                L"Apple Music ASIO", MB_ICONERROR | MB_OK);
                    return 0;
                }
                MessageBoxW(hwnd, L"Settings saved. Restart Apple Music to apply.",
                            L"Apple Music ASIO", MB_ICONINFORMATION | MB_OK);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case IDC_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show)
{
    (void)previous;
    (void)command_line;
    SetProcessDPIAware();

    if (!IsElevated()) {
        if (!RelaunchElevated() && GetLastError() != ERROR_CANCELLED) {
            MessageBoxW(NULL, L"Administrator privileges are required.",
                        L"Apple Music ASIO", MB_ICONERROR | MB_OK);
        }
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AppleMusicASIOSetup";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Apple Music ASIO",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 650, 410,
                                NULL, NULL, instance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

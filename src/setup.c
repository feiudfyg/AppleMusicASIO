#include "common.h"
#include "setup_common.h"

#include <shellapi.h>

#define IDC_STATUS 1001
#define IDC_WASAPI 1002
#define IDC_ASIO 1003
#define IDC_INSTALL 1004
#define IDC_UNINSTALL 1005
#define IDC_APPLY 1006
#define IDC_CLOSE 1007

static HWND g_status;
static HWND g_location;
static HWND g_wasapi;
static HWND g_asio;
static HWND g_install;
static HWND g_uninstall;
static HFONT g_font;
static BOOL g_installed;

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

static const wchar_t* SelectedMode(void)
{
    return SendMessageW(g_wasapi, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"wasapi" : L"asio";
}

static void SetFont(HWND control)
{
    if (g_font) SendMessageW(control, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static void RefreshStatus(void)
{
    g_installed = Setup_IsInstalled();

    wchar_t mode[32];
    Setup_GetMode(mode, 32);
    BOOL wasapi = !_wcsicmp(mode, L"wasapi") || !_wcsicmp(mode, L"exclusive");
    SendMessageW(g_wasapi, BM_SETCHECK, wasapi ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_asio, BM_SETCHECK, wasapi ? BST_UNCHECKED : BST_CHECKED, 0);

    wchar_t text[512];
    _snwprintf_s(text, 512, _TRUNCATE,
                 L"Status: %ls",
                 g_installed ? L"Installed" : L"Not installed");
    SetWindowTextW(g_status, text);
    _snwprintf_s(text, 512, _TRUNCATE,
                 L"Location: %ls", Setup_InstallDir());
    SetWindowTextW(g_location, text);
    EnableWindow(g_install, !g_installed);
    EnableWindow(g_uninstall, g_installed);
}

static void InstallBridge(HWND hwnd)
{
    wchar_t message[1024];
    if (!Setup_Install(SelectedMode(), message, 1024)) {
        MessageBoxW(hwnd, message, L"Apple Music ASIO", MB_ICONERROR | MB_OK);
        return;
    }
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

        HWND group = CreateWindowW(L"BUTTON", L"Output mode",
                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                   18, 100, 398, 84, hwnd, NULL, NULL, NULL);
        SetFont(group);

        g_wasapi = CreateWindowW(L"BUTTON", L"WASAPI exclusive",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 BS_AUTORADIOBUTTON | WS_GROUP,
                                 34, 122, 180, 22, hwnd, (HMENU)IDC_WASAPI, NULL, NULL);
        g_asio = CreateWindowW(L"BUTTON", L"ASIO",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                               34, 148, 180, 22, hwnd, (HMENU)IDC_ASIO, NULL, NULL);
        SetFont(g_wasapi);
        SetFont(g_asio);

        g_install = CreateWindowW(L"BUTTON", L"Install",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  236, 112, 160, 26, hwnd, (HMENU)IDC_INSTALL, NULL, NULL);
        g_uninstall = CreateWindowW(L"BUTTON", L"Uninstall",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    236, 146, 160, 26, hwnd, (HMENU)IDC_UNINSTALL, NULL, NULL);
        SetFont(g_install);
        SetFont(g_uninstall);

        HWND hint = CreateWindowW(L"STATIC", L"Restart Apple Music after a change.",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
                                  18, 188, 398, 20, hwnd, NULL, NULL, NULL);
        SetFont(hint);

        HWND apply = CreateWindowW(L"BUTTON", L"Apply",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                   206, 208, 90, 28, hwnd, (HMENU)IDC_APPLY, NULL, NULL);
        HWND close = CreateWindowW(L"BUTTON", L"Close",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   306, 208, 90, 28, hwnd, (HMENU)IDC_CLOSE, NULL, NULL);
        SetFont(apply);
        SetFont(close);

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
        case IDC_APPLY: {
            wchar_t mode[32];
            Setup_GetMode(mode, 32);
            const wchar_t* selected = SelectedMode();
            if (_wcsicmp(mode, selected) != 0) {
                if (!Setup_SetMode(selected)) {
                    MessageBoxW(hwnd, L"Could not save the output mode.",
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
                                CW_USEDEFAULT, CW_USEDEFAULT, 450, 300,
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

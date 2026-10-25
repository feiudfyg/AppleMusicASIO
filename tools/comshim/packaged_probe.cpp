#include "comshim_common.h"
#include <appmodel.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("PackagedComProbe start");
    UINT32 fullNameLength = 0;
    LONG packageResult = GetCurrentPackageFullName(&fullNameLength, nullptr);
    ComLog("PackagedComProbe GetCurrentPackageFullName probe result=%ld length=%lu",
           packageResult, fullNameLength);
    if (fullNameLength > 0 && fullNameLength < 1024) {
        wchar_t fullName[1024]{};
        packageResult = GetCurrentPackageFullName(&fullNameLength, fullName);
        ComLog("PackagedComProbe package result=%ld name=%ls", packageResult, fullName);
    }

    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)) {
            ComLog("PackagedComProbe TokenElevation=%lu", elevation.TokenIsElevated);
        }
        bytes = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
        if (bytes > 0) {
            auto* label = static_cast<TOKEN_MANDATORY_LABEL*>(HeapAlloc(GetProcessHeap(), 0, bytes));
            if (label) {
                if (GetTokenInformation(token, TokenIntegrityLevel, label, bytes, &bytes)) {
                    DWORD rid = *GetSidSubAuthority(
                        label->Label.Sid,
                        static_cast<DWORD>(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
                    ComLog("PackagedComProbe integrity RID=%lu", rid);
                }
                HeapFree(GetProcessHeap(), 0, label);
            }
        }
        CloseHandle(token);
    } else {
        ComLog("PackagedComProbe OpenProcessToken failed=%lu", GetLastError());
    }
    Sleep(8000);
    ComLog("PackagedComProbe exit");
    return 0;
}

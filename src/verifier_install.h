#pragma once

#include <windows.h>

BOOL Verifier_IsInstalled(void);
BOOL Verifier_GetMachineDir(wchar_t* out, DWORD count);
BOOL Verifier_Install(const wchar_t* sourceDir, wchar_t* message, size_t count);
BOOL Verifier_Uninstall(wchar_t* message, size_t count);

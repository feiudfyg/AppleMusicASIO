#pragma once

#include "common.h"

BOOL Setup_IsInstalled(void);
BOOL Setup_GetMode(wchar_t* mode, size_t count);
BOOL Setup_SetMode(const wchar_t* mode);
BOOL Setup_Install(const wchar_t* mode, wchar_t* message, size_t count);
BOOL Setup_Uninstall(wchar_t* message, size_t count);
BOOL Setup_EnsurePreferencesJunction(BOOL repair, wchar_t* message, size_t count);
const wchar_t* Setup_InstallDir(void);

#include <windows.h>
#include <stdio.h>
#include "setup_common.h"

int wmain(void)
{
    wchar_t message[2048];
    BOOL ok = Setup_EnsurePreferencesJunction(TRUE, message, 2048);
    wprintf(L"ok=%d: %ls\n", ok, message);
    return ok ? 0 : 1;
}

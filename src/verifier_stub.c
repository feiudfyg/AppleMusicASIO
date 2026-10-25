#include "verifier_install.h"

BOOL Verifier_IsInstalled(void)
{
    return FALSE;
}

BOOL Verifier_GetMachineDir(wchar_t* out, DWORD count)
{
    (void)out;
    (void)count;
    return FALSE;
}

BOOL Verifier_Install(const wchar_t* sourceDir, wchar_t* message, size_t count)
{
    (void)sourceDir;
    (void)message;
    (void)count;
    return FALSE;
}

BOOL Verifier_Uninstall(wchar_t* message, size_t count)
{
    (void)message;
    (void)count;
    return FALSE;
}

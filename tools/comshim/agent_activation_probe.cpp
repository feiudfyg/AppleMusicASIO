#include "comshim_common.h"
#include <appmodel.h>
#include <shobjidl.h>

static const CLSID kApplicationActivationManager = {
    0x45ba127d, 0x10a8, 0x46ea, {0x8a, 0xb7, 0x56, 0xea, 0x90, 0x78, 0x94, 0x3c}
};
static const IID kIidApplicationActivationManager = {
    0x2e941141, 0x7f97, 0x4756, {0xba, 0x1d, 0x9d, 0xec, 0xde, 0x89, 0x4a, 0x3d}
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("AgentActivationProbe start");
    CoLogPackageIdentity();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("AgentActivationProbe CoInitializeEx hr=0x%08lx", hr);

    IApplicationActivationManager* manager = nullptr;
    hr = CoCreateInstance(kApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                          kIidApplicationActivationManager,
                          reinterpret_cast<void**>(&manager));
    ComLog("AgentActivationProbe activation manager hr=0x%08lx ptr=%p", hr, manager);
    if (manager) {
        DWORD pid = 0;
        hr = manager->ActivateApplication(
            L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!AMPLibraryAgent",
            nullptr, AO_NONE, &pid);
        ComLog("AgentActivationProbe ActivateApplication hr=0x%08lx pid=%lu", hr, pid);
        manager->Release();
    }

    Sleep(2000);
    HANDLE guard = CreateMutexW(nullptr, FALSE, L"Local\\AppleMusicASIOAgentActivationProbe");
    if (guard && GetLastError() == ERROR_ALREADY_EXISTS) {
        ComLog("AgentActivationProbe recursive activation blocked");
        CloseHandle(guard);
        return 0;
    }

    IUnknown* real = nullptr;
    hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IUnknown, reinterpret_cast<void**>(&real));
    ComLog("AgentActivationProbe CoCreateInstance hr=0x%08lx ptr=%p", hr, real);
    if (real) real->Release();
    if (guard) CloseHandle(guard);
    Sleep(5000);
    ComLog("AgentActivationProbe exit");
    return 0;
}

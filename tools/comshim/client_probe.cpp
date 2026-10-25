#include "comshim_common.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ComLog("ClientProbe start");
    CoLogPackageIdentity();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComLog("ClientProbe CoInitializeEx hr=0x%08lx", hr);

    // Give the debugger time to attach before COM resolves the class.
    Sleep(5000);

    IUnknown* object = nullptr;
    hr = CoCreateInstance(kAppleMusicLibrary, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IUnknown, reinterpret_cast<void**>(&object));
    ComLog("ClientProbe CoCreateInstance hr=0x%08lx ptr=%p", hr, object);
    if (object) object->Release();

    Sleep(10000);
    CoUninitialize();
    ComLog("ClientProbe exit");
    return 0;
}

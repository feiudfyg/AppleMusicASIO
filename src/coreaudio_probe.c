#include "common.h"
#include <tlhelp32.h>

typedef int32_t OSStatus;
typedef void* AudioConverterRef;
typedef void* AudioUnit;
typedef void* AudioQueueRef;
typedef void* ExtAudioFileRef;
typedef void* AudioFileID;
typedef void* AudioFileStreamID;
typedef void* CFURLRef;
typedef void* CFRunLoopRef;
typedef void* CFStringRef;

typedef struct AudioStreamBasicDescription {
    double mSampleRate;
    uint32_t mFormatID;
    uint32_t mFormatFlags;
    uint32_t mBytesPerPacket;
    uint32_t mFramesPerPacket;
    uint32_t mBytesPerFrame;
    uint32_t mChannelsPerFrame;
    uint32_t mBitsPerChannel;
    uint32_t mReserved;
} AudioStreamBasicDescription;

typedef OSStatus (*PFN_AudioConverterNew)(const AudioStreamBasicDescription*,
                                          const AudioStreamBasicDescription*,
                                          AudioConverterRef*);
typedef OSStatus (*PFN_AudioConverterNewSpecific)(const AudioStreamBasicDescription*,
                                                  const AudioStreamBasicDescription*,
                                                  uint32_t, const void*, AudioConverterRef*);
typedef OSStatus (*PFN_AudioUnitSetProperty)(AudioUnit, uint32_t, uint32_t, uint32_t,
                                             const void*, uint32_t);
typedef OSStatus (*PFN_AudioQueueNewOutput)(const AudioStreamBasicDescription*, void*, void*,
                                            CFRunLoopRef, CFStringRef, uint32_t,
                                            AudioQueueRef*);
typedef OSStatus (*PFN_ExtAudioFileOpenURL)(CFURLRef, ExtAudioFileRef*);
typedef OSStatus (*PFN_ExtAudioFileGetProperty)(ExtAudioFileRef, uint32_t, uint32_t*, void*);
typedef OSStatus (*PFN_AudioFileOpenURL)(CFURLRef, int8_t, uint32_t, AudioFileID*);
typedef OSStatus (*PFN_AudioFileGetProperty)(AudioFileID, uint32_t, uint32_t*, void*);
typedef OSStatus (*PFN_AudioFileStreamGetProperty)(AudioFileStreamID, uint32_t, uint32_t*, void*);

static HMODULE g_module = NULL;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;

static PFN_AudioConverterNew g_converter_new = NULL;
static PFN_AudioConverterNewSpecific g_converter_new_specific = NULL;
static PFN_AudioUnitSetProperty g_unit_set_property = NULL;
static PFN_AudioQueueNewOutput g_queue_new_output = NULL;
static PFN_ExtAudioFileOpenURL g_ext_open = NULL;
static PFN_ExtAudioFileGetProperty g_ext_get = NULL;
static PFN_AudioFileOpenURL g_file_open = NULL;
static PFN_AudioFileGetProperty g_file_get = NULL;
static PFN_AudioFileStreamGetProperty g_stream_get = NULL;

static uintptr_t g_patched_modules[256];
static volatile LONG g_patched_count = 0;

static void ProbeLog(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int used = vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    if (used < 0) return;
    if (used > (int)sizeof(msg) - 2) used = (int)sizeof(msg) - 2;

    char line[1200];
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%5lu] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     (unsigned long)GetCurrentThreadId());
    if (n < 0) n = 0;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    int copy = used;
    if (copy > (int)sizeof(line) - n - 2) copy = (int)sizeof(line) - n - 2;
    memcpy(line + n, msg, (size_t)copy);
    line[n + copy] = '\n';
    line[n + copy + 1] = 0;

    EnterCriticalSection(&g_log_cs);
    DWORD written = 0;
    WriteFile(g_log, line, (DWORD)(n + copy + 1), &written, NULL);
    LeaveCriticalSection(&g_log_cs);
}

static void FormatFourCC(uint32_t id, char out[5])
{
    out[0] = (char)((id >> 24) & 0xFF);
    out[1] = (char)((id >> 16) & 0xFF);
    out[2] = (char)((id >> 8) & 0xFF);
    out[3] = (char)(id & 0xFF);
    out[4] = 0;
    for (int i = 0; i < 4; ++i) {
        if (out[i] < 32 || out[i] > 126) out[i] = '.';
    }
}

static void LogAsbd(const char* what, const AudioStreamBasicDescription* a)
{
    if (!a) {
        ProbeLog("%s: <null>", what);
        return;
    }
    char fourcc[5];
    FormatFourCC(a->mFormatID, fourcc);
    ProbeLog("%s: %.0f Hz, fmt='%s' flags=0x%08lx, %lu ch, %lu bits, "
             "bytesPacket=%lu framesPacket=%lu bytesFrame=%lu",
             what, a->mSampleRate, fourcc, (unsigned long)a->mFormatFlags,
             (unsigned long)a->mChannelsPerFrame, (unsigned long)a->mBitsPerChannel,
             (unsigned long)a->mBytesPerPacket, (unsigned long)a->mFramesPerPacket,
             (unsigned long)a->mBytesPerFrame);
}

static OSStatus Hook_AudioConverterNew(const AudioStreamBasicDescription* src,
                                       const AudioStreamBasicDescription* dst,
                                       AudioConverterRef* out)
{
    LogAsbd("AudioConverterNew source", src);
    LogAsbd("AudioConverterNew destination", dst);
    return g_converter_new ? g_converter_new(src, dst, out) : -1;
}

static OSStatus Hook_AudioConverterNewSpecific(const AudioStreamBasicDescription* src,
                                               const AudioStreamBasicDescription* dst,
                                               uint32_t manufacturer, const void* classID,
                                               AudioConverterRef* out)
{
    LogAsbd("AudioConverterNewSpecific source", src);
    LogAsbd("AudioConverterNewSpecific destination", dst);
    return g_converter_new_specific
               ? g_converter_new_specific(src, dst, manufacturer, classID, out)
               : -1;
}

static OSStatus Hook_AudioUnitSetProperty(AudioUnit unit, uint32_t id, uint32_t scope,
                                          uint32_t element, const void* data, uint32_t size)
{
    if (id == 8 && data && size >= sizeof(AudioStreamBasicDescription)) {
        char what[96];
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "AudioUnitSetProperty(StreamFormat) scope=%lu element=%lu",
                    (unsigned long)scope, (unsigned long)element);
        LogAsbd(what, (const AudioStreamBasicDescription*)data);
    }
    return g_unit_set_property ? g_unit_set_property(unit, id, scope, element, data, size) : -1;
}

static OSStatus Hook_AudioQueueNewOutput(const AudioStreamBasicDescription* fmt,
                                         void* callback, void* user, CFRunLoopRef loop,
                                         CFStringRef mode, uint32_t flags, AudioQueueRef* out)
{
    LogAsbd("AudioQueueNewOutput format", fmt);
    return g_queue_new_output
               ? g_queue_new_output(fmt, callback, user, loop, mode, flags, out)
               : -1;
}

static void LogFileFormat(ExtAudioFileRef file)
{
    if (!g_ext_get) return;
    AudioStreamBasicDescription asbd;
    uint32_t size = sizeof(asbd);
    memset(&asbd, 0, sizeof(asbd));
    OSStatus status = g_ext_get(file, 0x66666D74 /* 'ffmt' */, &size, &asbd);
    if (status == 0) {
        LogAsbd("ExtAudioFile file format", &asbd);
    } else {
        ProbeLog("ExtAudioFileGetProperty('ffmt') failed: %ld", (long)status);
    }
}

static OSStatus Hook_ExtAudioFileOpenURL(CFURLRef url, ExtAudioFileRef* out)
{
    OSStatus status = g_ext_open ? g_ext_open(url, out) : -1;
    ProbeLog("ExtAudioFileOpenURL -> %ld", (long)status);
    if (status == 0 && out && *out) LogFileFormat(*out);
    return status;
}

static OSStatus Hook_ExtAudioFileGetProperty(ExtAudioFileRef file, uint32_t id,
                                             uint32_t* size, void* data)
{
    OSStatus status = g_ext_get ? g_ext_get(file, id, size, data) : -1;
    if (status == 0 && id == 0x66666D74 && data && size &&
        *size >= sizeof(AudioStreamBasicDescription)) {
        LogAsbd("ExtAudioFileGetProperty(file format)", (const AudioStreamBasicDescription*)data);
    }
    return status;
}

static OSStatus Hook_AudioFileOpenURL(CFURLRef url, int8_t permissions, uint32_t type,
                                      AudioFileID* out)
{
    OSStatus status = g_file_open ? g_file_open(url, permissions, type, out) : -1;
    ProbeLog("AudioFileOpenURL -> %ld", (long)status);
    if (status == 0 && out && *out && g_file_get) {
        AudioStreamBasicDescription asbd;
        uint32_t size = sizeof(asbd);
        memset(&asbd, 0, sizeof(asbd));
        OSStatus s = g_file_get(*out, 0x64666D74 /* 'dfmt' */, &size, &asbd);
        if (s == 0) LogAsbd("AudioFile data format", &asbd);
    }
    return status;
}

static OSStatus Hook_AudioFileStreamGetProperty(AudioFileStreamID stream, uint32_t id,
                                                uint32_t* size, void* data)
{
    OSStatus status = g_stream_get ? g_stream_get(stream, id, size, data) : -1;
    if (status == 0 && id == 0x64666D74 && data && size &&
        *size >= sizeof(AudioStreamBasicDescription)) {
        LogAsbd("AudioFileStream data format", (const AudioStreamBasicDescription*)data);
    }
    return status;
}

typedef struct {
    const char* name;
    void* hook;
    void** original;
} HookSpec;

static const HookSpec g_hooks[] = {
    {"AudioConverterNew", (void*)Hook_AudioConverterNew, (void**)&g_converter_new},
    {"AudioConverterNewSpecific", (void*)Hook_AudioConverterNewSpecific,
     (void**)&g_converter_new_specific},
    {"AudioUnitSetProperty", (void*)Hook_AudioUnitSetProperty, (void**)&g_unit_set_property},
    {"AudioQueueNewOutput", (void*)Hook_AudioQueueNewOutput, (void**)&g_queue_new_output},
    {"ExtAudioFileOpenURL", (void*)Hook_ExtAudioFileOpenURL, (void**)&g_ext_open},
    {"ExtAudioFileGetProperty", (void*)Hook_ExtAudioFileGetProperty, (void**)&g_ext_get},
    {"AudioFileOpenURL", (void*)Hook_AudioFileOpenURL, (void**)&g_file_open},
    {"AudioFileStreamGetProperty", (void*)Hook_AudioFileStreamGetProperty,
     (void**)&g_stream_get},
};

static BOOL ModuleAlreadyPatched(uintptr_t base)
{
    LONG count = g_patched_count;
    for (LONG i = 0; i < count; ++i) {
        if (g_patched_modules[i] == base) return TRUE;
    }
    return FALSE;
}

static void MarkModulePatched(uintptr_t base)
{
    LONG index = InterlockedIncrement(&g_patched_count) - 1;
    if (index >= 0 && index < (LONG)(sizeof(g_patched_modules) / sizeof(g_patched_modules[0]))) {
        g_patched_modules[index] = base;
    }
}

static int PatchModuleIat(HMODULE module, const wchar_t* module_name)
{
    BYTE* base = (BYTE*)module;
    if (!base) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;

    int patched = 0;
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, "CoreAudioToolbox.dll") != 0) continue;
        PIMAGE_THUNK_DATA oft = imp->OriginalFirstThunk
                                    ? (PIMAGE_THUNK_DATA)(base + imp->OriginalFirstThunk)
                                    : (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        PIMAGE_THUNK_DATA ft = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            PIMAGE_IMPORT_BY_NAME name =
                (PIMAGE_IMPORT_BY_NAME)(base + oft->u1.AddressOfData);
            for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
                if (strcmp((const char*)name->Name, g_hooks[i].name) != 0) continue;
                if (!*g_hooks[i].original) break;
                if ((void*)ft->u1.Function == g_hooks[i].hook) break;
                DWORD old_protect = 0;
                if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE,
                                    &old_protect)) {
                    break;
                }
                ft->u1.Function = (ULONG_PTR)g_hooks[i].hook;
                DWORD ignored = 0;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old_protect, &ignored);
                ProbeLog("patched %ls!%s", module_name, g_hooks[i].name);
                ++patched;
                break;
            }
        }
    }
    return patched;
}

static void PatchAllModules(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snap, &entry)) {
        do {
            if ((HMODULE)entry.modBaseAddr == g_module) continue;
            if (ModuleAlreadyPatched((uintptr_t)entry.modBaseAddr)) continue;
            MarkModulePatched((uintptr_t)entry.modBaseAddr);
            PatchModuleIat((HMODULE)entry.modBaseAddr, entry.szModule);
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

static void ResolveOriginals(void)
{
    HMODULE coreaudio = GetModuleHandleW(L"CoreAudioToolbox.dll");
    if (!coreaudio) return;
    for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
        void* fn = (void*)GetProcAddress(coreaudio, g_hooks[i].name);
        if (fn && !*g_hooks[i].original) *g_hooks[i].original = fn;
    }
    g_file_get = (PFN_AudioFileGetProperty)GetProcAddress(coreaudio, "AudioFileGetProperty");
}

static DWORD WINAPI InitThread(void* param)
{
    AMB_UNUSED(param);
    InitializeCriticalSection(&g_log_cs);

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) slash[1] = 0;
        wcscat_s(path, MAX_PATH, L"AMCoreAudioProbe.log");
        g_log = CreateFileW(path, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    ProbeLog("CoreAudio probe v2 loaded (pid %lu)", (unsigned long)GetCurrentProcessId());
    ProbeLog("main module: %p", (void*)GetModuleHandleW(NULL));

    for (int i = 0; i < 100; ++i) {
        if (GetModuleHandleW(L"CoreAudioToolbox.dll")) break;
        Sleep(50);
    }
    ResolveOriginals();
    ProbeLog("CoreAudioToolbox base: %p", (void*)GetModuleHandleW(L"CoreAudioToolbox.dll"));

    for (int pass = 0; pass < 100; ++pass) {
        PatchAllModules();
        Sleep(100);
    }
    ProbeLog("module scan complete");
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    AMB_UNUSED(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

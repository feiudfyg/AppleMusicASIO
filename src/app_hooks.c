#include "common.h"
#include "log.h"
#include "app_hooks.h"
#include <tlhelp32.h>

typedef int32_t OSStatus;

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

typedef OSStatus (*PFN_AudioQueueNewOutput)(const AudioStreamBasicDescription*, void*, void*,
                                            void*, void*, uint32_t, void**);
typedef OSStatus (*PFN_AudioConverterNew)(const AudioStreamBasicDescription*,
                                          const AudioStreamBasicDescription*, void**);
typedef OSStatus (*PFN_AudioConverterNewSpecific)(const AudioStreamBasicDescription*,
                                                  const AudioStreamBasicDescription*,
                                                  uint32_t, const void*, void**);

static HMODULE g_module = NULL;
static PFN_AudioQueueNewOutput g_queue_new = NULL;
static PFN_AudioConverterNew g_converter_new = NULL;
static PFN_AudioConverterNewSpecific g_converter_specific = NULL;
static volatile LONG g_source_rate = 0;

static uintptr_t g_seen_modules[256];
static volatile LONG g_seen_count = 0;

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

static void SetSourceRate(double rate, const char* source)
{
    if (rate < 8000.0 || rate > 768000.0) return;
    uint32_t value = (uint32_t)(rate + 0.5);
    uint32_t old = (uint32_t)InterlockedExchange(&g_source_rate, (LONG)value);
    if (old != value) {
        LOGI("source rate: %u Hz (from %s)", value, source);
    }
}

uint32_t AppHooks_GetSourceRate(void)
{
    return (uint32_t)InterlockedCompareExchange(&g_source_rate, 0, 0);
}

static OSStatus Hook_AudioQueueNewOutput(const AudioStreamBasicDescription* fmt,
                                         void* callback, void* user, void* loop,
                                         void* mode, uint32_t flags, void** out)
{
    if (fmt) {
        char fourcc[5];
        FormatFourCC(fmt->mFormatID, fourcc);
        LOGI("AudioQueueNewOutput: %.0f Hz fmt='%s' flags=0x%08lx %lu ch",
             fmt->mSampleRate, fourcc, (unsigned long)fmt->mFormatFlags,
             (unsigned long)fmt->mChannelsPerFrame);
        SetSourceRate(fmt->mSampleRate, "AudioQueueNewOutput");
    }
    return g_queue_new ? g_queue_new(fmt, callback, user, loop, mode, flags, out) : -1;
}

static OSStatus Hook_AudioConverterNew(const AudioStreamBasicDescription* src,
                                       const AudioStreamBasicDescription* dst, void** out)
{
    if (src && dst) {
        LOGI("AudioConverterNew: %.0f -> %.0f Hz (fmt flags 0x%lx -> 0x%lx)",
             src->mSampleRate, dst->mSampleRate, (unsigned long)src->mFormatFlags,
             (unsigned long)dst->mFormatFlags);
        SetSourceRate(src->mSampleRate, "AudioConverterNew source");
    }
    return g_converter_new ? g_converter_new(src, dst, out) : -1;
}

static OSStatus Hook_AudioConverterNewSpecific(const AudioStreamBasicDescription* src,
                                               const AudioStreamBasicDescription* dst,
                                               uint32_t manufacturer, const void* class_id,
                                               void** out)
{
    if (src && dst) {
        LOGI("AudioConverterNewSpecific: %.0f -> %.0f Hz", src->mSampleRate,
             dst->mSampleRate);
        SetSourceRate(src->mSampleRate, "AudioConverterNewSpecific source");
    }
    return g_converter_specific
               ? g_converter_specific(src, dst, manufacturer, class_id, out)
               : -1;
}

typedef struct {
    const char* name;
    void* hook;
    void** original;
} HookSpec;

static const HookSpec g_hooks[] = {
    {"AudioQueueNewOutput", (void*)Hook_AudioQueueNewOutput, (void**)&g_queue_new},
    {"AudioConverterNew", (void*)Hook_AudioConverterNew, (void**)&g_converter_new},
    {"AudioConverterNewSpecific", (void*)Hook_AudioConverterNewSpecific,
     (void**)&g_converter_specific},
};

static BOOL ModuleSeen(uintptr_t base)
{
    LONG count = g_seen_count;
    for (LONG i = 0; i < count; ++i) {
        if (g_seen_modules[i] == base) return TRUE;
    }
    return FALSE;
}

static void MarkModule(uintptr_t base)
{
    LONG index = InterlockedIncrement(&g_seen_count) - 1;
    if (index >= 0 && index < (LONG)(sizeof(g_seen_modules) / sizeof(g_seen_modules[0]))) {
        g_seen_modules[index] = base;
    }
}

static int PatchModule(HMODULE module, const wchar_t* module_name)
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
    DWORD count = dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress);
    for (DWORD d = 0; d < count && imp->Name; ++d, ++imp) {
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
                LOGI("coreaudio hook: %ls!%s", module_name, g_hooks[i].name);
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
            if (ModuleSeen((uintptr_t)entry.modBaseAddr)) continue;
            MarkModule((uintptr_t)entry.modBaseAddr);
            PatchModule((HMODULE)entry.modBaseAddr, entry.szModule);
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

static DWORD WINAPI AppHooksThread(void* param)
{
    AMB_UNUSED(param);
    for (int i = 0; i < 200; ++i) {
        if (GetModuleHandleW(L"CoreAudioToolbox.dll")) break;
        Sleep(50);
    }

    HMODULE coreaudio = GetModuleHandleW(L"CoreAudioToolbox.dll");
    if (!coreaudio) {
        LOGW("coreaudio hook: CoreAudioToolbox not loaded");
        return 0;
    }
    for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i) {
        void* fn = (void*)GetProcAddress(coreaudio, g_hooks[i].name);
        if (fn) *g_hooks[i].original = fn;
    }

    for (int pass = 0; pass < 300; ++pass) {
        PatchAllModules();
        Sleep(100);
    }
    LOGI("coreaudio hook scan complete");
    return 0;
}

void AppHooks_Start(HMODULE module)
{
    g_module = module;
    HANDLE thread = CreateThread(NULL, 0, AppHooksThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

#include <windows.h>
#include <winternl.h>
#include <stdio.h>

typedef NTSTATUS (NTAPI* PFN_NTQUERY)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

static BOOL ReadMem(HANDLE p, const void* addr, void* out, SIZE_T size)
{
    SIZE_T done = 0;
    return ReadProcessMemory(p, addr, out, size, &done) && done == size;
}

static void PrintUnicode(HANDLE p, const UNICODE_STRING* us, const char* label)
{
    wchar_t buf[4096];
    DWORD chars = us->Length / sizeof(wchar_t);
    if (!us->Buffer || chars == 0) {
        printf("%s: <empty>\n", label);
        return;
    }
    if (chars >= 4096) chars = 4095;
    if (!ReadMem(p, us->Buffer, buf, chars * sizeof(wchar_t))) {
        printf("%s: <read failed at %p>\n", label, us->Buffer);
        return;
    }
    buf[chars] = 0;
    printf("%s: %ls\n", label, buf);
}

int wmain(int argc, wchar_t** argv)
{
    DWORD pid = 0;
    if (argc < 2) {
        fwprintf(stderr, L"usage: proc_cwd <pid>\n");
        return 2;
    }
    pid = wcstoul(argv[1], NULL, 10);
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) {
        printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_NTQUERY query = (PFN_NTQUERY)GetProcAddress(ntdll, "NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION pbi = {0};
    ULONG ret = 0;
    NTSTATUS st = query(p, ProcessBasicInformation, &pbi, sizeof(pbi), &ret);
    if (st < 0) {
        printf("NtQueryInformationProcess failed: 0x%lX\n", (unsigned long)st);
        return 1;
    }

    BYTE peb[0x100] = {0};
    if (!ReadMem(p, pbi.PebBaseAddress, peb, sizeof(peb))) {
        printf("read PEB failed\n");
        return 1;
    }
    PVOID params = NULL;
    memcpy(&params, peb + 0x20, sizeof(params));
    printf("PEB=%p params=%p\n", pbi.PebBaseAddress, params);

    BYTE pp[0x200] = {0};
    if (!ReadMem(p, params, pp, sizeof(pp))) {
        printf("read process parameters failed\n");
        return 1;
    }
    PrintUnicode(p, (const UNICODE_STRING*)(pp + 0x38), "CurrentDirectory");
    PrintUnicode(p, (const UNICODE_STRING*)(pp + 0x50), "DllPath");
    PrintUnicode(p, (const UNICODE_STRING*)(pp + 0x60), "ImagePathName");
    PrintUnicode(p, (const UNICODE_STRING*)(pp + 0x70), "CommandLine");

    PVOID env = NULL;
    memcpy(&env, pp + 0x80, sizeof(env));
    if (env) {
        wchar_t block[65536];
        SIZE_T done = 0;
        ReadProcessMemory(p, env, block, sizeof(block) - sizeof(wchar_t), &done);
        block[done / sizeof(wchar_t)] = 0;
        for (wchar_t* s = block; *s; s += wcslen(s) + 1) {
            if (!_wcsnicmp(s, L"PATH=", 5)) {
                wprintf(L"Environment PATH: %ls\n", s + 5);
                break;
            }
        }
    }
    CloseHandle(p);
    return 0;
}

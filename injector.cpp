
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winnt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <conio.h>

extern "C" {
    extern const unsigned char _binary_lunar_nick_overlay_dll_start[];
    extern const unsigned char _binary_lunar_nick_overlay_dll_end[];
}

#pragma pack(push, 8)
struct NickIpc {
    volatile LONG seq;
    volatile LONG ack;
    volatile LONG cmd;
    volatile LONG result;
    char nick[32];
};
#pragma pack(pop)

enum {
    CMD_NONE = 0,
    CMD_SET_NICK = 1,
    CMD_PREMIUM = 2
};

enum {
    RES_IDLE = 0,
    RES_OK = 1,
    RES_FAIL = 2
};

static const char* kIpcName = "Local\\LunarNickOverlayIpc";
static NickIpc* g_ipc = nullptr;
static HANDLE g_map = nullptr;

static bool OpenIpcClient() {
    for (int i = 0; i < 50; ++i) {
        g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kIpcName);
        if (g_map) break;
        Sleep(100);
    }
    if (!g_map) return false;
    g_ipc = (NickIpc*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NickIpc));
    return g_ipc != nullptr;
}

static bool SendIpc(LONG cmd, const char* nick, DWORD timeoutMs) {
    if (!g_ipc) return false;
    InterlockedExchange(&g_ipc->result, RES_IDLE);
    InterlockedExchange(&g_ipc->ack, 0);
    if (nick) {
        ZeroMemory((void*)g_ipc->nick, sizeof(g_ipc->nick));
        strncpy((char*)g_ipc->nick, nick, sizeof(g_ipc->nick) - 1);
    } else {
        ZeroMemory((void*)g_ipc->nick, sizeof(g_ipc->nick));
    }
    MemoryBarrier();
    InterlockedExchange(&g_ipc->cmd, cmd);
    LONG seq = InterlockedIncrement(&g_ipc->seq);

    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        LONG ack = InterlockedCompareExchange(&g_ipc->ack, 0, 0);
        if (ack == seq) {
            return InterlockedCompareExchange(&g_ipc->result, 0, 0) == RES_OK;
        }
        Sleep(10);
    }
    return false;
}

static HANDLE g_con = nullptr;
static WORD g_attrNormal = 7;

static void ClearLine(int row) {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(g_con, &csbi);
    COORD pos{0, (SHORT)row};
    DWORD n = 0;
    FillConsoleOutputCharacterA(g_con, ' ', csbi.dwSize.X, pos, &n);
    FillConsoleOutputAttribute(g_con, g_attrNormal, csbi.dwSize.X, pos, &n);
    SetConsoleCursorPosition(g_con, pos);
}

static void ShowHelpLine() {
    ClearLine(0);
    printf("Type \"1\" to select nick that u want or type \"2\" to switch for premium acc which was originally launched");
    fflush(stdout);
    ClearLine(1);
    printf("ESC = set premium and killinjector process");
    fflush(stdout);
}

static void ShowNickLine(const char* nick) {
    ClearLine(2);
    printf("> %s", nick ? nick : "");
    fflush(stdout);
}

static void ShowStatusPremium() {
    ClearLine(3);
    printf("Status: ");
    SetConsoleTextAttribute(g_con, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    printf("Premium");
    SetConsoleTextAttribute(g_con, g_attrNormal);
    fflush(stdout);
}

static void ShowStatusNick(const char* nick) {
    ClearLine(3);
    printf("Status: %s", nick ? nick : "-");
    fflush(stdout);
}

static void RunCli() {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(g_con, &csbi);
    g_attrNormal = csbi.wAttributes;

    char nickDraft[17] = {0};
    int nickLen = 0;
    bool editing = false;

    ShowHelpLine();
    ShowNickLine("");
    ShowStatusPremium();

    while (true) {
        if (!_kbhit()) {
            Sleep(20);
            continue;
        }
        int ch = _getch();
        if (ch == 0 || ch == 224) {
            _getch();
            continue;
        }

        if (ch == 27) {
            SendIpc(CMD_PREMIUM, nullptr, 30000);
            ShowStatusPremium();
            break;
        }

        if (!editing) {
            if (ch == '1') {
                editing = true;
                nickLen = 0;
                nickDraft[0] = 0;
                ShowNickLine("");
            } else if (ch == '2') {
                SendIpc(CMD_PREMIUM, nullptr, 30000);
                ShowStatusPremium();
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (nickLen > 0) {
                SendIpc(CMD_SET_NICK, nickDraft, 30000);
                editing = false;
                ShowNickLine("");
                ShowStatusNick(nickDraft);
            }
        } else if (ch == 8) {
            if (nickLen > 0) {
                nickDraft[--nickLen] = 0;
                ShowNickLine(nickDraft);
            }
        } else if (nickLen < 16) {
            char c = (char)ch;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_') {
                nickDraft[nickLen++] = c;
                nickDraft[nickLen] = 0;
                ShowNickLine(nickDraft);
            }
        }
    }
}

std::string ReadProcessCmdLine(DWORD pid) {
    std::string result;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return result;

    using NtQueryInformationProcessFn = LONG (NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto NtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess"));
    if (!NtQueryInformationProcess) {
        CloseHandle(hProc);
        return result;
    }

    struct PROCESS_BASIC_INFORMATION {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    } pbi{};

    if (NtQueryInformationProcess(hProc, 0, &pbi, sizeof(pbi), nullptr) != 0) {
        CloseHandle(hProc);
        return result;
    }

    struct PEB_PARTIAL {
        BYTE Reserved1[16];
        PVOID Reserved3[2];
        PVOID Ldr;
        PVOID ProcessParameters;
    } peb{};

    if (!ReadProcessMemory(hProc, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr)) {
        CloseHandle(hProc);
        return result;
    }

    struct RTL_USER_PROCESS_PARAMETERS_PARTIAL {
        BYTE Reserved1[56];
        BYTE Reserved2[16];
        struct {
            USHORT Length;
            USHORT MaximumLength;
            PWSTR Buffer;
        } ImagePathName;
        struct {
            USHORT Length;
            USHORT MaximumLength;
            PWSTR Buffer;
        } CommandLine;
    } params{};

    if (!ReadProcessMemory(hProc, peb.ProcessParameters, &params, sizeof(params), nullptr)) {
        CloseHandle(hProc);
        return result;
    }

    std::vector<wchar_t> buf(params.CommandLine.Length / sizeof(wchar_t) + 1, 0);
    if (ReadProcessMemory(hProc, params.CommandLine.Buffer, buf.data(), params.CommandLine.Length, nullptr)) {
        std::wstring ws(buf.data());
        result.assign(ws.begin(), ws.end());
    }

    CloseHandle(hProc);
    return result;
}

bool Is64BitProcess(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    BOOL wow64 = FALSE;
    IsWow64Process(hProc, &wow64);
    CloseHandle(hProc);
    return !wow64;
}

WORD GetDllMachineType(const char* path) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return IMAGE_FILE_MACHINE_UNKNOWN;

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }

    void* base = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        UnmapViewOfFile(base);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        UnmapViewOfFile(base);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return IMAGE_FILE_MACHINE_UNKNOWN;
    }

    WORD machine = nt->FileHeader.Machine;
    UnmapViewOfFile(base);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return machine;
}

const char* MachineToStr(WORD machine) {
    if (machine == IMAGE_FILE_MACHINE_AMD64) return "x64";
    if (machine == IMAGE_FILE_MACHINE_I386) return "x86";
    if (machine == IMAGE_FILE_MACHINE_ARM64) return "arm64";
    return "unknown";
}

DWORD FindLunarProcess() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD bestPid = 0;
    int bestScore = -1;

    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name(pe.szExeFile);
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name != L"javaw.exe" && name != L"java.exe") continue;
            if (!Is64BitProcess(pe.th32ProcessID)) continue;

            std::string cmd = ReadProcessCmdLine(pe.th32ProcessID);
            std::string lower = cmd;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            int score = 0;
            if (lower.find("lunar") != std::string::npos) score += 6;
            if (lower.find("moonsworth") != std::string::npos) score += 6;
            if (lower.find("genesis") != std::string::npos) score += 4;
            if (lower.find("minecraft") != std::string::npos) score += 3;
            if (lower.find("lwjgl") != std::string::npos) score += 2;
            if (lower.find("--accessToken") != std::string::npos) score += 2;

            HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
            if (modSnap != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W me{};
                me.dwSize = sizeof(me);
                if (Module32FirstW(modSnap, &me)) {
                    do {
                        std::wstring mod(me.szModule);
                        std::transform(mod.begin(), mod.end(), mod.begin(), ::towlower);
                        if (mod.find(L"lwjgl") != std::wstring::npos) score += 5;
                        if (mod.find(L"nanovg") != std::wstring::npos) score += 8;
                        if (mod.find(L"opengl32.dll") != std::wstring::npos) score += 2;
                    } while (Module32NextW(modSnap, &me));
                }
                CloseHandle(modSnap);
            }

            if (score > bestScore) {
                bestScore = score;
                bestPid = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return bestPid;
}

bool InjectDll(HANDLE hProc, const char* fullPath, DWORD* remoteErr) {
    size_t pathLen = strlen(fullPath) + 1;
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        printf("[-] VirtualAllocEx failed (%lu)\n", GetLastError());
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteMem, fullPath, pathLen, nullptr)) {
        printf("[-] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    if (!loadLib) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadLib, remoteMem, 0, nullptr);
    if (!hThread) {
        printf("[-] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(hThread, 15000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);

    if (exitCode == 0) {
        if (remoteErr) *remoteErr = 0;
        return false;
    }
    return true;
}

bool ExtractEmbeddedDll(char* outPath, DWORD outSize) {
    const unsigned char* start = _binary_lunar_nick_overlay_dll_start;
    const unsigned char* end = _binary_lunar_nick_overlay_dll_end;
    size_t size = (size_t)(end - start);
    if (!start || size < 64) {
        printf("[-] Embedded DLL missing/empty. Rebuild with dll_blob.o linked.\n");
        return false;
    }

    char tempDir[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, tempDir)) return false;
    if (snprintf(outPath, outSize, "%slunar_nick_overlay_%lu.dll", tempDir, GetCurrentProcessId()) <= 0) {
        return false;
    }

    HANDLE hFile = CreateFileA(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("[-] Cannot write temp DLL (%lu)\n", GetLastError());
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, start, (DWORD)size, &written, nullptr);
    CloseHandle(hFile);
    if (!ok || written != size) {
        DeleteFileA(outPath);
        printf("[-] Failed writing embedded DLL.\n");
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    char fullPath[MAX_PATH] = {0};
    bool fromEmbedded = true;

    if (argc > 1 && GetFileAttributesA(argv[1]) != INVALID_FILE_ATTRIBUTES &&
        strstr(argv[1], ".dll")) {
        DWORD got = GetFullPathNameA(argv[1], MAX_PATH, fullPath, nullptr);
        if (!got || got >= MAX_PATH) {
            printf("[-] Invalid DLL path.\n");
            return 1;
        }
        fromEmbedded = false;
        printf("[*] Using external DLL: %s\n", fullPath);
    } else {
        if (!ExtractEmbeddedDll(fullPath, MAX_PATH)) return 1;
        printf("[*] Extracted embedded DLL: %s\n", fullPath);
    }

    WORD dllMachine = GetDllMachineType(fullPath);
    if (dllMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
        printf("[-] Cannot read DLL architecture (invalid/corrupted PE?).\n");
        if (fromEmbedded) DeleteFileA(fullPath);
        return 1;
    }
    printf("[*] DLL architecture: %s\n", MachineToStr(dllMachine));

    if (dllMachine != IMAGE_FILE_MACHINE_AMD64) {
        printf("[-] This injector expects x64 DLL. Current DLL is %s.\n", MachineToStr(dllMachine));
        if (fromEmbedded) DeleteFileA(fullPath);
        return 1;
    }

    printf("[*] Waiting for Lunar Client (javaw.exe)...\n");

    DWORD pid = 0;
    for (int i = 1; i < argc; ++i) {
        if (strstr(argv[i], ".dll")) continue;
        pid = static_cast<DWORD>(strtoul(argv[i], nullptr, 10));
        if (pid) {
            printf("[*] Using manual PID: %lu\n", pid);
            break;
        }
    }
    if (!pid) {
        while (pid == 0) {
            pid = FindLunarProcess();
            if (!pid) Sleep(1000);
        }
    }

    printf("[+] Found process PID: %lu\n", pid);
    if (!Is64BitProcess(pid)) {
        printf("[-] Target Java process is not x64.\n");
        if (fromEmbedded) DeleteFileA(fullPath);
        return 1;
    }
    std::string cmd = ReadProcessCmdLine(pid);
    if (!cmd.empty()) {
        if (cmd.size() > 120) cmd = cmd.substr(0, 120) + "...";
        printf("[*] Cmd: %s\n", cmd.c_str());
    }

    printf("[*] Waiting 3s for game init...\n");
    Sleep(3000);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) {
        printf("[-] OpenProcess failed (%lu). Run as Administrator.\n", GetLastError());
        if (fromEmbedded) DeleteFileA(fullPath);
        return 1;
    }

    printf("[*] Injecting...\n");
    DWORD remoteErr = 0;
    bool injected = InjectDll(hProc, fullPath, &remoteErr);
    CloseHandle(hProc);

    if (!injected) {
        printf("[-] Injection failed (LoadLibrary returned NULL).\n");
        if (fromEmbedded) DeleteFileA(fullPath);
        return 1;
    }

    printf("[+] DLL injected successfully!\n");
    printf("[*] Connecting CLI...\n");
    if (!OpenIpcClient()) {
        printf("[-] Cannot open IPC with DLL.\n");
        if (fromEmbedded) DeleteFileA(fullPath);
        return 1;
    }
    printf("[+] Ready.\n");
    Sleep(400);
    RunCli();

    if (g_ipc) UnmapViewOfFile((LPCVOID)g_ipc);
    if (g_map) CloseHandle(g_map);
    if (fromEmbedded) DeleteFileA(fullPath);
    return 0;
}

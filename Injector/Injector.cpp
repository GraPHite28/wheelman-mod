#include <Windows.h>
#include <cstdio>
#include <string>

// Default game location, used when Injector.exe is run with no arguments
// (e.g. double-clicked from Explorer) so it acts as a self-contained
// launcher - no need to type the game/DLL paths on a command line every
// time. Override by passing explicit args, same as before.
// The game location is not hardcoded: Injector.ini next to this exe can hold it ([Injector] GameExe=... WorkingDir=...),
// otherwise Wheelman.exe is looked for in the current folder (run it from the game's Binaries folder) or the paths are
// passed as arguments. The graphical Launcher (WheelmanModLauncher.exe) finds the game on its own.
static std::wstring IniValue(const wchar_t* key, const wchar_t* def)
{
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring ini = self;
    size_t slash = ini.find_last_of(L"\\/");
    ini = (slash == std::wstring::npos ? L"" : ini.substr(0, slash + 1)) + L"Injector.ini";
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Injector", key, def, buf, MAX_PATH, ini.c_str());
    return buf;
}
static const std::wstring kDefaultExePathStr = IniValue(L"GameExe", L"Wheelman.exe");
static const std::wstring kDefaultWorkingDirStr = IniValue(L"WorkingDir", L".");
static const wchar_t* kDefaultExePath = kDefaultExePathStr.c_str();
static const wchar_t* kDefaultWorkingDir = kDefaultWorkingDirStr.c_str();

// Launching with NO game args at all reliably crashes during engine
// bootstrap (GPF inside GSemaphore/GColor init, before the mod's own D3D
// hook ever fires - confirmed via WheelmanMod_log.txt cutting off right
// after InstallInputHook, twice in a row). All previously-working launches
// documented in this file's own usage example passed -language=rus, so
// something in the game's startup path depends on it being set explicitly
// instead of auto-detected. Always pass it by default; an explicit
// override (argc >= 4) can still replace it if that ever needs to change.
static const wchar_t* kDefaultGameArgs = L"-language=rus";

// The mod DLL is always built into the same bin\Win32\Release folder as
// this injector, so its default path is derived from our own exe path
// rather than hardcoded separately.
static std::wstring DefaultDllPath()
{
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    return path + L"WheelmanMod.dll";
}

// Launches the game itself (CREATE_SUSPENDED), injects the mod DLL while the
// main thread is still frozen (so DllMain's Direct3DCreate9 IAT hook is in
// place before the game ever calls it), then resumes. This is required for
// a real, non-dummy-device D3D9 hook - see IATHook.cpp for why.
int wmain(int argc, wchar_t* argv[])
{
    std::wstring exePathBuf = kDefaultExePath;
    std::wstring workingDirBuf = kDefaultWorkingDir;
    std::wstring dllPathBuf = DefaultDllPath();
    int gameArgsStart = argc; // no extra game args by default

    if (argc >= 4)
    {
        // Explicit override: <game exe path> <working directory> <mod dll path> [game args...]
        exePathBuf = argv[1];
        workingDirBuf = argv[2];
        dllPathBuf = argv[3];
        gameArgsStart = 4;
    }
    else if (argc != 1)
    {
        wprintf(L"Usage: Injector.exe [<game exe path> <working directory> <mod dll path>] [game args...]\n");
        wprintf(L"Run with no arguments to launch %s using the mod DLL next to this exe.\n", kDefaultExePath);
        wprintf(L"Or override everything: Injector.exe E:\\WheelMan\\Binaries\\Wheelman.exe "
                 L"E:\\WheelMan\\Binaries C:\\...\\WheelmanMod.dll -language=rus\n");
        return 1;
    }

    const wchar_t* exePath = exePathBuf.c_str();
    const wchar_t* workingDir = workingDirBuf.c_str();
    const wchar_t* dllPath = dllPathBuf.c_str();

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"Game exe not found: %s\n", exePath);
        return 1;
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"Mod DLL not found: %s\n", dllPath);
        return 1;
    }

    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\"";
    if (gameArgsStart >= argc) // no explicit game args given - fall back to the known-working default
    {
        cmdLine += L" ";
        cmdLine += kDefaultGameArgs;
    }
    for (int i = gameArgsStart; i < argc; ++i)
    {
        cmdLine += L" ";
        cmdLine += argv[i];
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    BOOL created = CreateProcessW(exePath, &cmdLine[0], nullptr, nullptr, FALSE,
                                   CREATE_SUSPENDED, nullptr, workingDir, &si, &pi);
    if (!created)
    {
        wprintf(L"CreateProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    wprintf(L"Launched suspended, PID %lu\n", pi.dwProcessId);

    size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(pi.hProcess, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
    {
        wprintf(L"VirtualAllocEx failed (%lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }

    if (!WriteProcessMemory(pi.hProcess, remoteMem, dllPath, pathBytes, nullptr))
    {
        wprintf(L"WriteProcessMemory failed (%lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }

    LPTHREAD_START_ROUTINE loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE remoteThread = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLibraryW, remoteMem, 0, nullptr);
    if (!remoteThread)
    {
        wprintf(L"CreateRemoteThread failed (%lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }

    WaitForSingleObject(remoteThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(remoteThread, &exitCode);
    CloseHandle(remoteThread);
    VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);

    if (exitCode == 0)
    {
        wprintf(L"LoadLibraryW returned NULL - DLL did not load. Terminating the suspended process.\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    wprintf(L"DLL loaded (module base 0x%08X), IAT hook installed. Resuming main thread...\n", exitCode);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    wprintf(L"Done. Press Insert in-game to toggle the overlay.\n");
    return 0;
}

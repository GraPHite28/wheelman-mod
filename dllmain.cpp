#include <Windows.h>
#include "IATHook.h"
#include "InputHook.h"
#include "D3DHook.h"
#include "Patches.h"
#include "Log.h"
#include "CrashHandler.h"

// This DLL must be injected into Wheelman.exe *before* it creates its D3D9
// device (i.e. injected into a process launched with CREATE_SUSPENDED, then
// resumed - see Injector.cpp). Everything here is plain memory patching, no
// LoadLibrary/COM calls, so it's safe to run directly under the loader lock.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        LogInit();
        LogF("DllMain: DLL_PROCESS_ATTACH");
        DisableThreadLibraryCalls(hModule);
        CrashHandler::Install(hModule);
        LogF("DllMain: crash handler installed, reports go to %s", CrashHandler::Dir());
        InstallGamePatches();
        LogF("DllMain: InstallGamePatches done");
        InstallD3DHook();
        LogF("DllMain: InstallD3DHook done");
        InstallInputHook();
        LogF("DllMain: InstallInputHook done");
        break;
    case DLL_PROCESS_DETACH:
        // reserved != NULL means the whole process is terminating (the game closing). Nothing has to be undone then,
        // and touching Direct3D from here (ImGui_ImplDX9_Shutdown) waits inside d3d9.dll for a device that is already
        // being torn down: the game window closed but the process never ended (found with the thread stack dumper).
        if (reserved) break;
        RemoveGamePatches();
        RemoveD3DHook();
        RemoveInputHook();
        ShutdownOverlay();
        break;
    }
    return TRUE;
}

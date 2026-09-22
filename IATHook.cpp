#include <Windows.h>
#include <d3d9.h>
#include "IATHook.h"
#include "D3DHook.h"
#include "Log.h"

namespace
{
    typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
    typedef HRESULT(__stdcall* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

    Direct3DCreate9_t oDirect3DCreate9 = nullptr;
    CreateDevice_t oCreateDevice = nullptr;
    void** g_iatSlot = nullptr;
    bool g_deviceHooked = false;

    HRESULT __stdcall hkCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE deviceType, HWND hFocus,
                                      DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDevice)
    {
        HRESULT hr = oCreateDevice(self, adapter, deviceType, hFocus, flags, pp, ppDevice);
        LogF("hkCreateDevice: hr=0x%08X pDevice=0x%p", hr, ppDevice ? *ppDevice : nullptr);
        if (SUCCEEDED(hr) && ppDevice && *ppDevice && !g_deviceHooked)
        {
            HookDeviceVTable(*ppDevice);
            g_deviceHooked = true;
        }
        return hr;
    }

    IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdkVersion)
    {
        IDirect3D9* pD3D = oDirect3DCreate9(sdkVersion);
        LogF("hkDirect3DCreate9: called, pD3D=0x%p", pD3D);
        if (pD3D && !oCreateDevice)
        {
            void** vtable = *reinterpret_cast<void***>(pD3D);
            oCreateDevice = reinterpret_cast<CreateDevice_t>(vtable[16]);

            DWORD oldProtect;
            VirtualProtect(&vtable[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
            vtable[16] = hkCreateDevice;
            VirtualProtect(&vtable[16], sizeof(void*), oldProtect, &oldProtect);
            LogF("hkDirect3DCreate9: IDirect3D9::CreateDevice hooked");
        }
        return pD3D;
    }

    // Walks the current process's own import table (Wheelman.exe) looking
    // for moduleName!importName and returns a pointer to its IAT slot -
    // the location the loader writes the resolved function address into,
    // which is what every CALL through that import actually reads from.
    void** FindIATSlot(const char* moduleName, const char* importName)
    {
        HMODULE base = GetModuleHandleA(nullptr);
        auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(base) + dos->e_lfanew);
        auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0) return nullptr;

        auto desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<BYTE*>(base) + importDir.VirtualAddress);
        for (; desc->Name != 0; ++desc)
        {
            const char* name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(base) + desc->Name);
            if (_stricmp(name, moduleName) != 0) continue;

            auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(base) + desc->FirstThunk);
            UINT_PTR origThunkRVA = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            auto origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(base) + origThunkRVA);

            for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++thunk)
            {
                if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
                auto importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                    reinterpret_cast<BYTE*>(base) + origThunk->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(importByName->Name), importName) == 0)
                    return reinterpret_cast<void**>(&thunk->u1.Function);
            }
        }
        return nullptr;
    }
}

bool InstallD3DHook()
{
    g_iatSlot = FindIATSlot("d3d9.dll", "Direct3DCreate9");
    LogF("InstallD3DHook: IAT slot for d3d9.dll!Direct3DCreate9 = 0x%p", g_iatSlot);
    if (!g_iatSlot) return false;

    oDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(*g_iatSlot);

    DWORD oldProtect;
    VirtualProtect(g_iatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *g_iatSlot = reinterpret_cast<void*>(hkDirect3DCreate9);
    VirtualProtect(g_iatSlot, sizeof(void*), oldProtect, &oldProtect);
    LogF("InstallD3DHook: IAT patched, original=0x%p", oDirect3DCreate9);
    return true;
}

void RemoveD3DHook()
{
    if (!g_iatSlot || !oDirect3DCreate9) return;
    DWORD oldProtect;
    VirtualProtect(g_iatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *g_iatSlot = reinterpret_cast<void*>(oDirect3DCreate9);
    VirtualProtect(g_iatSlot, sizeof(void*), oldProtect, &oldProtect);
}

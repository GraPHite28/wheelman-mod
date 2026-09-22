#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>
#include "InputHook.h"
#include "Overlay.h"
#include "Aimbot.h"
#include "MouseLook.h"
#include "Log.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace
{
    typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    typedef HRESULT(__stdcall* CreateDevice_t)(IDirectInput8*, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN);
    typedef HRESULT(__stdcall* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
    typedef HRESULT(__stdcall* GetDeviceData_t)(IDirectInputDevice8*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

    DirectInput8Create_t oDirectInput8Create = nullptr;
    CreateDevice_t oCreateDevice = nullptr;
    GetDeviceState_t oGetDeviceState = nullptr;
    GetDeviceData_t oGetDeviceData = nullptr;
    void** g_iatSlot = nullptr;
    void* g_mouseDevice = nullptr;
    void* g_mouseDevices[8] = {};   // every GUID_SysMouse device the game created (it creates two)
    volatile LONG g_lastDiTick = 0;
    typedef BOOL(WINAPI* GetCursorPos_t)(LPPOINT);
    GetCursorPos_t oGetCursorPos = nullptr;
    void** g_cursorSlot = nullptr;
    bool IsMouse(void* dev) { for (void* d : g_mouseDevices) if (d && d == dev) return true; return false; }

    HRESULT __stdcall hkGetDeviceState(IDirectInputDevice8* self, DWORD size, LPVOID data)
    {
        HRESULT hr = oGetDeviceState(self, size, data);
        if (IsMouse(self) && Overlay::wantMouseCapture && SUCCEEDED(hr) && data)
        {
            static bool logged = false;
            if (!logged) { LogF("hkGetDeviceState: suppressing mouse input (self=0x%p)", self); logged = true; }
            memset(data, 0, size); // report "nothing pressed, no movement" to the game
        }
        else if (IsMouse(self) && SUCCEEDED(hr) && data && size >= sizeof(DIMOUSESTATE))
        {
            // Aimbot: merge the pending motion / fire button into what the game reads.
            InterlockedIncrement(&Aimbot::statStateCalls);
            g_lastDiTick = static_cast<LONG>(GetTickCount());
            auto* st = static_cast<DIMOUSESTATE*>(data);
            if (MouseLook::active)
            {
                // direct mouse camera: the raw motion is used by the mod, the game sees none
                InterlockedExchangeAdd(&MouseLook::accX, st->lX); InterlockedExchangeAdd(&MouseLook::accY, st->lY);
                st->lX = 0; st->lY = 0;
            }
            long dx = InterlockedExchange(&Aimbot::pendingDx, 0), dy = InterlockedExchange(&Aimbot::pendingDy, 0);
            if (dx || dy) { st->lX += dx; st->lY += dy; InterlockedIncrement(&Aimbot::statInjected); }
            if (Aimbot::fireHeld) { st->rgbButtons[0] = 0x80; InterlockedIncrement(&Aimbot::statFireInjected); }
        }
        return hr;
    }

    HRESULT __stdcall hkGetDeviceData(IDirectInputDevice8* self, DWORD size, LPDIDEVICEOBJECTDATA data,
                                       LPDWORD inOut, DWORD flags)
    {
        if (IsMouse(self) && Overlay::wantMouseCapture)
        {
            if (inOut) *inOut = 0; // pretend the buffered-data queue is empty
            return DI_OK;
        }
        DWORD capacity = inOut ? *inOut : 0;
        HRESULT hr = oGetDeviceData(self, size, data, inOut, flags);
        if (IsMouse(self) && SUCCEEDED(hr) && data && inOut && (size == sizeof(DIDEVICEOBJECTDATA) || size == 16) &&
            !(flags & DIGDD_PEEK))
        {
            // Aimbot: append synthetic axis / button events to the buffered stream.
            InterlockedIncrement(&Aimbot::statDataCalls);
            g_lastDiTick = static_cast<LONG>(GetTickCount());
            if (MouseLook::active)
            {
                // direct mouse camera: take the axis events for the mod and leave the game a silent stream (buttons stay)
                for (DWORD i = 0; i < *inOut; ++i)
                {
                    auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data) + static_cast<size_t>(i) * size);
                    if (e->dwOfs == DIMOFS_X) { InterlockedExchangeAdd(&MouseLook::accX, static_cast<LONG>(e->dwData)); e->dwData = 0; }
                    else if (e->dwOfs == DIMOFS_Y) { InterlockedExchangeAdd(&MouseLook::accY, static_cast<LONG>(e->dwData)); e->dwData = 0; }
                }
            }
            static long lastFire = 0;
            long dx = InterlockedExchange(&Aimbot::pendingDx, 0), dy = InterlockedExchange(&Aimbot::pendingDy, 0);
            long fire = Aimbot::fireHeld;
            DWORD n = *inOut;
            DWORD now = GetTickCount();
            auto push = [&](DWORD ofs, DWORD value)
            {
                if (n >= capacity) return;
                auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data) + static_cast<size_t>(n) * size);
                e->dwOfs = ofs; e->dwData = value; e->dwTimeStamp = now; e->dwSequence = 0;
                if (size >= sizeof(DIDEVICEOBJECTDATA)) e->uAppData = 0;
                ++n;
            };
            if (dx) push(DIMOFS_X, static_cast<DWORD>(dx));
            if (dy) push(DIMOFS_Y, static_cast<DWORD>(dy));
            if (dx || dy) InterlockedIncrement(&Aimbot::statInjected);
            if (fire != lastFire) { push(DIMOFS_BUTTON0, fire ? 0x80 : 0); lastFire = fire; if (fire) InterlockedIncrement(&Aimbot::statFireInjected); }
            *inOut = n;
        }
        return hr;
    }

    // Some Xbox-derived games recentre the cursor every frame and read the look delta from
    // GetCursorPos. When DirectInput isn't being read for the mouse (no DI mouse call for a
    // second) the aimbot motion is added to the position returned here instead.
    BOOL WINAPI hkGetCursorPos(LPPOINT pt)
    {
        BOOL r = oGetCursorPos(pt);
        if (r && pt)
        {
            InterlockedIncrement(&Aimbot::statCursorCalls);
            LONG now = static_cast<LONG>(GetTickCount());
            if (now - g_lastDiTick > 1000 && !Overlay::wantMouseCapture)
            {
                long dx = InterlockedExchange(&Aimbot::pendingDx, 0), dy = InterlockedExchange(&Aimbot::pendingDy, 0);
                if (dx || dy) { pt->x += dx; pt->y += dy; InterlockedIncrement(&Aimbot::statInjected); }
            }
        }
        return r;
    }
    HRESULT __stdcall hkCreateDevice(IDirectInput8* self, REFGUID rguid, LPDIRECTINPUTDEVICE8* ppDevice, LPUNKNOWN pUnk)
    {
        HRESULT hr = oCreateDevice(self, rguid, ppDevice, pUnk);
        if (FAILED(hr) || !ppDevice || !*ppDevice)
            return hr;

        if (IsEqualGUID(rguid, GUID_SysMouse))
        {
            g_mouseDevice = *ppDevice;
            for (void*& slot : g_mouseDevices) if (!slot) { slot = *ppDevice; break; }
            LogF("InputHook: mouse device created = 0x%p", g_mouseDevice);

            if (!oGetDeviceState)
            {
                void** vtable = *reinterpret_cast<void***>(*ppDevice);
                oGetDeviceState = reinterpret_cast<GetDeviceState_t>(vtable[9]);
                oGetDeviceData = reinterpret_cast<GetDeviceData_t>(vtable[10]);

                DWORD oldProtect;
                VirtualProtect(&vtable[9], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
                vtable[9] = hkGetDeviceState;
                VirtualProtect(&vtable[9], sizeof(void*), oldProtect, &oldProtect);

                VirtualProtect(&vtable[10], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
                vtable[10] = hkGetDeviceData;
                VirtualProtect(&vtable[10], sizeof(void*), oldProtect, &oldProtect);
                LogF("InputHook: GetDeviceState(9)/GetDeviceData(10) hooked on mouse vtable");
            }
        }
        return hr;
    }

    HRESULT WINAPI hkDirectInput8Create(HINSTANCE hInst, DWORD version, REFIID riid, LPVOID* ppv, LPUNKNOWN pUnkOuter)
    {
        HRESULT hr = oDirectInput8Create(hInst, version, riid, ppv, pUnkOuter);
        LogF("hkDirectInput8Create: hr=0x%08X ppv=0x%p", hr, ppv ? *ppv : nullptr);
        if (SUCCEEDED(hr) && ppv && *ppv && !oCreateDevice)
        {
            auto pDI = reinterpret_cast<IDirectInput8*>(*ppv);
            void** vtable = *reinterpret_cast<void***>(pDI);
            oCreateDevice = reinterpret_cast<CreateDevice_t>(vtable[3]); // IDirectInput8::CreateDevice

            DWORD oldProtect;
            VirtualProtect(&vtable[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
            vtable[3] = hkCreateDevice;
            VirtualProtect(&vtable[3], sizeof(void*), oldProtect, &oldProtect);
            LogF("hkDirectInput8Create: IDirectInput8::CreateDevice hooked");
        }
        return hr;
    }

    // Same IAT-walking approach as IATHook.cpp.
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

bool InstallInputHook()
{
    g_iatSlot = FindIATSlot("dinput8.dll", "DirectInput8Create");
    LogF("InstallInputHook: IAT slot for dinput8.dll!DirectInput8Create = 0x%p", g_iatSlot);
    if (!g_iatSlot) return false;

    g_cursorSlot = FindIATSlot("user32.dll", "GetCursorPos");
    if (g_cursorSlot)
    {
        oGetCursorPos = reinterpret_cast<GetCursorPos_t>(*g_cursorSlot);
        DWORD op;
        VirtualProtect(g_cursorSlot, sizeof(void*), PAGE_READWRITE, &op);
        *g_cursorSlot = reinterpret_cast<void*>(hkGetCursorPos);
        VirtualProtect(g_cursorSlot, sizeof(void*), op, &op);
        LogF("InstallInputHook: GetCursorPos IAT hooked");
    }
    oDirectInput8Create = reinterpret_cast<DirectInput8Create_t>(*g_iatSlot);

    DWORD oldProtect;
    VirtualProtect(g_iatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *g_iatSlot = reinterpret_cast<void*>(hkDirectInput8Create);
    VirtualProtect(g_iatSlot, sizeof(void*), oldProtect, &oldProtect);
    LogF("InstallInputHook: IAT patched, original=0x%p", oDirectInput8Create);
    return true;
}

void RemoveInputHook()
{
    if (!g_iatSlot || !oDirectInput8Create) return;
    DWORD oldProtect;
    VirtualProtect(g_iatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *g_iatSlot = reinterpret_cast<void*>(oDirectInput8Create);
    VirtualProtect(g_iatSlot, sizeof(void*), oldProtect, &oldProtect);
}

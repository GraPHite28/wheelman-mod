#include <Windows.h>
#include <cmath>
#include <cstdio>
#include "LoadGuard.h"
#include "Police.h"
#include "Log.h"
#include "CrashHandler.h"
#include "Spawner.h"
#include "VehicleMod.h"
#include "BuiltinCheats.h"
#include "Weapons.h"
#include "GameConsole.h"
#include "MouseLook.h"

// Read by the naked Mana detour in Patches.cpp so the pump is only entered
// when there is something to do.
extern "C" volatile long g_policeWork = 0;
extern "C" volatile long g_policePeaceful = 0;
extern "C" volatile long g_policeTickHooked = 0;        // set by Patches.cpp once the subsystem tick hook is installed
extern "C" volatile uintptr_t g_policeInstFromTick = 0; // live subsystem, stored by that hook every frame
extern "C" volatile long g_policeTickCalls = 0;         // how often that tick runs (diagnostics)   // read by the SetArrested detour

namespace Police
{
    bool neverWanted = false;
    bool lockLevel = false;
    int lockValue = 3;
    bool peaceful = false;

    namespace
    {
        // Static vtables (the exe is loaded at a fixed 0x400000 base).
        constexpr uint32_t kVtbl = 0x0136EE28;      // WheelmanPoliceResponseSubsystem
        constexpr uint32_t kCompVtbl = 0x0136FFE8;  // WheelmanPoliceComponent
        constexpr uintptr_t kSetLevelFn = 0x005E70B0;
        constexpr int kBits = 0x30, kLevel = 0x54, kMax = 0x58, kPercent = 0x5C, kLost = 0x60, kCrime = 0x61,
                      kArchetype = 0x2C;

        volatile uintptr_t g_inst = 0;
        volatile long g_pendingLevel = -1;
        ULONGLONG g_lastScan = 0;

        template <class T> bool Rd(uintptr_t a, T& out)
        {
            __try { out = *reinterpret_cast<const T*>(a); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        template <class T> bool Wr(uintptr_t a, T v)
        {
            __try { *reinterpret_cast<T*>(a) = v; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool Valid(uintptr_t b)
        {
            uint32_t vt = 0; int32_t mx = 0, lvl = 0;
            return b && Rd(b, vt) && vt == kVtbl && Rd(b + kMax, mx) && mx >= 1 && mx < 1000 &&
                   Rd(b + kLevel, lvl) && lvl >= 0 && lvl < 1000;
        }

        // The live instance's archetype (+0x2C) is Default__WheelmanPoliceResponseSubsystem,
        // itself a same-vtable object - so of the matches, pick the one whose
        // archetype is another match.
        uintptr_t Scan()
        {
            uintptr_t found[16]; int n = 0;
            SYSTEM_INFO si; GetSystemInfo(&si);
            uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
            const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
            while (addr < maxAddr && n < 16)
            {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
                uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
                {
                    __try
                    {
                        for (uintptr_t p = start; p + 0x70 <= end && n < 16; p += 4)
                            if (*reinterpret_cast<const uint32_t*>(p) == kVtbl && Valid(p)) found[n++] = p;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                addr = end;
            }
            for (int i = 0; i < n; ++i)
            {
                uint32_t arch = 0;
                if (!Rd(found[i] + kArchetype, arch)) continue;
                for (int j = 0; j < n; ++j)
                    if (j != i && found[j] == arch) return found[i];
            }
            return 0;
        }

        uintptr_t Instance(bool allowScan)
        {
            // The subsystem tick hook (Patches.cpp) reports the live instance every frame - no scan needed.
            uintptr_t fromTick = g_policeInstFromTick;
            if (fromTick && Valid(fromTick)) { g_inst = fromTick; return g_inst; }
            if (g_inst && !Valid(g_inst)) g_inst = 0;
            if (!g_inst && allowScan)
            {
                ULONGLONG now = GetTickCount64();
                if (now - g_lastScan > 3000)
                {
                    g_lastScan = now;
                    g_inst = Scan();
                    if (g_inst) LogF("Police: subsystem found @ 0x%p", reinterpret_cast<void*>(g_inst));
                }
            }
            return g_inst;
        }

        // ---- police components (shooting / arresting) -----------------------
        // Offsets from Default__WheelmanPoliceComponent groups.
        struct CompField { int off; float peaceValue; float defaultValue; };
        const CompField kCompFields[] = {
            { 0x5C, 1.0e9f, 5.0f },     // m_fAggressionTrigger
            { 0x60, 1.0e9f, 2.0f },     // m_fMinTimeBeforeShooting
            { 0x64, 1.0e9f, 10.0f },    // m_fMaxTimeBeforeShooting
            { 0x70, 1.0e9f, 200.0f },   // m_fSpeedToShoot
            { 0x7C, -1.0f, 300.0f },    // m_fDistanceToArrestInVehicle
            { 0x80, -1.0f, 150.0f },    // m_fDistanceToArrestOnFoot
            { 0x84, -1.0f, 500.0f },    // m_fMaxVehArrestSpeed
        };

        void ApplyToComponents(bool peace)
        {
            SYSTEM_INFO si; GetSystemInfo(&si);
            uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
            const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
            while (addr < maxAddr)
            {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
                uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
                {
                    __try
                    {
                        for (uintptr_t p = start; p + 0xA0 <= end; p += 4)
                        {
                            if (*reinterpret_cast<const uint32_t*>(p) != kCompVtbl) continue;
                            // Only live components: UObject marker and an owner pointer. Anything else may be
                            // freed memory or a stray copy of the vtable value, and writing there corrupts the heap.
                            if (*reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                            const uint32_t owner = *reinterpret_cast<const uint32_t*>(p + 0x1C);
                            if (owner < 0x10000 || owner > 0x7FFF0000) continue;
                            for (const CompField& f : kCompFields)
                                *reinterpret_cast<float*>(p + f.off) = peace ? f.peaceValue : f.defaultValue;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                addr = end;
            }
        }

        volatile long g_peaceApplied = 0;   // what the worker last applied
        volatile long g_workerStarted = 0;

        DWORD WINAPI PeaceWorker(LPVOID)
        {
            for (;;)
            {
                if (LoadGuard::Quiet()) { Sleep(250); continue; }   // never write into components while a level loads
                long want = peaceful ? 1 : 0;
                if (want || g_peaceApplied)
                {
                    ApplyToComponents(want != 0);
                    g_peaceApplied = want;
                }
                Sleep(want ? 1500 : 500);
            }
        }
    }

    // Runs on the game thread (from the police-subsystem tick hook, or the Mana hook as a fallback).
    void PumpImpl(uintptr_t b)
    {
        if (!b || !Valid(b)) return;

        int want = -1;
        long req = InterlockedExchange(&g_pendingLevel, -1);
        if (neverWanted) want = 0;
        else if (req >= 0) want = static_cast<int>(req);
        else if (lockLevel) want = lockValue < 0 ? 0 : lockValue;
        if (want < 0) return;

        int32_t lvl = 0, mx = 0; float pct = 0.f;
        Rd(b + kLevel, lvl); Rd(b + kMax, mx); Rd(b + kPercent, pct);
        if (lvl != want)
        {
            static bool checked = false, ok = false;
            if (!checked)
            {
                checked = true;
                static const unsigned char sig[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x0C, 0x53, 0x8B, 0x5D, 0x08 };
                ok = memcmp(reinterpret_cast<void*>(kSetLevelFn), sig, sizeof(sig)) == 0;
                if (!ok) LogF("Police: setter prologue mismatch at 0x%p - level changes disabled", reinterpret_cast<void*>(kSetLevelFn));
            }
            if (!ok) return;
            // Level changes spawn / dismiss whole police waves (vehicles, AI, Kismet events). Firing them back to
            // back froze the game once, so successive setter calls are spaced 600 ms apart; a one-shot request that
            // arrives too early is kept for the next update.
            static DWORD lastSetterCall = 0;
            DWORD nowTick = GetTickCount();
            if (nowTick - lastSetterCall < 600)
            {
                if (req >= 0) InterlockedExchange(&g_pendingLevel, req);
                return;
            }
            lastSetterCall = nowTick;
            if (want > mx) Wr<int32_t>(b + kMax, want);
            typedef void(__stdcall * SetLevel_t)(void*, int, int, int);
            __try { reinterpret_cast<SetLevel_t>(kSetLevelFn)(reinterpret_cast<void*>(b), want, 0, -1); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogF("Police: setter raised an exception"); }
            Wr<float>(b + kPercent, static_cast<float>(want));
        }
        else if (std::fabs(pct - static_cast<float>(want)) > 0.001f)
        {
            Wr<float>(b + kPercent, static_cast<float>(want)); // pin the heat so it can't drift and re-trigger the setter
        }
    }

    extern "C" void __cdecl Police_GamePump() { if (!LoadGuard::Quiet()) PumpImpl(g_inst); }

    extern "C" void __cdecl Police_GamePumpFor(void* subsystem)
    {
        if (LoadGuard::Quiet()) return;   // loading / cut-scene: queued work waits, nothing is written
        Spawner::RunPending();   // queued vehicle spawns also run in this per-frame game-thread context
        VehicleMod::RunRepair();
        Weapons::RunPending();
        BuiltinCheats::RunPending();
        GameConsole::RunPending();
        MouseLook::GameTick();
        uintptr_t b = reinterpret_cast<uintptr_t>(subsystem);
        if (!Valid(b)) return;
        g_inst = b;
        PumpImpl(b);
    }

    void Refresh()
    {
        g_lastScan = 0;
        g_inst = 0;
        Instance(true);
    }

    State Read()
    {
        State s;
        uintptr_t b = Instance(false);
        if (!b) return s;
        int32_t lvl = 0, mx = 0; float pct = 0; uint8_t l8 = 0, c8 = 0; uint32_t bits = 0;
        if (!Rd(b + kLevel, lvl) || !Rd(b + kMax, mx) || !Rd(b + kPercent, pct)) return s;
        Rd(b + kLost, l8); Rd(b + kCrime, c8); Rd(b + kBits, bits);
        s.found = true; s.address = b; s.level = lvl; s.maxLevel = mx; s.percent = pct;
        s.lostState = l8; s.lastCrime = c8; s.enabled = (bits & 2u) != 0;
        return s;
    }

    void SetLevel(int level)
    {
        { char b[48]; sprintf_s(b, "police SetLevel(%d)", level); CrashHandler::Note(b); }
        Instance(true);
        InterlockedExchange(&g_pendingLevel, level < 0 ? 0 : level);
        InterlockedExchange(&g_policeWork, 1);
    }

    void LoseNow() { SetLevel(0); }

    void Tick()
    {
        const bool active = neverWanted || lockLevel;
        Instance(active);
        g_policePeaceful = peaceful ? 1 : 0;
        InterlockedExchange(&g_policeWork, (active || g_pendingLevel >= 0 || Spawner::HasPending() || VehicleMod::RepairPending() || Weapons::HasPending() || BuiltinCheats::Pending() || GameConsole::HasPending() || MouseLook::active) ? 1 : 0);

        // No pumping from here: calling the setter from the render hook froze the game (a hash-map chain in the
        // game became a cycle). Requests are applied only inside the police subsystem's own per-frame update
        // (or, if that hook is missing, the Mana hook).

        if ((peaceful || g_peaceApplied) && !g_workerStarted)
        {
            g_workerStarted = 1;
            CreateThread(nullptr, 0, PeaceWorker, nullptr, 0, nullptr);
        }
    }
}

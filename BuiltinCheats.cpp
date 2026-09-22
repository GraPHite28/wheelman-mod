#include <Windows.h>
#include <cstring>
#include "LoadGuard.h"
#include "BuiltinCheats.h"
#include "Patches.h"
#include "VehicleMod.h"
#include "Overlay.h"
#include "Log.h"

namespace BuiltinCheats
{
    bool ammoGod = false;
    float timeScale = 1.f;
    int perfLevel = 0, meleeLevel = 0, healthLevel = 0;
    const char* message = "";

    namespace
    {
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

        enum { kKillMe = 1, kKillVehicle = 2, kFocus = 4, kUpgrades = 8 };
        volatile long g_pending = 0;
        uint32_t g_fake[16] = {};                       // stands in for the cheat manager: only +0x1C (the controller) is read

        constexpr uintptr_t kKillMe_ = 0x00525C60, kKillVehicle_ = 0x00525D60, kFocus_ = 0x00525E50;
        constexpr uintptr_t kUpgrade[3] = { 0x00525EF0, 0x00525F60, 0x00525FD0 };

        uintptr_t Controller()
        {
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            uint32_t c = 0;
            if (!pawn || !Rd(pawn + 0x1D0, c) || c < 0x10000 || c >= 0x7FFF0000) return 0;
            uint32_t vt = 0;
            return Rd(c, vt) && vt >= 0x1000000 && vt <= 0x1800000 ? c : 0;
        }

        void CallMethod0(uintptr_t fn, void* self)
        {
            __asm
            {
                mov ecx, self
                mov eax, fn
                call eax
            }
        }

        // The upgrade functions take the level on the stack and pop it themselves (ret 4); ecx is not used.
        void CallUpgrade(uintptr_t fn, int level)
        {
            __asm
            {
                xor ecx, ecx
                push level
                mov eax, fn
                call eax
            }
        }

        uintptr_t WorldInfo()
        {
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            uint32_t wi = 0;
            return pawn && Rd(pawn + 0x94, wi) && wi > 0x10000 && wi < 0x7FFF0000 ? wi : 0;
        }

        bool g_prevAmmo = false, g_timeApplied = false;

        // ---- debug functions of the player pawn / controller ---------------------------------------------------
        volatile long g_ops = 0;

        void CallVirtual0(uintptr_t obj, int slot)
        {
            __asm
            {
                mov ecx, obj
                mov eax, dword ptr [ecx]
                mov edx, slot
                add eax, edx
                call dword ptr [eax]
            }
        }

        void CallVirtual1(uintptr_t obj, int slot, int arg)
        {
            __asm
            {
                push arg
                mov ecx, obj
                mov eax, dword ptr [ecx]
                mov edx, slot
                add eax, edx
                call dword ptr [eax]
            }
        }

        // Pawn::Resurrect(FVector loc, FRotator rot, bool onlyIfDead): 7 dwords on the stack, callee pops (ret 0x1C).
        void CallResurrect(uintptr_t pawn, const uint32_t* loc, const uint32_t* rot, int onlyIfDead)
        {
            __asm
            {
                mov esi, loc
                mov edi, rot
                push onlyIfDead
                push dword ptr [edi + 8]
                push dword ptr [edi + 4]
                push dword ptr [edi]
                push dword ptr [esi + 8]
                push dword ptr [esi + 4]
                push dword ptr [esi]
                mov ecx, pawn
                mov eax, dword ptr [ecx]
                mov edx, 0x430
                add eax, edx
                call dword ptr [eax]
            }
        }

        // Pawn::Teleport(FVector loc, FRotator rot, bool): vtable slot +0xFC, 7 dwords on the stack, callee pops (ret 0x1C).
        void CallTeleport(uintptr_t pawn, const uint32_t* loc, const uint32_t* rot, int flag)
        {
            __asm
            {
                mov esi, loc
                mov edi, rot
                push flag
                push dword ptr [edi + 8]
                push dword ptr [edi + 4]
                push dword ptr [edi]
                push dword ptr [esi + 8]
                push dword ptr [esi + 4]
                push dword ptr [esi]
                mov ecx, pawn
                mov eax, dword ptr [ecx]
                mov edx, 0xFC
                add eax, edx
                call dword ptr [eax]
            }
        }

        volatile long g_teleportMarker = -1;
        uint32_t g_teleportLoc[3] = {}, g_teleportRot[3] = {};

        struct FlagDef { bool onController; int off; int bit; };
        const FlagDef kFlagDefs[FlagCount] = {
            { true, 0x514, 0 }, { true, 0x4EC, 4 }, { false, 0x1634, 0 },
            { true, 0x514, 3 }, { true, 0x514, 4 }, { true, 0x514, 8 }, { true, 0x514, 9 }, { true, 0x514, 2 },
            { true, 0x514, 12 }, { true, 0x514, 11 }, { true, 0x514, 10 }, { true, 0x514, 13 },
        };
        bool g_flagForced[FlagCount] = {};
        ULONGLONG g_lastGarageCheck = 0, g_lastGarageScan = 0;
    }

    bool flags[FlagCount] = {};
    int fakeGodMinHealth = 100;
    bool garagesAlwaysOpen = false;
    bool antiFailMission = false;

    namespace
    {
        // WheelmanPlayerPawn vtable 0x1354038, slot +0x688 = FailMission (returns bool)
        constexpr uintptr_t kPawnVtable = 0x01354038, kFailSlot = kPawnVtable + 0x688, kFailOriginal = 0x0052D1D0;
        int __fastcall FailStub(void*) { return 0; }
        // WheelmanSeqAct_HUDDisplayFailureMessage (vtable 0x13468A8), slot +0xE8 = Activated. Every mission fail condition of the
        // Kismet mission scripts ends in this action (75 uses in the mission packages, e.g. MIS017 "Felipe spotted you").
        constexpr uintptr_t kFailMsgSlot = 0x013468A8 + 0xE8, kFailMsgOriginal = 0x004F2E80;
        // The same action also shows the arrested / death / mission complete screens: m_eFailureType (byte at +0x110) is
        // 0 ARRESTED, 1 DEATH, 2 MISSION_FAILED, 3 MISSION_COMPLETE. Only the MISSION_FAILED one is swallowed.
        void __fastcall FailMsgStub(void* self)
        {
            unsigned char type = 0xFF;
            __try { type = *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(self) + 0x110); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (type == 2) return;
            reinterpret_cast<void(__fastcall*)(void*)>(kFailMsgOriginal)(self);
        }
        bool g_failPatched = false;

        bool PatchSlot(uintptr_t slotAddr, uintptr_t original, uintptr_t stub, bool on)
        {
            uintptr_t* slot = reinterpret_cast<uintptr_t*>(slotAddr);
            __try
            {
                if (on && *slot != original) return false;     // unexpected content: leave it alone
                DWORD old;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
                *slot = on ? stub : original;
                VirtualProtect(slot, sizeof(void*), old, &old);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void ApplyAntiFail()
        {
            if (antiFailMission == g_failPatched) return;
            const bool a = PatchSlot(kFailSlot, kFailOriginal, reinterpret_cast<uintptr_t>(&FailStub), antiFailMission);
            const bool b = PatchSlot(kFailMsgSlot, kFailMsgOriginal, reinterpret_cast<uintptr_t>(&FailMsgStub), antiFailMission);
            if (a || b)
            {
                g_failPatched = antiFailMission;
                LogF("BuiltinCheats: anti-fail %s (pawn %d, failure message %d)", antiFailMission ? "on" : "off", a ? 1 : 0, b ? 1 : 0);
            }
        }
    }

    void Request(Op op) { InterlockedOr(&g_ops, 1L << op); message = "queued"; }
    void RequestKillMe() { InterlockedOr(&g_pending, kKillMe); message = "queued"; }
    void RequestKillVehicle() { InterlockedOr(&g_pending, kKillVehicle); message = "queued"; }
    void RequestFocusGauge() { InterlockedOr(&g_pending, kFocus); message = "queued"; }
    void RequestUpgrades() { InterlockedOr(&g_pending, kUpgrades); message = "queued"; }
    bool Pending() { return g_pending != 0 || g_ops != 0 || g_teleportMarker >= 0; }

    int CurrentUpgrade(int which)
    {
        const uintptr_t pawn = Overlay::GetPlayerPawn();
        uint8_t v = 0;
        if (which < 0 || which > 2 || !pawn || !Rd(pawn + 0x175C + which, v)) return -1;
        return v;
    }

    void RunPending()
    {
        if (g_teleportMarker >= 0)
        {
            g_teleportMarker = -1;
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            if (!pawn) message = "player pawn not found";
            else
            {
                // In a vehicle the pawn is only attached to the car: moving the pawn leaves the car (and the control)
                // behind, so the vehicle itself is moved, keeping its own heading.
                uint32_t veh = 0;
                if (Rd(pawn + 0x4BC, veh) && veh > 0x10000 && IsLiveVehicle(reinterpret_cast<void*>(veh)))
                {
                    uint32_t rot[3] = {};
                    Rd(static_cast<uintptr_t>(veh) + 0xE0, rot[0]); Rd(static_cast<uintptr_t>(veh) + 0xE4, rot[1]); Rd(static_cast<uintptr_t>(veh) + 0xE8, rot[2]);
                    message = VehicleMod::TeleportVehicle(veh, g_teleportLoc, rot) ? "teleported" : "vehicle teleport failed";
                }
                else
                {
                    bool threw = false;
                    __try { CallTeleport(pawn, g_teleportLoc, g_teleportRot, 0); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
                    message = threw ? "the game function raised an exception" : "teleported";
                }
            }
        }
        if (g_ops)
        {
            const long ops = InterlockedExchange(&g_ops, 0);
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            const uintptr_t ctrl = Controller();
            bool threw = false;
            __try
            {
                for (int op = 0; op < OpCount; ++op)
                {
                    if (!(ops & (1L << op))) continue;
                    if (op <= OpRetryMission)   // pawn mission functions
                    {
                        static const int kPawnSlot[] = { 0x69C, 0x688, 0x68C, 0x690, 0x694, 0x698 };
                        if (!pawn) { message = "player pawn not found"; continue; }
                        CallVirtual0(pawn, kPawnSlot[op]);
                    }
                    else if (op == OpResurrect)
                    {
                        if (!pawn) { message = "player pawn not found"; continue; }
                        uint32_t loc[3] = {}, rot[3] = {};
                        Rd(pawn + 0xD4, loc[0]); Rd(pawn + 0xD8, loc[1]); Rd(pawn + 0xDC, loc[2]);
                        Rd(pawn + 0xE0, rot[0]); Rd(pawn + 0xE4, rot[1]); Rd(pawn + 0xE8, rot[2]);
                        CallResurrect(pawn, loc, rot, 1);
                    }
                    else
                    {
                        if (!ctrl) { message = "player controller not found"; continue; }
                        switch (op)
                        {
                        case OpToggleRadio: CallVirtual0(ctrl, 0x458); break;
                        case OpNextStation: CallVirtual1(ctrl, 0x464, 1); break;
                        case OpPrevStation: CallVirtual1(ctrl, 0x464, 0); break;
                        case OpToggleMap: CallVirtual0(ctrl, 0x46C); break;
                        case OpToggleBigMap: CallVirtual0(ctrl, 0x468); break;
                        case OpTrafficDebug: CallVirtual0(ctrl, 0x47C); break;
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
            message = threw ? "the game function raised an exception" : "done";
            if (threw) LogF("BuiltinCheats: debug function raised an exception (ops 0x%X)", static_cast<unsigned>(ops));
        }
        const long todo = InterlockedExchange(&g_pending, 0);
        if (!todo) return;
        const uintptr_t ctrl = Controller();
        if (!ctrl) { message = "player controller not found (load into the world)"; return; }
        g_fake[7] = static_cast<uint32_t>(ctrl);        // this+0x1C = Outer = PlayerController
        bool threw = false;
        __try
        {
            if (todo & kKillMe) CallMethod0(kKillMe_, g_fake);
            if (todo & kKillVehicle) CallMethod0(kKillVehicle_, g_fake);
            if (todo & kFocus) CallMethod0(kFocus_, g_fake);
            if (todo & kUpgrades)
            {
                const int levels[3] = { perfLevel, meleeLevel, healthLevel };
                for (int i = 0; i < 3; ++i) CallUpgrade(kUpgrade[i], levels[i] < 0 ? 0 : (levels[i] > 15 ? 15 : levels[i]));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
        message = threw ? "the game function raised an exception" : "done";
        if (threw) LogF("BuiltinCheats: call raised an exception (mask 0x%X)", static_cast<unsigned>(todo));
    }

    void KeepGaragesOpen();

    // Forces / releases the bits of the game's own debug switches (see FlagId).
    static void ApplyFlags(uintptr_t ctrl)
    {
        const uintptr_t pawn = Overlay::GetPlayerPawn();
        for (int i = 0; i < FlagCount; ++i)
        {
            const uintptr_t base = kFlagDefs[i].onController ? ctrl : pawn;
            if (!base) continue;
            uint32_t bits = 0;
            if (!Rd(base + kFlagDefs[i].off, bits)) continue;
            const uint32_t mask = 1u << kFlagDefs[i].bit;
            if (flags[i] && !(bits & mask)) { Wr<uint32_t>(base + kFlagDefs[i].off, bits | mask); g_flagForced[i] = true; }
            else if (!flags[i] && g_flagForced[i]) { Wr<uint32_t>(base + kFlagDefs[i].off, bits & ~mask); g_flagForced[i] = false; }
        }
        if (ctrl && flags[FlagFakeGod]) Wr<int32_t>(ctrl + 0x508, fakeGodMinHealth);
    }

    void Tick()
    {
        const uintptr_t ctrl = Controller();
        if (ctrl)
        {
            uint32_t bits = 0;
            if (Rd(ctrl + 0x514, bits))
            {
                if (ammoGod && !(bits & 2)) Wr<uint32_t>(ctrl + 0x514, bits | 2);
                else if (!ammoGod && g_prevAmmo) Wr<uint32_t>(ctrl + 0x514, bits & ~2u);
            }
            g_prevAmmo = ammoGod;
        }
        ApplyFlags(ctrl);
        KeepGaragesOpen();
        ApplyAntiFail();
        // slow motion: the game's SlowMo cheat is GameInfo.SetGameSpeed, which ends in WorldInfo.TimeDilation
        if (timeScale != 1.f || g_timeApplied)
        {
            if (const uintptr_t wi = WorldInfo())
            {
                float ts = timeScale < 0.05f ? 0.05f : (timeScale > 4.f ? 4.f : timeScale);
                Wr<float>(wi + 0x318, ts);
                g_timeApplied = timeScale != 1.f;
            }
        }
    }

    // ---- mission / event markers ----------------------------------------------------------------------------------
    namespace
    {
        struct MarkerKind { uint32_t vtable; const char* label; };
        const MarkerKind kMarkerKinds[] = {
            { 0x0135ECD8, "Mission objective" }, { 0x01360560, "BOP objective" }, { 0x0135EFE8, "Event" },
            { 0x01360A58, "Contracts" }, { 0x01360D88, "Fugitive" }, { 0x013610B0, "Hot potato" }, { 0x013613D8, "Made to Order" },
            { 0x01361700, "Rampage" }, { 0x01361A28, "Street race" }, { 0x01361D50, "Taxi" }, { 0x01362078, "The Hard Path" },
            { 0x01360250, "Event marker" }, { 0x013623A0, "DLC event marker" }, { 0x01362EF8, "Objective (no collision)" },
        };
        constexpr int kMaxMarkers = 400;
        uintptr_t g_markers[kMaxMarkers];
        int g_markerKind[kMaxMarkers];
        int g_markerCount = 0;

        int KindOf(uint32_t vt)
        {
            for (int i = 0; i < static_cast<int>(sizeof(kMarkerKinds) / sizeof(kMarkerKinds[0])); ++i)
                if (kMarkerKinds[i].vtable == vt) return i;
            return -1;
        }

        bool MarkerLive(uintptr_t m, int& kind)
        {
            uint32_t vt = 0; int32_t mark = 0;
            if (m < 0x10000 || !Rd(m, vt) || !Rd(m + 0x18, mark) || mark != -1) return false;
            kind = KindOf(vt);
            return kind >= 0;
        }

        // FString at `addr`: { wchar* data, int num, int max } -> UTF-8
        bool ReadFString(uintptr_t addr, char* out, int cap)
        {
            uint32_t data = 0; int32_t num = 0;
            out[0] = 0;
            if (!Rd(addr, data) || !Rd(addr + 4, num) || data < 0x10000 || num < 2 || num > 200) return false;
            wchar_t w[201] = {};
            for (int i = 0; i < num - 1; ++i)
            {
                wchar_t c = 0;
                if (!Rd(data + i * 2, c) || c < 0x20) return false;
                w[i] = c;
            }
            return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr) > 0;
        }
    }

    int ScanMissionMarkers()
    {
        g_markerCount = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr && g_markerCount < kMaxMarkers)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
            {
                __try
                {
                    for (uintptr_t p = start; p + 0x300 <= end && g_markerCount < kMaxMarkers; p += 4)
                    {
                        const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                        if (vt < 0x01350000 || vt > 0x01370000) continue;
                        const int kind = KindOf(vt);
                        if (kind < 0 || *reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                        g_markers[g_markerCount] = p;
                        g_markerKind[g_markerCount++] = kind;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = end;
        }
        LogF("BuiltinCheats: found %d mission/event markers", g_markerCount);
        return g_markerCount;
    }

    int MarkerCount() { return g_markerCount; }

    bool MarkerAt(int i, MarkerInfo& out)
    {
        int kind = 0;
        if (i < 0 || i >= g_markerCount || !MarkerLive(g_markers[i], kind)) return false;
        const uintptr_t m = g_markers[i];
        out = {};
        out.obj = m;
        out.kind = kMarkerKinds[kind].label;
        Rd(m + 0xD4, out.x); Rd(m + 0xD8, out.y); Rd(m + 0xDC, out.z);
        if (!ReadFString(m + 0x1F0, out.name, sizeof(out.name))) strcpy_s(out.name, "(unnamed)");
        return true;
    }

    void RequestTeleportToMarker(int i)
    {
        int kind = 0;
        if (i < 0 || i >= g_markerCount || !MarkerLive(g_markers[i], kind)) { message = "marker is gone - scan again"; return; }
        const uintptr_t m = g_markers[i];
        float x = 0, y = 0, z = 0;
        Rd(m + 0xD4, x); Rd(m + 0xD8, y); Rd(m + 0xDC, z);
        z += 100.f;
        memcpy(&g_teleportLoc[0], &x, 4); memcpy(&g_teleportLoc[1], &y, 4); memcpy(&g_teleportLoc[2], &z, 4);
        Rd(m + 0xE0, g_teleportRot[0]); Rd(m + 0xE4, g_teleportRot[1]); Rd(m + 0xE8, g_teleportRot[2]);
        InterlockedExchange(&g_teleportMarker, i);
        message = "queued";
    }

    // ---- garages ----------------------------------------------------------------------------------------------------
    namespace
    {
        constexpr uint32_t kGarageVtable = 0x0135FF30;
        constexpr int kMaxGarages = 48;
        uintptr_t g_garages[kMaxGarages];
        int g_garageCount = 0;

        bool LooksLive(uintptr_t g)
        {
            uint32_t vt = 0; int32_t mark = 0;
            return g > 0x10000 && Rd(g, vt) && vt == kGarageVtable && Rd(g + 0x18, mark) && mark == -1;
        }
    }

    int ScanGarages()
    {
        g_garageCount = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr && g_garageCount < kMaxGarages)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
            {
                __try
                {
                    for (uintptr_t p = start; p + 0x2C0 <= end && g_garageCount < kMaxGarages; p += 4)
                    {
                        if (*reinterpret_cast<const uint32_t*>(p) != kGarageVtable) continue;
                        if (*reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                        g_garages[g_garageCount++] = p;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = end;
        }
        LogF("BuiltinCheats: found %d garage objects", g_garageCount);
        return g_garageCount;
    }

    int GarageCount() { return g_garageCount; }

    bool GarageAt(int i, GarageInfo& out)
    {
        if (i < 0 || i >= g_garageCount || !LooksLive(g_garages[i])) return false;
        const uintptr_t g = g_garages[i];
        int32_t state = 0; uint32_t bits = 0;
        out = {};
        out.obj = g;
        Rd(g + 0xD4, out.x); Rd(g + 0xD8, out.y); Rd(g + 0xDC, out.z);
        Rd(g + 0x1E4, state); Rd(g + 0x280, bits);
        out.state = state;
        out.inUse = (bits & 2) != 0;
        out.missionGarage = (bits & 4) != 0;
        return true;
    }

    const char* garageMessage = "";

    // "Garages always open": state 1 (temporarily closed: wanted level, just used ...) and 0 (locked) are forced to 2
    // (available). The garage objects are found by a heap walk that is repeated only when none of the known ones is alive.
    void KeepGaragesOpen()
    {
        if (!garagesAlwaysOpen) return;
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastGarageCheck < 250) return;
        g_lastGarageCheck = now;
        int live = 0;
        for (int i = 0; i < g_garageCount; ++i)
        {
            if (!LooksLive(g_garages[i])) continue;
            ++live;
            int32_t st = 0;
            if (Rd(g_garages[i] + 0x1E4, st) && st != 2) Wr<int32_t>(g_garages[i] + 0x1E4, 2);
        }
        if (live == 0 && Overlay::GetPlayerPawn() && now - g_lastGarageScan > 15000)
        {
            g_lastGarageScan = now;
            ScanGarages();
        }
    }

    int UnlockAllGarages()
    {
        if (g_garageCount == 0) ScanGarages();
        int changed = 0;
        for (int i = 0; i < g_garageCount; ++i)
        {
            int32_t st = 0;
            if (!LooksLive(g_garages[i]) || !Rd(g_garages[i] + 0x1E4, st) || st == 2) continue;
            if (Wr<int32_t>(g_garages[i] + 0x1E4, 2)) ++changed;
        }
        garageMessage = changed ? "garages set to available" : "nothing to unlock (scan while in the world)";
        return changed;
    }

    void SetAllGarageStates(int state)
    {
        for (int i = 0; i < g_garageCount; ++i)
            if (LooksLive(g_garages[i])) Wr<int32_t>(g_garages[i] + 0x1E4, state);
    }
}

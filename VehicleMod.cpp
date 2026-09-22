#include <Windows.h>
#include <cmath>
#include <cstring>
#include "LoadGuard.h"
#include "VehicleMod.h"
#include "Patches.h"
#include "Overlay.h"
#include "Spawner.h"

namespace VehicleMod
{
    bool noDeformation = false;
    bool noCollisionDamage = false;
    bool bulletproof = false;
    bool invulnerableTyres = false;
    bool jackAny = false;
    bool noBikeFall = false;
    bool jackIgnoreOccupants = false;
    const char* jackGateStatus = "off";

    const TuningField kTuning[] = {
        { "Engine",       "Max torque (Nm) - acceleration", 0x274, 0x14, 50.f },
        { "Engine",       "Min RPM",                        0x274, 0x08, 100.f },
        { "Engine",       "Optimal RPM",                    0x274, 0x0C, 100.f },
        { "Engine",       "Max RPM",                        0x274, 0x10, 100.f },
        { "Engine",       "Torque factor at min RPM",       0x274, 0x18, 0.05f },
        { "Engine",       "Torque factor at max RPM",       0x274, 0x1C, 0.05f },
        { "Transmission", "Upshift RPM",                    0x280, 0x0C, 100.f },
        { "Transmission", "Primary ratio (lower = higher top speed)", 0x280, 0x10, 0.1f },
        { "Transmission", "Clutch delay (s)",               0x280, 0x14, 0.05f },
        { "Transmission", "Reverse gear ratio",             0x280, 0x18, 0.1f },
        { "Aerodynamics", "Drag coefficient",               0x26C, 0x10, 0.005f },
        { "Aerodynamics", "Lift coefficient (negative = downforce)", 0x26C, 0x14, 0.005f },
        { "Aerodynamics", "Air density",                    0x26C, 0x08, 0.1f },
        { "Aerodynamics", "Frontal area (m^2)",             0x26C, 0x0C, 0.1f },
        { "Steering",     "Max steering angle (radians)",   0x27C, 0x08, 0.05f },
        { "World",        "Gravity Z (m/s^2)",              0x268, 0x18, 0.5f },
        { "Vehicle sim",  "Top speed limit (m/s; 44.7 = 100 mph)", -2, 0x178, 2.f },
        { "Profile",      "Top speed (mph) - design value, no effect", -1, 0x130, 5.f },
        { "Profile",      "Mass (kg) - design value",       -1,    0x3C, 50.f },
    };
    const int kTuningCount = sizeof(kTuning) / sizeof(kTuning[0]);

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
        bool Ptr(uintptr_t base, int off, uintptr_t& out)
        {
            uint32_t v = 0;
            if (base < 0x10000 || !Rd(base + off, v)) return false;
            out = v;
            return out >= 0x10000 && out < 0x7FFF0000;
        }
    }

    uintptr_t FieldAddress(uintptr_t profile, int i)
    {
        if (i < 0 || i >= kTuningCount) return 0;
        const TuningField& f = kTuning[i];
        if (f.ptrOffset == -2)
        {
            // Live per-vehicle simulation object (vtable 0x142DB38) at sim+0x44; +0x178 is the speed cap the engine
            // actually enforces. The profile's own +0x130 is only a design value.
            uintptr_t v = reinterpret_cast<uintptr_t>(g_pVehicle), data = 0, sim = 0, obj = 0;
            uint32_t vt = 0;
            if (!v || !Ptr(v, 0x4D8, data) || !Ptr(data, 0, sim) || !Ptr(sim, 0x44, obj)) return 0;
            if (!Rd(obj, vt) || vt != 0x0142DB38) return 0;
            return obj + f.offset;
        }
        if (f.ptrOffset < 0) return profile + f.offset;
        uintptr_t obj = 0;
        if (!Ptr(profile, f.ptrOffset, obj)) return 0;
        return obj + f.offset;
    }

    namespace
    {
        // Original values, captured the first time a vehicle is seen (before any write).
        struct Orig
        {
            uintptr_t vehicle = 0;
            uint32_t bits6C8 = 0, physBits = 0;
            float collision = 0, engineMul = 0, fuelMul = 0, removeRiderPlayer = 0, bikeFlip = 0;
            bool valid = false;
        } g_orig;
        bool g_prevNoColl = false, g_prevBullet = false, g_prevTyres = false, g_prevNoBikeFall = false;
        // WheelmanVehicle m_fCA_MinSpeedChangeRemoveRiderPlayer / m_fCA_BikeFlipAmountRemoveRider: a rider is thrown
        // only when a crash's speed change / flip amount passes these per-vehicle thresholds.
        constexpr int kRemoveRiderPlayer = 0x790, kBikeFlip = 0x79C;

        constexpr int kBits6C8 = 0x6C8, kCollision = 0x68C, kEngineMul = 0xE64, kFuelMul = 0xE68;
        constexpr uint32_t kBulletMask = (1u << 1) | (1u << 2) | (1u << 3);
        constexpr int kPhysBits = 0x1E0, kImpact = 0x250, kDamageListNum = 0x258;

        uintptr_t PhysComp(uintptr_t v)
        {
            uintptr_t pc = 0;
            return Ptr(v, 0x170, pc) ? pc : 0;
        }

        void Capture(uintptr_t v)
        {
            g_orig = Orig();
            g_orig.vehicle = v;
            Rd(v + kBits6C8, g_orig.bits6C8);
            Rd(v + kCollision, g_orig.collision);
            Rd(v + kEngineMul, g_orig.engineMul);
            Rd(v + kFuelMul, g_orig.fuelMul);
            Rd(v + kRemoveRiderPlayer, g_orig.removeRiderPlayer);
            Rd(v + kBikeFlip, g_orig.bikeFlip);
            if (uintptr_t pc = PhysComp(v)) Rd(pc + kPhysBits, g_orig.physBits);
            g_orig.valid = true;
            g_prevNoColl = g_prevBullet = g_prevTyres = g_prevNoBikeFall = false;
        }

        // ---- tuning originals, per profile pointer -----------------------------
        struct ProfileOrig { uintptr_t profile = 0; float values[32] = {}; bool have[32] = {}; };
        ProfileOrig g_profiles[8];
        int g_profileNext = 0;

        ProfileOrig* FindOrCapture(uintptr_t profile)
        {
            for (ProfileOrig& p : g_profiles) if (p.profile == profile) return &p;
            ProfileOrig& p = g_profiles[g_profileNext++ % 8];
            p = ProfileOrig();
            p.profile = profile;
            for (int i = 0; i < kTuningCount && i < 32; ++i)
            {
                uintptr_t a = FieldAddress(profile, i);
                p.have[i] = a && Rd(a, p.values[i]);
            }
            return &p;
        }
    }

    bool ProfilePlausible(uintptr_t profile)
    {
        float mass = 0, top = 0;
        return profile && Rd(profile + 0x3C, mass) && Rd(profile + 0x130, top) &&
               mass > 100.f && mass < 50000.f && top > 10.f && top < 1000.f;
    }

    uintptr_t Profile()
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(g_pVehicle), data = 0, sim = 0, prof = 0;
        if (!v || !Ptr(v, 0x4D8, data) || !Ptr(data, 0, sim)) return 0;
        if (Ptr(sim, 0xCC, prof) && ProfilePlausible(prof)) { FindOrCapture(prof); return prof; }
        if (Ptr(sim, 0x30, prof) && ProfilePlausible(prof)) { FindOrCapture(prof); return prof; }
        return 0;
    }

    bool topSpeedLock = false;
    float topSpeedMs = 0.f;

    namespace
    {
        int TopSpeedIndex()
        {
            static int idx = -2;
            if (idx == -2) { idx = -1; for (int i = 0; i < kTuningCount; ++i) if (kTuning[i].ptrOffset == -2 && kTuning[i].offset == 0x178) idx = i; }
            return idx;
        }
    }

    bool CurrentTopSpeed(float& ms)
    {
        const int i = TopSpeedIndex();
        if (i < 0 || !g_pVehicle || !IsLiveVehicle(g_pVehicle, true)) return false;
        const uintptr_t a = FieldAddress(0, i);
        float v = 0.f;
        if (!a || !Rd(a, v) || !(v > 0.f) || v > 1000.f) return false;
        ms = v;
        return true;
    }

    void KeepTopSpeed()
    {
        if (!topSpeedLock) return;
        const int i = TopSpeedIndex();
        if (i < 0 || !g_pVehicle || !IsLiveVehicle(g_pVehicle, true)) return;
        const uintptr_t a = FieldAddress(0, i);
        float cur = 0.f;
        if (!a || !Rd(a, cur)) return;
        if (!(topSpeedMs > 0.f)) { if (cur > 0.f && cur < 1000.f) topSpeedMs = cur; return; }   // first use: adopt the current limit
        if (std::fabs(cur - topSpeedMs) > 0.01f) Wr<float>(a, topSpeedMs);
    }

    void ResetTuning()
    {
        uintptr_t prof = Profile();
        if (!prof) return;
        ProfileOrig* o = FindOrCapture(prof);
        for (int i = 0; i < kTuningCount && i < 32; ++i)
            if (o->have[i]) { uintptr_t a = FieldAddress(prof, i); if (a) Wr<float>(a, o->values[i]); }
    }

    namespace
    {
        // ---- jack any vehicle: +0x6C8 b9 AllowCarToCarJack, b10 AllowOnFootJack ----
        constexpr uint32_t kJackMask = (1u << 9) | (1u << 10);
        struct JackOrig { uintptr_t vehicle = 0; uint32_t bits = 0; };
        JackOrig g_jack[128];
        int g_jackCount = 0;
        bool g_prevJack = false;

        void ApplyJack()
        {
            if (jackAny)
            {
                auto apply = [](uintptr_t v)
                {
                    if (!v || !IsLiveVehicle(reinterpret_cast<void*>(v))) return;
                    uint32_t bits = 0;
                    if (!Rd(v + kBits6C8, bits)) return;
                    if ((bits & kJackMask) == kJackMask) return;
                    bool known = false;
                    for (int i = 0; i < g_jackCount; ++i) if (g_jack[i].vehicle == v) { known = true; break; }
                    if (!known && g_jackCount < 128) { g_jack[g_jackCount].vehicle = v; g_jack[g_jackCount].bits = bits; ++g_jackCount; }
                    Wr<uint32_t>(v + kBits6C8, bits | kJackMask);
                };
                apply(reinterpret_cast<uintptr_t>(g_pVehicle));
                const DWORD now = GetTickCount();
                for (int i = 0; i < g_vehicleListCount; ++i)
                    if (now - g_vehicleList[i].lastSeenTick < 5000)   // only vehicles the game touched recently
                        apply(reinterpret_cast<uintptr_t>(g_vehicleList[i].ptr));
            }
            else if (g_prevJack)
            {
                for (int i = 0; i < g_jackCount; ++i)
                {
                    uint32_t bits = 0;
                    if (IsLiveVehicle(reinterpret_cast<void*>(g_jack[i].vehicle)) && Rd(g_jack[i].vehicle + kBits6C8, bits))
                        Wr<uint32_t>(g_jack[i].vehicle + kBits6C8, (bits & ~kJackMask) | (g_jack[i].bits & kJackMask));
                }
                g_jackCount = 0;
            }
            g_prevJack = jackAny;
        }

        // ---- car-to-car jack: ignore the passenger rules ------------------------------------------------------
        // Vehicle::CanBeCarJacked (0x4B11B0(target vehicle, player pawn) -> 1 = allowed) refuses a target that
        //  (a) has 2+ occupants while a passenger is alive (the block at 0x4B1228, skipped when the occupant-count
        //      comparison `cmp eax,2 / jl 0x4B1287` jumps), and
        //  (b) has fewer occupants than the player's free-seat requirement (`cmp eax,esi / jl fail` at 0x4B130D).
        // Both gates are patched while the option is on: (a) jl -> jmp, (b) the 6-byte jl is NOPed. The vehicle's own
        // AllowCarToCarJack bit and the other checks (dead vehicle, helicopters / trains ...) still apply.
        constexpr uintptr_t kGateA = 0x004B122B;   // 7C 5A
        constexpr uintptr_t kGateB = 0x004B130F;   // 0F 8C 59 FF FF FF
        bool g_gatesPatched = false;

        bool PatchBytes(uintptr_t at, const uint8_t* expect, const uint8_t* now, int len)
        {
            uint8_t cur[8] = {};
            for (int i = 0; i < len; ++i) if (!Rd(at + i, cur[i])) return false;
            if (memcmp(cur, expect, len) != 0) return false;
            DWORD old = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
            memcpy(reinterpret_cast<void*>(at), now, len);
            VirtualProtect(reinterpret_cast<void*>(at), len, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
            return true;
        }

        void ApplyJackGates()
        {
            static const uint8_t origA[2] = { 0x7C, 0x5A }, patchA[2] = { 0xEB, 0x5A };
            static const uint8_t origB[6] = { 0x0F, 0x8C, 0x59, 0xFF, 0xFF, 0xFF }, patchB[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
            if (jackIgnoreOccupants && !g_gatesPatched)
            {
                const bool a = PatchBytes(kGateA, origA, patchA, 2);
                const bool b = PatchBytes(kGateB, origB, patchB, 6);
                g_gatesPatched = a || b;
                jackGateStatus = a && b ? "patched" : (a || b ? "partly patched" : "code does not match - not patched");
            }
            else if (!jackIgnoreOccupants && g_gatesPatched)
            {
                PatchBytes(kGateA, patchA, origA, 2);
                PatchBytes(kGateB, patchB, origB, 6);
                g_gatesPatched = false;
                jackGateStatus = "off";
            }
        }
    }

    const char* repairMessage = "";
    namespace
    {
        volatile long g_repairPending = 0;

        // Vehicle::Reset(bEjectOccupants) - thiscall, callee pops the argument. With 1 it first throws every occupant
        // out (seat loop calling occupant vfunc +0x620); with 0 that loop is skipped and everybody stays seated.
        void CallVehicleReset(void* vehicle, int eject)
        {
            __asm
            {
                mov ecx, vehicle
                mov eax, eject
                push eax
                mov edx, 0x00493400
                call edx
            }
        }

        // Pawn::LeaveVehicle(vehicle) - the occupant's vfunc +0x620 (0x52EB20 for the player), thiscall, callee pops 4.
        // It is what the vehicle reset / garage call to throw occupants out.
        void CallLeaveVehicle(uintptr_t fn, uintptr_t pawn, uintptr_t vehicle)
        {
            __asm
            {
                mov eax, fn
                mov ecx, pawn
                mov edx, vehicle
                push edx
                call eax
            }
        }

        // Pawn::EnterSeat(vehicle, seat, 0) - the occupant's vfunc +0x61C (0x52EAB0 for the player), thiscall, callee pops 12.
        void CallEnterSeat(uintptr_t fn, uintptr_t occupant, uintptr_t vehicle, int seat)
        {
            __asm
            {
                mov eax, fn
                mov ecx, occupant
                mov esi, vehicle
                mov edi, seat
                push 0
                push edi
                push esi
                call eax
            }
        }

        // Physics component vfunc +0x150 (0xA823A0): SetLocationAndRotation(pos, rot, resetVelocity) - what the garage
        // uses to put the reset vehicle back into the world; without it the rebuilt simulation is never re-attached
        // and the car can't be driven. thiscall, structs by value (pos on top), callee pops 0x1C.
        void CallSetTransform(uintptr_t fn, uintptr_t pc, const uint32_t* pos, const uint32_t* rot)
        {
            __asm
            {
                mov eax, fn
                mov ecx, pc
                mov esi, pos
                mov edi, rot
                push 1
                push dword ptr [edi + 8]
                push dword ptr [edi + 4]
                push dword ptr [edi]
                push dword ptr [esi + 8]
                push dword ptr [esi + 4]
                push dword ptr [esi]
                call eax
            }
        }

        // Vehicle vfunc +0x630 (0x493100)(0, 1) - the last step of the garage routine, thiscall, callee pops 8.
        void CallVehicleFinish(uintptr_t fn, void* vehicle)
        {
            __asm
            {
                mov eax, fn
                mov ecx, vehicle
                push 1
                push 0
                call eax
            }
        }
    }

    namespace { volatile long g_exitPending = 0; }
    void RequestRepair() { InterlockedExchange(&g_repairPending, 1); repairMessage = "queued"; }
    void RequestExit() { InterlockedExchange(&g_exitPending, 1); repairMessage = "exit queued"; }
    namespace { volatile long g_deletePending = 0; uintptr_t g_deleteVeh = 0; ULONGLONG g_deleteAt = 0; }
    void RequestDelete() { InterlockedExchange(&g_deletePending, 1); repairMessage = "delete queued"; }
    bool RepairPending() { return g_repairPending != 0 || g_exitPending != 0 || g_deletePending != 0 || g_deleteVeh != 0 || HornSirenPending(); }

    // Emergency exit: the player's own LeaveVehicle (pawn vfunc +0x620, argument = the vehicle from pawn+0x4BC).
    void RunExit()
    {
        InterlockedExchange(&g_exitPending, 0);
        const uintptr_t pawn = Overlay::GetPlayerPawn();
        uint32_t veh = 0, vt = 0, fn = 0;
        if (!pawn || !Rd(pawn + 0x4BC, veh) || veh < 0x10000) { repairMessage = "not in a vehicle"; return; }
        if (!IsLiveVehicle(reinterpret_cast<void*>(veh))) { repairMessage = "vehicle pointer looks stale"; return; }
        if (!Rd(pawn, vt) || vt < 0x1000000 || vt > 0x1800000 || !Rd(vt + 0x620, fn) || fn < 0x401000 || fn > 0xD00000) { repairMessage = "LeaveVehicle slot not found"; return; }
        bool threw = false;
        __try { CallLeaveVehicle(fn, pawn, veh); }
        __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
        repairMessage = threw ? "exit raised an exception" : "left the vehicle";
    }

    bool TeleportVehicle(uintptr_t va, const uint32_t* pos, const uint32_t* rot)
    {
        uint32_t pc = 0, pvt = 0, setTransform = 0;
        if (!va || !IsLiveVehicle(reinterpret_cast<void*>(va)) || !Rd(va + 0x170, pc) || pc < 0x10000 || !Rd(pc, pvt) ||
            pvt < 0x1000000 || pvt > 0x1800000 || !Rd(pvt + 0x150, setTransform) || setTransform < 0x401000 || setTransform > 0xD00000)
            return false;
        bool threw = false;
        __try { CallSetTransform(setTransform, pc, pos, rot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
        return !threw;
    }

    // Delete my vehicle: the player is thrown out first (LeaveVehicle), the vehicle is destroyed ~0.6 s later.
    void RunDelete()
    {
        if (g_deletePending)
        {
            InterlockedExchange(&g_deletePending, 0);
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            uint32_t veh = 0, vt = 0, fn = 0;
            const bool inside = pawn && Rd(pawn + 0x4BC, veh) && veh > 0x10000 && IsLiveVehicle(reinterpret_cast<void*>(veh));
            if (!inside) veh = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_pVehicle));
            if (!veh || !IsLiveVehicle(reinterpret_cast<void*>(veh))) { repairMessage = "no vehicle to delete"; return; }
            if (inside && Rd(pawn, vt) && vt >= 0x1000000 && vt <= 0x1800000 && Rd(vt + 0x620, fn) && fn >= 0x401000 && fn <= 0xD00000)
            {
                __try { CallLeaveVehicle(fn, pawn, veh); }
                __except (EXCEPTION_EXECUTE_HANDLER) { repairMessage = "exit raised an exception"; return; }
            }
            g_deleteVeh = veh;
            g_deleteAt = GetTickCount64() + 600;
            repairMessage = "deleting...";
        }
        if (g_deleteVeh && GetTickCount64() >= g_deleteAt)
        {
            const uintptr_t v = g_deleteVeh;
            g_deleteVeh = 0;
            if (!IsLiveVehicle(reinterpret_cast<void*>(v))) { repairMessage = "the vehicle is already gone"; return; }
            repairMessage = Spawner::DestroyActorNow(v) ? "vehicle deleted" : "the game refused to destroy the vehicle";
        }
    }

    void RunRepair()
    {
        RunDelete();
        RunHornSirenIfQueued();
        if (g_exitPending) RunExit();
        if (!g_repairPending) return;
        InterlockedExchange(&g_repairPending, 0);
        void* v = g_pVehicle;
        if (!v || !IsLiveVehicle(v, true)) { repairMessage = "no vehicle"; return; }
        const uintptr_t va = reinterpret_cast<uintptr_t>(v);
        int32_t maxHealth = 0;
        uint32_t sim = 0;
        if (!Rd(va + 0x628, maxHealth) || maxHealth <= 0 || maxHealth > 1000000) { repairMessage = "max health unreadable"; return; }
        if (!Rd(va + 0x354, sim) || sim < 0x10000) { repairMessage = "vehicle has no simulation object (+0x354) - not repaired"; return; }
        uint32_t pos[3] = {}, rot[3] = {};
        if (!Rd(va + 0xD4, pos[0]) || !Rd(va + 0xD8, pos[1]) || !Rd(va + 0xDC, pos[2]) ||
            !Rd(va + 0xE0, rot[0]) || !Rd(va + 0xE4, rot[1]) || !Rd(va + 0xE8, rot[2])) { repairMessage = "can't read the vehicle transform"; return; }
        // The garage resets with occupants ejected and re-seats them; re-seating left the player sitting in the car
        // without control (no way out either). Reset(0) skips the ejection loop, so nobody has to be put back:
        // reset the parts / simulation, restore the health, re-attach the physics at the current place.
        bool threw = false;
        __try
        {
            CallVehicleReset(v, 0);
            Wr<int32_t>(va + 0x2AC, maxHealth);
            uint32_t pc = 0, pvt = 0, setTransform = 0;
            if (Rd(va + 0x170, pc) && pc > 0x10000 && Rd(pc, pvt) && pvt >= 0x1000000 && pvt <= 0x1800000 &&
                Rd(pvt + 0x150, setTransform) && setTransform >= 0x401000 && setTransform <= 0xD00000)
                CallSetTransform(setTransform, pc, pos, rot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
        repairMessage = threw ? "repair raised an exception" : "repaired";
    }

    // ---- car-to-car jack from any distance ----------------------------------------------------------------------
    // WheelmanVehicleToVehicleJackDetection (vtable 0x132C600, one live instance) finds a target when its bounding
    // boxes overlap: its 16 tuning floats at +0x4C are (in memory order) rotation threshold, speed-difference threshold
    // (mph), min player / traffic speed (mph), front box width / length / offset (m), box collision scale, relative push
    // offset / speed, relative length scale, timeout, speed-match radius, min valid target time, inner box scale min / max.
    // The Parameters object (found by its default float signature) holds the jump arc: min/max distance (m) at +0x38/+0x3C
    // and jump speed at +0x40. While the option is on the boxes are made huge and the constraints are dropped, the arc
    // can span any distance; the originals are put back when it is switched off.
    bool jackAnywhere = false;
    const char* jackAnywhereStatus = "off";
    namespace
    {
        constexpr uint32_t kDetectVtable = 0x0132C600;
        constexpr int kDetectN = 7;
        struct DetectSaved { uintptr_t rec; float f[kDetectN]; };
        DetectSaved g_detect[4];
        int g_detectCount = 0;
        struct ParamsSaved { uintptr_t obj; float arcMin, arcMax, speed; } g_params = {};
        uintptr_t g_foundRec[4] = {};
        uintptr_t g_foundParams = 0;
        volatile long g_scanThread = 0;
        // Measured on the live object while a target 31 m away had a RED marker (detected but not usable): the outer box
        // (candidates) was 33 m long, the inner box (usable, green marker) 21 m. Inner = outer * t with
        // t = (relative speed - MinRel) / (MaxRel - MinRel) clamped to 0..1 (fields +0x84 / +0x88: -20 / 60 mph; 28 mph
        // gave 0.62 = 21/33), and the box length grows with kfRelativeLengthScale (+0x74, 2.5) times the relative speed.
        // So the conditions are relaxed (rotation, speed difference, minimum speeds), the inner box is forced to the full
        // outer size (MinRel/MaxRel far below any speed -> t = 1) and the length scale is raised. The box width / base
        // length (+0x5C / +0x60) stay standard.
        constexpr int kDetectOffs[kDetectN] = { 0x4C, 0x50, 0x54, 0x58, 0x74, 0x84, 0x88 };
        constexpr float kDetectNew[kDetectN] = { -1.f, 100000.f, 0.f, 0.f, 12.f, -100000.f, -99999.f };

        // Heap scan of one region (own function: __try can't share one with objects that need unwinding).
        void ScanRegionForJack(uintptr_t start, uintptr_t end)
        {
            __try
            {
                for (uintptr_t p = start; p + 0x130 <= end; p += 4)
                {
                    const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                    if (vt == kDetectVtable && *reinterpret_cast<const int32_t*>(p + 0x18) == -1)
                    {
                        const float w = *reinterpret_cast<const float*>(p + 0x5C), l = *reinterpret_cast<const float*>(p + 0x60);
                        if (w > 0.5f && w < 5000.f && l > 1.f && l < 10000.f)
                            for (int i = 0; i < 4; ++i) if (!g_foundRec[i]) { g_foundRec[i] = p; break; }
                    }
                    else if (*reinterpret_cast<const float*>(p + 0x30) == 0.5f && *reinterpret_cast<const float*>(p + 0x34) == 1.5f &&
                             *reinterpret_cast<const float*>(p + 0x38) == 5.0f && *reinterpret_cast<const float*>(p + 0x3C) == 35.0f &&
                             *reinterpret_cast<const int32_t*>(p + 0x18) == -1)
                        g_foundParams = p;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        DWORD WINAPI JackScanWorker(LPVOID)
        {
            for (;;)
            {
                if (LoadGuard::Quiet()) { Sleep(250); continue; }
                if (jackAnywhere)
                {
                    for (int i = 0; i < 4; ++i) g_foundRec[i] = 0;
                    g_foundParams = 0;
                    SYSTEM_INFO si; GetSystemInfo(&si);
                    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
                    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
                    while (addr < maxAddr && jackAnywhere)
                    {
                        MEMORY_BASIC_INFORMATION mbi;
                        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
                        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
                        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x130) ScanRegionForJack(start, end);
                        addr = end;
                    }
                    Sleep(4000);
                }
                else Sleep(500);
            }
        }

        bool DetectOk(uintptr_t rec)
        {
            uint32_t vt = 0; int32_t mark = 0;
            return rec > 0x10000 && Rd(rec, vt) && vt == kDetectVtable && Rd(rec + 0x18, mark) && mark == -1;
        }

        void ApplyJackAnywhere()
        {
            static bool prev = false;
            if (jackAnywhere && !g_scanThread)
            {
                InterlockedExchange(&g_scanThread, 1);
                CreateThread(nullptr, 0, JackScanWorker, nullptr, 0, nullptr);
            }
            if (jackAnywhere)
            {
                int active = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const uintptr_t rec = g_foundRec[i];
                    if (!rec || !DetectOk(rec)) continue;
                    DetectSaved* s = nullptr;
                    for (int k = 0; k < g_detectCount; ++k) if (g_detect[k].rec == rec) s = &g_detect[k];
                    if (!s && g_detectCount < 4)
                    {
                        s = &g_detect[g_detectCount++];
                        s->rec = rec;
                        for (int k = 0; k < kDetectN; ++k) Rd(rec + kDetectOffs[k], s->f[k]);
                    }
                    if (!s) continue;
                    for (int k = 0; k < kDetectN; ++k) Wr<float>(rec + kDetectOffs[k], kDetectNew[k]);
                    ++active;
                }
                const uintptr_t po = g_foundParams;
                if (po)
                {
                    if (g_params.obj != po) { g_params.obj = po; Rd(po + 0x38, g_params.arcMin); Rd(po + 0x3C, g_params.arcMax); Rd(po + 0x40, g_params.speed); }
                    Wr<float>(po + 0x38, 0.f); Wr<float>(po + 0x3C, 5000.f); Wr<float>(po + 0x40, 90.f);
                }
                jackAnywhereStatus = active ? (po ? "active (detection + jump arc)" : "active (detection; jump arc object not found)") : "searching for the jack detection object...";
            }
            else if (prev)
            {
                for (int k = 0; k < g_detectCount; ++k)
                    if (DetectOk(g_detect[k].rec)) for (int j = 0; j < kDetectN; ++j) Wr<float>(g_detect[k].rec + kDetectOffs[j], g_detect[k].f[j]);
                g_detectCount = 0;
                if (g_params.obj) { Wr<float>(g_params.obj + 0x38, g_params.arcMin); Wr<float>(g_params.obj + 0x3C, g_params.arcMax); Wr<float>(g_params.obj + 0x40, g_params.speed); g_params.obj = 0; }
                jackAnywhereStatus = "off";
            }
            prev = jackAnywhere;
        }
    }

    // ---- horn / siren -------------------------------------------------------------------------------------------
    // Setting the "active" bits of the vehicle does nothing audible; the game's own routines have to run. The script
    // natives (execStartHorn ... in the WheelmanVehicle native table at 0x15B1090) are one-line wrappers around
    // virtual methods taking no arguments: vehicle vtable +0x688 StartHorn, +0x68C StopHorn, +0x690 StartSiren,
    // +0x694 StopSiren, +0x698 ToggleSiren, +0x69C StartSirenLights, +0x6A0 StopSirenLights, +0x6A4 ToggleSirenLights.
    // The options are switches (bind them to a key, Hold mode for the horn); the calls are made on the game thread.
    bool hornOn = false, sirenOn = false, sirenLightsOn = false;
    namespace
    {
        enum { kStartHorn = 0x688, kStopHorn = 0x68C, kStartSiren = 0x690, kStopSiren = 0x694, kStartLights = 0x69C, kStopLights = 0x6A0 };
        volatile long g_hsPending = 0;               // 1 while some call is queued
        struct HsCall { uintptr_t vehicle; int slot; };
        HsCall g_hsQueue[8];
        int g_hsCount = 0;
        bool g_prevHorn = false, g_prevSiren = false, g_prevLights = false;
        uintptr_t g_hornVeh = 0, g_sirenVeh = 0, g_lightsVeh = 0;   // vehicle each function was started on

        void QueueHs(uintptr_t v, int slot)
        {
            if (g_hsCount < 8) g_hsQueue[g_hsCount++] = { v, slot };
            InterlockedExchange(&g_hsPending, 1);
        }

        void CallVirtual0(void* vehicle, int slot)
        {
            __asm
            {
                mov ecx, vehicle
                mov eax, dword ptr [ecx]
                mov edx, slot
                add eax, edx
                call dword ptr [eax]
            }
        }

        // One switch: start on the current vehicle when it turns on, stop on the vehicle it was started on when it turns
        // off (or when the player changes vehicle while it is on).
        void HsSwitch(bool on, bool& prev, uintptr_t& startedOn, int startSlot, int stopSlot, uintptr_t cur)
        {
            if (on && cur && cur != startedOn)
            {
                if (startedOn) QueueHs(startedOn, stopSlot);
                QueueHs(cur, startSlot);
                startedOn = cur;
            }
            else if (!on && prev)
            {
                if (startedOn) QueueHs(startedOn, stopSlot);
                startedOn = 0;
            }
            prev = on;
        }

        void TickHornSiren()
        {
            uintptr_t cur = reinterpret_cast<uintptr_t>(g_pVehicle);
            if (cur && !IsLiveVehicle(reinterpret_cast<void*>(cur), true)) cur = 0;
            HsSwitch(hornOn, g_prevHorn, g_hornVeh, kStartHorn, kStopHorn, cur);
            HsSwitch(sirenOn, g_prevSiren, g_sirenVeh, kStartSiren, kStopSiren, cur);
            HsSwitch(sirenLightsOn, g_prevLights, g_lightsVeh, kStartLights, kStopLights, cur);
        }

        void RunHornSiren()
        {
            InterlockedExchange(&g_hsPending, 0);
            HsCall calls[8];
            const int n = g_hsCount;
            memcpy(calls, g_hsQueue, sizeof(HsCall) * n);
            g_hsCount = 0;
            for (int i = 0; i < n; ++i)
            {
                if (!IsLiveVehicle(reinterpret_cast<void*>(calls[i].vehicle))) continue;   // gone: nothing to stop
                uint32_t vt = 0, fn = 0;
                if (!Rd(calls[i].vehicle, vt) || !Rd(vt + calls[i].slot, fn) || fn < 0x401000 || fn > 0xD00000) continue;
                __try { CallVirtual0(reinterpret_cast<void*>(calls[i].vehicle), calls[i].slot); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }

    bool HornSirenPending() { return g_hsPending != 0; }
    void RunHornSirenIfQueued() { if (g_hsPending) RunHornSiren(); }

    void Tick()
    {
        TickHornSiren();
        ApplyJack();
        ApplyJackGates();
        ApplyJackAnywhere();
        uintptr_t v = reinterpret_cast<uintptr_t>(g_pVehicle);
        // Never write into a stale / freed vehicle (that corrupts the game's pool allocator).
        if (!v || !IsLiveVehicle(reinterpret_cast<void*>(v), true)) { g_orig = Orig(); return; }
        if (!g_orig.valid || g_orig.vehicle != v) Capture(v);
        uintptr_t pc = PhysComp(v);

        // ---- bullet proof ----
        if (bulletproof)
        {
            uint32_t bits = 0;
            if (Rd(v + kBits6C8, bits) && (bits & kBulletMask) != kBulletMask) Wr<uint32_t>(v + kBits6C8, bits | kBulletMask);
            Wr<float>(v + kEngineMul, 0.f);
            Wr<float>(v + kFuelMul, 0.f);
        }
        else if (g_prevBullet)
        {
            uint32_t bits = 0;
            if (Rd(v + kBits6C8, bits)) Wr<uint32_t>(v + kBits6C8, (bits & ~kBulletMask) | (g_orig.bits6C8 & kBulletMask));
            Wr<float>(v + kEngineMul, g_orig.engineMul);
            Wr<float>(v + kFuelMul, g_orig.fuelMul);
        }
        g_prevBullet = bulletproof;

        // ---- collision damage to the health bar ----
        if (noCollisionDamage) Wr<float>(v + kCollision, 0.f);
        else if (g_prevNoColl) Wr<float>(v + kCollision, g_orig.collision);
        g_prevNoColl = noCollisionDamage;

        // ---- can't fall off a bike: push both thresholds out of reach ----
        if (noBikeFall)
        {
            Wr<float>(v + kRemoveRiderPlayer, 1.0e9f);
            Wr<float>(v + kBikeFlip, 1.0e9f);
        }
        else if (g_prevNoBikeFall)
        {
            Wr<float>(v + kRemoveRiderPlayer, g_orig.removeRiderPlayer);
            Wr<float>(v + kBikeFlip, g_orig.bikeFlip);
        }
        g_prevNoBikeFall = noBikeFall;

        if (!pc) return;

        // ---- body deformation: drop queued impact damage every frame ----
        if (noDeformation)
        {
            Wr<float>(pc + kImpact, 0.f);
            Wr<int32_t>(pc + kDamageListNum, 0);
        }

        // ---- tyres ----
        uint32_t pb = 0;
        if (Rd(pc + kPhysBits, pb))
        {
            bool cur = (pb & (1u << 5)) != 0;
            if (invulnerableTyres && !cur) Wr<uint32_t>(pc + kPhysBits, pb | (1u << 5));
            else if (!invulnerableTyres && g_prevTyres && cur && !(g_orig.physBits & (1u << 5))) Wr<uint32_t>(pc + kPhysBits, pb & ~(1u << 5));
        }
        g_prevTyres = invulnerableTyres;
    }
}

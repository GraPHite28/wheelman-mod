#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include "Offsets.h"
#include "Patches.h"
#include "Log.h"
#include "Spawner.h"

// ---------------------------------------------------------------------------
// Native re-implementations of effects originally found via a community
// Cheat Engine table: Mission Timer is a hook on the countdown tick; Ram Boom / Mana /
// No Reload are jump-hook trampolines (Ram Boom also reports the player's
// vehicle pointer for the overlay - see OnVehicleSeen).
//
// The CE table locates every one of these via aobscan (a wildcard byte
// signature), not a fixed address - the "Wheelman.exe+X" comments in it only
// describe where the table's author found them on their own exe build. This
// repack's Wheelman.exe has different code at those raw offsets, so we
// resolve addresses the same way CE does: scan the loaded module for the
// signature at runtime. Every patch still verifies the original bytes right
// before touching anything; a mismatch means "skip", never "guess".
//
// Cops and Tank Mod were dropped: Cops had no aobscan in the source table
// (address-only, and it doesn't match this build) and Tank Mod's signature
// isn't found anywhere in this exe either - neither ever activates here.
// ---------------------------------------------------------------------------

void* g_pVehicle = nullptr;
void* g_pPlayerPawn = nullptr;
PatchState g_patches;


float g_vehicleSpeed = 0.f;


bool IsLiveVehicle(const void* v, bool playerCar)
{
    if (!v) return false;
    __try
    {
        auto b = reinterpret_cast<const uint8_t*>(v);
        uint32_t vt = *reinterpret_cast<const uint32_t*>(b);
        if (vt < 0x1000000 || vt > 0x1800000 || (vt & 3) != 0) return false;
        if (*reinterpret_cast<const int32_t*>(b + 0x18) != -1) return false;
        if (playerCar && *reinterpret_cast<const int32_t*>(b + Offsets::Vehicle_ActiveFlag) != 1) return false;
        uint32_t pc = *reinterpret_cast<const uint32_t*>(b + 0x170);
        if (pc)
        {
            if (pc < 0x10000 || pc > 0x7FFF0000) return false;
            // The physics component points back at its vehicle. A vehicle that was reset by the garage routine (Repair
            // button) gets a physics component whose +0x1C points elsewhere, so a live component (vtable in the exe,
            // UObject marker) is accepted too; a freed one has neither.
            if (*reinterpret_cast<const uint32_t*>(pc + 0x1C) != reinterpret_cast<uintptr_t>(v))
            {
                const uint32_t pvt = *reinterpret_cast<const uint32_t*>(pc);
                if (pvt < 0x1000000 || pvt > 0x1800000 || *reinterpret_cast<const int32_t*>(pc + 0x18) != -1) return false;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void UpdateVehicleTelemetry()
{
    static void* lastVehicle = nullptr;
    static Vec3 lastPos;
    static DWORD lastTick = 0;

    if (!g_pVehicle) { g_vehicleSpeed = 0.f; lastVehicle = nullptr; return; }

    Vec3 pos;
    bool ok = false;
    __try
    {
        pos.x = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_pVehicle) + Offsets::Vehicle_PosX);
        pos.y = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_pVehicle) + Offsets::Vehicle_PosY);
        pos.z = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_pVehicle) + Offsets::Vehicle_PosZ);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!ok) { g_vehicleSpeed = 0.f; return; }

    DWORD now = GetTickCount();
    if (g_pVehicle == lastVehicle && lastTick != 0)
    {
        float dt = (now - lastTick) / 1000.f;
        if (dt > 0.001f)
        {
            float dx = pos.x - lastPos.x, dy = pos.y - lastPos.y, dz = pos.z - lastPos.z;
            g_vehicleSpeed = sqrtf(dx * dx + dy * dy + dz * dz) / dt;
        }
    }
    lastVehicle = g_pVehicle;
    lastPos = pos;
    lastTick = now;
}


namespace
{
    constexpr int kVehicleDiffRangeBytes = 0x3000;
    uint8_t g_vehicleDiffSnapshot[kVehicleDiffRangeBytes] = {};
    bool g_vehicleDiffHasSnapshot = false;

    // Every sub-component pointer found on the vehicle so far (see
    // DumpVehicleRawData's candidateOffsets in this file). +0x170 turned out
    // to be MwyVehiclePhysicsComponent (confirmed via its constructor/class
    // registration - see conversation), not paint, and gets reallocated on
    // repaint incidentally, probably as part of a broader reconstruction.
    // The real color data might live in one of these OTHER components
    // instead, possibly WITHOUT its pointer changing (a persisting mesh/
    // material component just having some internal field edited) - so for
    // each one we snapshot both the pointer value AND a chunk of its target,
    // to catch either kind of change.
    constexpr int kPtrDumpBytes = 0x300;
    struct PtrSnapshot
    {
        int vehicleOffset;
        const char* label;
        void* ptrAtSnapshot = nullptr;
        uint8_t content[kPtrDumpBytes] = {};
        bool valid = false;
    };
    PtrSnapshot g_ptrSnapshots[] = {
        { 0x170, "+0x170 (MwyVehiclePhysicsComponent)" },
        { 0x588, "+0x588" },
        { 0x598, "+0x598" },
        { 0x5A0, "+0x5A0" },
        { 0x5C0, "+0x5C0" },
        { 0x610, "+0x610" },
    };
    constexpr int kPtrSnapshotCount = sizeof(g_ptrSnapshots) / sizeof(g_ptrSnapshots[0]);

    void DumpPtrBlock(const char* label, void* target, int len)
    {
        if (!target) { LogF("  %s: null", label); return; }
        __try
        {
            auto base = reinterpret_cast<uint8_t*>(target);
            LogF("  %s @ 0x%p:", label, target);
            for (int off = 0; off < len; off += 0x20)
            {
                auto row = reinterpret_cast<const int32_t*>(base + off);
                LogF("    +0x%03X: %08X %08X %08X %08X %08X %08X %08X %08X",
                     off, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogF("  %s: access violation", label);
        }
    }
}

void SnapshotVehicleForDiff()
{
    g_vehicleDiffHasSnapshot = false;
    if (!g_pVehicle) { LogF("SnapshotVehicleForDiff: no vehicle"); return; }
    __try
    {
        memcpy(g_vehicleDiffSnapshot, g_pVehicle, kVehicleDiffRangeBytes);
        g_vehicleDiffHasSnapshot = true;
        LogF("SnapshotVehicleForDiff: captured 0x%X bytes from 0x%p", kVehicleDiffRangeBytes, g_pVehicle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogF("SnapshotVehicleForDiff: access violation reading from 0x%p", g_pVehicle);
        return;
    }

    for (PtrSnapshot& ps : g_ptrSnapshots)
    {
        ps.valid = false;
        ps.ptrAtSnapshot = nullptr;
        __try
        {
            ps.ptrAtSnapshot = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_pVehicle) + ps.vehicleOffset);
            if (ps.ptrAtSnapshot)
            {
                memcpy(ps.content, ps.ptrAtSnapshot, kPtrDumpBytes);
                ps.valid = true;
            }
            LogF("SnapshotVehicleForDiff: %s = 0x%p", ps.label, ps.ptrAtSnapshot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogF("SnapshotVehicleForDiff: %s dereference failed", ps.label);
        }
    }
}

void CompareVehicleDiff()
{
    if (!g_vehicleDiffHasSnapshot) { LogF("CompareVehicleDiff: no snapshot taken yet"); return; }
    if (!g_pVehicle) { LogF("CompareVehicleDiff: no vehicle"); return; }

    static uint8_t current[kVehicleDiffRangeBytes];
    __try { memcpy(current, g_pVehicle, kVehicleDiffRangeBytes); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogF("CompareVehicleDiff: access violation reading from 0x%p", g_pVehicle);
        return;
    }

    LogF("CompareVehicleDiff: differences (dword-aligned) between snapshot and now:");
    int diffCount = 0;
    for (int off = 0; off + 4 <= kVehicleDiffRangeBytes; off += 4)
    {
        auto oldVal = *reinterpret_cast<int32_t*>(g_vehicleDiffSnapshot + off);
        auto newVal = *reinterpret_cast<int32_t*>(current + off);
        if (oldVal != newVal)
        {
            LogF("  +0x%03X: %08X -> %08X", off, static_cast<uint32_t>(oldVal), static_cast<uint32_t>(newVal));
            ++diffCount;
        }
    }
    LogF("CompareVehicleDiff: %d differing dword(s)", diffCount);

    for (PtrSnapshot& ps : g_ptrSnapshots)
    {
        void* nowPtr = nullptr;
        __try { nowPtr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_pVehicle) + ps.vehicleOffset); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (nowPtr == ps.ptrAtSnapshot)
        {
            if (!ps.valid || !nowPtr) continue;
            uint8_t nowContent[kPtrDumpBytes];
            bool ok = false;
            __try { memcpy(nowContent, nowPtr, kPtrDumpBytes); ok = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!ok) continue;

            int cnt = 0;
            for (int off = 0; off + 4 <= kPtrDumpBytes; off += 4)
            {
                auto o = *reinterpret_cast<int32_t*>(ps.content + off);
                auto n = *reinterpret_cast<int32_t*>(nowContent + off);
                if (o != n)
                {
                    if (cnt == 0) LogF("  %s: SAME pointer (0x%p), content changed:", ps.label, nowPtr);
                    LogF("    +0x%03X: %08X -> %08X", off, static_cast<uint32_t>(o), static_cast<uint32_t>(n));
                    ++cnt;
                }
            }
            if (cnt == 0) LogF("  %s: unchanged (same pointer, same content)", ps.label);
        }
        else
        {
            LogF("  %s: pointer CHANGED 0x%p -> 0x%p", ps.label, ps.ptrAtSnapshot, nowPtr);
            DumpPtrBlock(ps.label, nowPtr, kPtrDumpBytes);
        }
    }

}

namespace
{
    int32_t g_scanBaseline[kScanRangeBytes / 4] = {};
    bool g_scanNarrowedOnce = false;
}
bool g_scanHasBaseline = false;
int g_scanCandidateOffsets[kMaxScanCandidates] = {};
int g_scanCandidateCount = 0;

void ScanSnapshot(void* base)
{
    g_scanHasBaseline = false;
    g_scanCandidateCount = 0;
    g_scanNarrowedOnce = false;
    if (!base) return;

    __try
    {
        memcpy(g_scanBaseline, base, kScanRangeBytes);
        g_scanHasBaseline = true;
        LogF("ScanSnapshot: captured 0x%X bytes from 0x%p", kScanRangeBytes, base);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogF("ScanSnapshot: access violation reading from 0x%p", base);
    }
}

void ScanNarrow(void* base, bool decreased)
{
    if (!base || !g_scanHasBaseline) return;

    static int32_t current[kScanRangeBytes / 4];
    __try { memcpy(current, base, kScanRangeBytes); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogF("ScanNarrow: access violation reading from 0x%p", base);
        return;
    }

    static int newOffsets[kMaxScanCandidates];
    int newCount = 0;
    int count = kScanRangeBytes / 4;

    if (!g_scanNarrowedOnce)
    {
        for (int i = 0; i < count && newCount < kMaxScanCandidates; ++i)
        {
            bool matches = decreased ? (current[i] < g_scanBaseline[i]) : (current[i] > g_scanBaseline[i]);
            if (matches)
                newOffsets[newCount++] = i * 4;
        }
        g_scanNarrowedOnce = true;
    }
    else
    {
        for (int c = 0; c < g_scanCandidateCount && newCount < kMaxScanCandidates; ++c)
        {
            int idx = g_scanCandidateOffsets[c] / 4;
            bool matches = decreased ? (current[idx] < g_scanBaseline[idx]) : (current[idx] > g_scanBaseline[idx]);
            if (matches)
                newOffsets[newCount++] = g_scanCandidateOffsets[c];
        }
    }

    memcpy(g_scanCandidateOffsets, newOffsets, sizeof(int) * newCount);
    g_scanCandidateCount = newCount;
    memcpy(g_scanBaseline, current, kScanRangeBytes); // baseline for the next round
    LogF("ScanNarrow: %s -> %d candidate(s)", decreased ? "decreased" : "increased", g_scanCandidateCount);
    for (int i = 0; i < g_scanCandidateCount && i < 20; ++i)
        LogF("  +0x%X = %d", g_scanCandidateOffsets[i], current[g_scanCandidateOffsets[i] / 4]);
}

TrackedVehicle g_vehicleList[kMaxTrackedVehicles];
int g_vehicleListCount = 0;

namespace
{
    // One-shot wide raw dump the first time a given vehicle pointer is seen,
    // so make/model/color hunting can happen offline against the log instead
    // of needing anything read out loud while driving. Capped so a long
    // drive can't blow the log up - past the cap, only the fields already
    // exposed in the ESP (pointer, Type, State, AI) are available live.
    int g_rawDumpsRemaining = 25;

    void* ReadPtr(void* base, int off)
    {
        if (!base) return nullptr;
        void* result = nullptr;
        __try { result = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return result;
    }

    // Returns target back out so callers can chase it one level further.
    void* DumpBlock(const char* label, void* target)
    {
        if (!target) return nullptr;
        __try
        {
            auto base = reinterpret_cast<uint8_t*>(target);
            LogF("  -- %s @ 0x%p --", label, target);
            for (int off = 0; off < 0x60; off += 0x20)
            {
                auto row = reinterpret_cast<const int32_t*>(base + off);
                LogF("    +0x%03X: %08X %08X %08X %08X %08X %08X %08X %08X",
                     off, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogF("  -- %s: access violation --", label);
            return nullptr;
        }
        return target;
    }

    void DumpVehicleRawData(void* ptr)
    {
        if (g_rawDumpsRemaining <= 0) return;
        --g_rawDumpsRemaining;
        __try
        {
            auto base = reinterpret_cast<uint8_t*>(ptr);
            LogF("VehicleRawDump 0x%p:", ptr);
            for (int off = 0x100; off < 0x900; off += 0x20)
            {
                auto row = reinterpret_cast<const int32_t*>(base + off);
                LogF("  +0x%03X: %08X %08X %08X %08X %08X %08X %08X %08X",
                     off, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogF("VehicleRawDump 0x%p: access violation", ptr);
        }

        // Chase the most plausible sub-component pointers (mesh/render/
        // material candidates) one level deep, hunting for color data that
        // doesn't live directly on the vehicle itself.
        auto base = reinterpret_cast<uint8_t*>(ptr);
        static const int candidateOffsets[] = { 0x588, 0x598, 0x5A0, 0x5C0, 0x610 };
        for (int off : candidateOffsets)
        {
            void* target = ReadPtr(base, off);
            if (!target) continue;
            char label[32];
            sprintf_s(label, sizeof(label), "*(+0x%X)", off);
            DumpBlock(label, target);
        }

        // One level deeper still: *(+0x5A0)+0x20 and *(+0x5C0)+0x00 both
        // landed inside the exe's own mapped range last time (~0x0133xxxx /
        // 0x0143xxxx, well inside module base 0x00400000..0x032B4000) -
        // prime candidates for a SHARED per-model archetype/material table
        // rather than more per-instance heap data.
        void* lvl1_5A0 = ReadPtr(base, 0x5A0);
        DumpBlock("*(+0x5A0)->+0x20", ReadPtr(lvl1_5A0, 0x20));

        void* lvl1_5C0 = ReadPtr(base, 0x5C0);
        DumpBlock("*(+0x5C0)->+0x00", ReadPtr(lvl1_5C0, 0x00));
    }
}

// Called (via a plain __cdecl call from the naked RamBoom detour) every time
// that code path runs, with whatever vehicle it's currently looking at.
// Confirmed by testing the original CE table: One_Taran_Boom sets every
// vehicle's health to 1 EXCEPT when [vehicle+0x560]==1 - i.e. that flag is
// how the game (and the original cheat) identifies the player's own car.
extern "C" void __cdecl OnVehicleSeen(void* ptr)
{
    __try
    {
        int32_t flag = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ptr) + Offsets::Vehicle_ActiveFlag);
        if (flag == 1)
            g_pVehicle = ptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    DWORD now = GetTickCount();
    for (int i = 0; i < g_vehicleListCount; ++i)
    {
        if (g_vehicleList[i].ptr == ptr)
        {
            g_vehicleList[i].lastSeenTick = now;
            return;
        }
    }
    DumpVehicleRawData(ptr);
    if (g_vehicleListCount < kMaxTrackedVehicles)
    {
        g_vehicleList[g_vehicleListCount].ptr = ptr;
        g_vehicleList[g_vehicleListCount].lastSeenTick = now;
        ++g_vehicleListCount;
        return;
    }
    int oldest = 0;
    for (int i = 1; i < kMaxTrackedVehicles; ++i)
        if (g_vehicleList[i].lastSeenTick < g_vehicleList[oldest].lastSeenTick)
            oldest = i;
    g_vehicleList[oldest].ptr = ptr;
    g_vehicleList[oldest].lastSeenTick = now;
}

namespace
{
    uintptr_t ModuleBase()
    {
        return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    }

    size_t ModuleSize()
    {
        auto base = reinterpret_cast<BYTE*>(ModuleBase());
        auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
        return nt->OptionalHeader.SizeOfImage;
    }

    uintptr_t FindPattern(const Signatures::Pattern& sig)
    {
        uintptr_t base = ModuleBase();
        size_t size = ModuleSize();
        size_t patLen = strlen(sig.mask);
        if (patLen == 0 || size < patLen) return 0;

        auto data = reinterpret_cast<const unsigned char*>(base);
        for (size_t i = 0; i + patLen <= size; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < patLen; ++j)
            {
                if (sig.mask[j] == 'x' && data[i + j] != sig.bytes[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return base + i;
        }
        return 0;
    }

    void LogBytes(const char* label, uintptr_t va, const unsigned char* bytes, int len)
    {
        char hex[64] = {};
        char* p = hex;
        for (int i = 0; i < len; ++i)
            p += sprintf_s(p, sizeof(hex) - (p - hex), "%02X ", bytes[i]);
        LogF("  %s @ 0x%p: %s", label, reinterpret_cast<void*>(va), hex);
    }

    void Write(uintptr_t va, const unsigned char* bytes, int len)
    {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(va), len, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(reinterpret_cast<void*>(va), bytes, len);
        VirtualProtect(reinterpret_cast<void*>(va), len, oldProtect, &oldProtect);
    }

    // ---- generic jump-hook (Ram Boom / Mana / No Reload / Mission Timer) -------------------
    struct JumpHook
    {
        uintptr_t va = 0;
        int len = 0;
        unsigned char original[16] = {};
        uintptr_t returnAddr = 0;
        bool valid = false;
    };

    bool InstallJumpHook(JumpHook& h, uintptr_t va, const unsigned char* expectedOriginal, int len,
                          void* detour, const char* name)
    {
        if (va == 0)
        {
            LogF("JumpHook %s: signature not found in module, skipping", name);
            h.valid = false;
            return false;
        }
        h.va = va;
        h.len = len;
        memcpy(h.original, expectedOriginal, len);
        h.returnAddr = h.va + len;

        if (memcmp(reinterpret_cast<void*>(h.va), h.original, len) != 0)
        {
            LogF("JumpHook %s: MISMATCH at resolved address, skipping", name);
            LogBytes("expected", h.va, h.original, len);
            LogBytes("actual  ", h.va, reinterpret_cast<unsigned char*>(h.va), len);
            h.valid = false;
            return false;
        }
        LogF("JumpHook %s: found+verified @ 0x%p", name, reinterpret_cast<void*>(h.va));

        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(h.va), len, PAGE_EXECUTE_READWRITE, &oldProtect);
        *reinterpret_cast<uint8_t*>(h.va) = 0xE9;
        *reinterpret_cast<uint32_t*>(h.va + 1) =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(detour) - (h.va + 5));
        for (int i = 5; i < len; ++i)
            *reinterpret_cast<uint8_t*>(h.va + i) = 0x90;
        VirtualProtect(reinterpret_cast<void*>(h.va), len, oldProtect, &oldProtect);

        h.valid = true;
        return true;
    }

    void RemoveJumpHook(JumpHook& h)
    {
        if (!h.valid) return;
        Write(h.va, h.original, h.len);
        h.valid = false;
    }

    JumpHook g_ramBoomHook;
    JumpHook g_manaHook;
    JumpHook g_noReloadHook;
    JumpHook g_timerHook;
    JumpHook g_arrestHook;

    uintptr_t g_returnRamBoom = 0;
    uintptr_t g_returnMana = 0;
    uintptr_t g_returnNoReload = 0;
    uintptr_t g_returnTimer = 0;
    uintptr_t g_returnPoliceTick = 0;
    JumpHook g_policeTickHook;
    JumpHook g_factoryHook;
    uintptr_t g_returnFactory = 0;
    uintptr_t g_returnArrest = 0;

    // 9999.0f as raw bits, computed by the compiler (no hand-derived hex).
    const float kInfiniteBoostValue = 9999.0f;
    const uint32_t kInfiniteBoostBits = *reinterpret_cast<const uint32_t*>(&kInfiniteBoostValue);
}

// ---------------------------------------------------------------------------
// Detours. __declspec(naked) + MSVC inline asm (x86 only). Each captures
// whatever register holds the object pointer, optionally mutates game state
// based on g_patches, re-executes the original instruction it replaced, then
// jumps back into the game's code.
// ---------------------------------------------------------------------------

__declspec(naked) void Detour_RamBoom()
{
    __asm
    {
        push esi
        call OnVehicleSeen
        add esp, 4

        cmp g_patches.ramBoom, 0
        je original
        // Was "mov [esi+2AC],1" (force health to exactly 1) - that clamps a
        // floor the vehicle can never drop below, so repeated ramming just
        // re-pins it at 1 forever instead of destroying it. Then tried
        // "sub [esi+2AC],2000" - too strong: most traffic vehicles have well
        // under 2000 max health, so every vehicle got shoved straight past 0
        // and exploded instantly on the first hit (same practical effect as
        // the original bug, just at a different clamp value). Now: only clamp
        // DOWN to 10 if health is currently above 10, and leave it alone once
        // it's at or below that - so a hit brings a healthy vehicle to a
        // visibly-damaged state without insta-killing it, but doesn't
        // re-force it back up if something else already dropped it lower.
        cmp dword ptr [esi + 0x2AC], 10
        jle original
        mov dword ptr [esi + 0x2AC], 10
original:
        cmp dword ptr [esi + 0x2AC], 0
        jmp dword ptr [g_returnRamBoom]
    }
}

extern "C" volatile long g_policeWork;
extern "C" void __cdecl Police_GamePump();
extern "C" volatile long g_policeTickHooked;
extern "C" volatile uintptr_t g_policeInstFromTick;
extern "C" volatile long g_policeTickCalls;
extern "C" void __cdecl Police_GamePumpFor(void* subsystem);

__declspec(naked) void Detour_Mana()
{
    __asm
    {
        mov g_pPlayerPawn, edi
        cmp g_policeTickHooked, 0
        jne no_police
        cmp g_policeWork, 0
        je no_police
        pushad
        pushfd
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        call Police_GamePump
        movups xmm7, [esp + 112]
        movups xmm6, [esp + 96]
        movups xmm5, [esp + 80]
        movups xmm4, [esp + 64]
        movups xmm3, [esp + 48]
        movups xmm2, [esp + 32]
        movups xmm1, [esp + 16]
        movups xmm0, [esp]
        add esp, 128
        popfd
        popad
no_police:
        cmp g_patches.infiniteMana, 0
        je original
        mov eax, kInfiniteBoostBits
        mov [edi + 0x5C], eax
original:
        movss xmm0, [edi + 0x5C]
        jmp dword ptr [g_returnMana]
    }
}

// Police subsystem per-frame update (0x5E6310, thiscall: this = ECX, RET 8). Found by watching the writer of
// m_eLostState (+0x60), which is written every frame (27k hits in a few minutes). It runs on the game thread
// whether or not the player is in a vehicle, so the wanted-level pump lives here. (0x5E72C0, hooked earlier, is
// the crime-heat handler: it only runs when a crime is reported, hence the unstable behaviour.) The first 9 bytes
// (push ebp; mov ebp,esp; and esp,-8; sub esp,0x1C) are re-executed by hand.
__declspec(naked) void Detour_PoliceTick()
{
    __asm
    {
        mov g_policeInstFromTick, ecx
        inc g_policeTickCalls
        cmp g_policeWork, 0
        je original
        pushad
        pushfd
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        push ecx
        call Police_GamePumpFor
        add esp, 4
        movups xmm7, [esp + 112]
        movups xmm6, [esp + 96]
        movups xmm5, [esp + 80]
        movups xmm4, [esp + 64]
        movups xmm3, [esp + 48]
        movups xmm2, [esp + 32]
        movups xmm1, [esp + 16]
        movups xmm0, [esp]
        add esp, 128
        popfd
        popad
original:
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        sub esp, 1Ch
        jmp dword ptr [g_returnPoliceTick]
    }
}

// Vehicle actor factory post-spawn handler (0x486B90 = vtable 0x13399B4 slot 0xCC, thiscall, ECX = factory, called
// from the post-spawn callback 0x8B4670 of every vehicle spawn). Only captures the factory instance for the spawner;
// the first 6 bytes (push ebp; mov ebp,esp; and esp,-8) are re-executed by hand.
extern "C" volatile uintptr_t g_spawnFactory;
extern "C" void __cdecl OnFactorySeen(void* factory) { Spawner::OnFactorySpawn(reinterpret_cast<uintptr_t>(factory)); }
__declspec(naked) void Detour_Factory()
{
    __asm
    {
        mov g_spawnFactory, ecx
        pushad
        pushfd
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        push ecx
        call OnFactorySeen
        add esp, 4
        movups xmm7, [esp + 112]
        movups xmm6, [esp + 96]
        movups xmm5, [esp + 80]
        movups xmm4, [esp + 64]
        movups xmm3, [esp + 48]
        movups xmm2, [esp + 32]
        movups xmm1, [esp + 16]
        movups xmm0, [esp]
        add esp, 128
        popfd
        popad
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        jmp dword ptr [g_returnFactory]
    }
}

__declspec(naked) void Detour_NoReload()
{
    __asm
    {
        cmp g_patches.noReload, 0
        je original
        cmp dword ptr [ecx + 0x2F0], 0x10
        jge original
        mov dword ptr [ecx + 0x2F0], 0x10
original:
        mov eax, [ecx + 0x2F0]
        jmp dword ptr [g_returnNoReload]
    }
}

// Countdown-timer object tracking. 0x4F0D5B sits inside the game's timer tick
// (this = EDI): [+0xEC] bit0 = running, [+0xF4] duration, [+0xFC] time
// remaining; the instruction there is `remaining -= dt`.
TimerRef g_timers[kMaxTimers];
int g_timerCount = 0;

extern "C" void __cdecl OnTimerSeen(void* obj)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < g_timerCount; ++i)
        if (g_timers[i].ptr == obj) { g_timers[i].lastSeenTick = now; return; }
    if (g_timerCount < kMaxTimers)
    {
        g_timers[g_timerCount].ptr = obj;
        g_timers[g_timerCount].lastSeenTick = now;
        ++g_timerCount;
        return;
    }
    int oldest = 0;
    for (int i = 1; i < kMaxTimers; ++i)
        if (g_timers[i].lastSeenTick < g_timers[oldest].lastSeenTick) oldest = i;
    g_timers[oldest].ptr = obj;
    g_timers[oldest].lastSeenTick = now;
}

__declspec(naked) void Detour_Timer()
{
    __asm
    {
        pushad
        pushfd
        sub esp, 16
        movups [esp], xmm0
        sub esp, 16
        movups [esp], xmm1
        push edi
        call OnTimerSeen
        add esp, 4
        movups xmm1, [esp]
        add esp, 16
        movups xmm0, [esp]
        add esp, 16
        popfd
        popad
        cmp g_patches.freezeTimer, 0
        jne skip
        subss xmm0, dword ptr [ebp + 8]
skip:
        jmp dword ptr [g_returnTimer]
    }
}

// WheelmanPawn::SetArrested(bool) - virtual at +0x610, sets the m_bArrested bit
// (0x1000 of pawn+0x648). Found by watching that word while getting arrested:
// the police component's arrest routine (0x5E5E50) calls it with 1. With
// "peaceful" on, arresting (arg != 0) is swallowed; clearing still works.
extern "C" volatile long g_policePeaceful;

__declspec(naked) void Detour_Arrest()
{
    __asm
    {
        cmp g_policePeaceful, 0
        je original
        cmp dword ptr [esp + 4], 0
        je original
        ret 4
original:
        mov eax, dword ptr [esp + 4]
        shl eax, 0xc
        jmp dword ptr [g_returnArrest]
    }
}

void InstallGamePatches()
{
    LogF("InstallGamePatches: module base=0x%p size=0x%X", reinterpret_cast<void*>(ModuleBase()), (unsigned)ModuleSize());
    {
        const unsigned char original[] = { 0xF3, 0x0F, 0x5C, 0x45, 0x08 }; // subss xmm0,[ebp+8]
        if (InstallJumpHook(g_timerHook, FindPattern(Signatures::MissionTimer), original, 5, Detour_Timer, "MissionTimer"))
            g_returnTimer = g_timerHook.returnAddr;
    }
    {
        const unsigned char original[] = { 0x8B, 0x44, 0x24, 0x04, 0xC1, 0xE0, 0x0C }; // mov eax,[esp+4]; shl eax,0xc
        if (InstallJumpHook(g_arrestHook, 0x0051F9C0, original, 7, Detour_Arrest, "SetArrested"))
            g_returnArrest = g_arrestHook.returnAddr;
    }
    {
        const unsigned char original[] = { 0x83, 0xBE, 0xAC, 0x02, 0x00, 0x00, 0x00 }; // cmp dword ptr [esi+2AC],0
        if (InstallJumpHook(g_ramBoomHook, FindPattern(Signatures::RamBoom), original, 7, Detour_RamBoom, "RamBoom"))
            g_returnRamBoom = g_ramBoomHook.returnAddr;
    }
    {
        const unsigned char original[] = { 0xF3, 0x0F, 0x10, 0x47, 0x5C }; // movss xmm0,[edi+5C]
        if (InstallJumpHook(g_manaHook, FindPattern(Signatures::Mana), original, 5, Detour_Mana, "Mana"))
            g_returnMana = g_manaHook.returnAddr;
    }
    {
        // Police subsystem tick (see Detour_PoliceTick); without it the Mana hook pumps (vehicle only).
        const unsigned char original[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x1C };
        if (InstallJumpHook(g_policeTickHook, 0x005E6310, original, 9, Detour_PoliceTick, "PoliceTick"))
        {
            g_returnPoliceTick = g_policeTickHook.returnAddr;
            g_policeTickHooked = 1;
        }
    }
    {
        const unsigned char original[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
        if (InstallJumpHook(g_factoryHook, 0x00486B90, original, 6, Detour_Factory, "VehicleFactory"))
            g_returnFactory = g_factoryHook.returnAddr;
    }
    {
        const unsigned char original[] = { 0x8B, 0x81, 0xF0, 0x02, 0x00, 0x00 }; // mov eax,[ecx+2F0]
        if (InstallJumpHook(g_noReloadHook, FindPattern(Signatures::NoReload), original, 6, Detour_NoReload, "NoReload"))
            g_returnNoReload = g_noReloadHook.returnAddr;
    }
}

void RemoveGamePatches()
{
    RemoveJumpHook(g_arrestHook);
    RemoveJumpHook(g_factoryHook);
    RemoveJumpHook(g_timerHook);
    RemoveJumpHook(g_ramBoomHook);
    g_policeTickHooked = 0;
    RemoveJumpHook(g_policeTickHook);
    RemoveJumpHook(g_manaHook);
    RemoveJumpHook(g_noReloadHook);
}

void SyncGamePatches()
{
    UpdateVehicleTelemetry();
    // RamBoom / Mana / NoReload branch on g_patches live inside their
    // trampolines, nothing to sync here.

    if (g_patches.vehicleGodMode && g_pVehicle && IsLiveVehicle(g_pVehicle, true))
    {
        // 40000 = the highest health value seen among tracked vehicles (the
        // sturdiest traffic model), so this always outranks real damage.
        __try { *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(g_pVehicle) + Offsets::Vehicle_Health) = 40000; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

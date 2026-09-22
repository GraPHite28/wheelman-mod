#include <Windows.h>
#include <cmath>
#include <cstring>
#include "CrimeTools.h"
#include "Log.h"

namespace CrimeTools
{
    bool overrideOn = false;
    Crime edited[kCrimes] = {};
    float changeScale = 1.f;
    float runAroundTime = 0, runAroundDistance = 0, runAroundResetTime = 0;
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

        constexpr uint32_t kVtable = 0x0136ED68, kSubsystemVtable = 0x0136EE28;
        constexpr int kFirst = 0x3C, kStride = 12, kRunAround = 0x174;

        const char* const kNames[kCrimes] = {
            "Ambient", "No line of sight: hiding", "No line of sight: switched car", "Police wipeout", "Police roadblock",
            "Destroy breakable", "Fire weapon", "Hit traffic car", "Melee traffic car", "Jack traffic car", "Shoot traffic car",
            "Destroy traffic car", "Shoot civilian", "Run over civilian", "Murder civilian", "Injure civilian (indirect)",
            "Hit police car", "Melee police car", "Jack police car", "Shoot police car", "Destroy police car", "Shoot police",
            "Run over police", "Murder police", "Injure police (indirect)", "Run around",
        };

        uintptr_t g_table = 0, g_targets[4] = {};
        int g_nTargets = 0;
        Crime g_orig[kCrimes] = {};
        bool g_have = false, g_prevOn = false;
        int g_order = 0;   // 0 = { int max, float change, bool serious }, 1 = { bool, int, float }
        float g_origRun[3] = {};

        uintptr_t At(int crime) { return g_table + (crime == 25 ? kRunAround : kFirst + crime * kStride); }

        bool PlausibleInt(int32_t v) { return v >= 0 && v <= 10; }
        bool PlausibleFloat(float f) { return std::isfinite(f) && std::fabs(f) < 20.f; }

        bool ReadRaw(uintptr_t at, int order, Crime& c)
        {
            uint32_t w[3];
            if (!Rd(at, w[0]) || !Rd(at + 4, w[1]) || !Rd(at + 8, w[2])) return false;
            int32_t maxLevel; float change; uint32_t ser;
            if (order == 0) { maxLevel = static_cast<int32_t>(w[0]); memcpy(&change, &w[1], 4); ser = w[2]; }
            else { ser = w[0]; maxLevel = static_cast<int32_t>(w[1]); memcpy(&change, &w[2], 4); }
            if (!PlausibleInt(maxLevel) || !PlausibleFloat(change) || ser > 1) return false;
            c.maxLevel = maxLevel; c.change = change; c.serious = ser != 0;
            return true;
        }

        bool WriteRaw(uintptr_t at, int order, const Crime& c)
        {
            uint32_t fl; memcpy(&fl, &c.change, 4);
            const uint32_t mx = static_cast<uint32_t>(c.maxLevel), ser = c.serious ? 1u : 0u;
            if (order == 0) return Wr<uint32_t>(at, mx) && Wr<uint32_t>(at + 4, fl) && Wr<uint32_t>(at + 8, ser);
            return Wr<uint32_t>(at, ser) && Wr<uint32_t>(at + 4, mx) && Wr<uint32_t>(at + 8, fl);
        }

        uintptr_t Slot(uintptr_t table, int crime) { return table + (crime == 25 ? kRunAround : kFirst + crime * kStride); }
        void WriteAll(int i, const Crime& c) { for (int t = 0; t < g_nTargets; ++t) WriteRaw(Slot(g_targets[t], i), g_order, c); }
        void WriteRunAll(float a, float b, float c)
        {
            for (int t = 0; t < g_nTargets; ++t) { Wr<float>(g_targets[t] + 0x168, a); Wr<float>(g_targets[t] + 0x16C, b); Wr<float>(g_targets[t] + 0x170, c); }
        }

        bool StillLive()
        {
            uint32_t vt = 0; int32_t mark = 0;
            return g_table && Rd(g_table, vt) && vt == kVtable && Rd(g_table + 0x18, mark) && mark == -1;
        }
    }

    const char* Name(int c) { return c >= 0 && c < kCrimes ? kNames[c] : "?"; }
    const Crime& Original(int c) { return g_orig[c < 0 || c >= kCrimes ? 0 : c]; }
    bool Loaded() { return g_have && StillLive(); }

    bool Load()
    {
        g_table = 0; g_have = false;
        // The heap holds several objects of the crime-table class: a default/class object with only negative decay values,
        // a partial copy, and the live table that the police response subsystem (vtable 0x136EE28) points to. Only the
        // referenced one is used by the game, so it is picked through that reference.
        uintptr_t tables[16], subs[8];
        int nTables = 0, nSubs = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
            {
                __try
                {
                    for (uintptr_t p = start; p + 0x200 <= end; p += 4)
                    {
                        const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                        if ((vt != kVtable && vt != kSubsystemVtable) || *reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                        if (vt == kVtable) { if (nTables < 16) tables[nTables++] = p; }
                        else if (nSubs < 8) subs[nSubs++] = p;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = end;
        }
        if (!nTables) { message = "crime table not found (load into the world)"; return false; }
        g_nTargets = 0;
        for (int t = 0; t < nTables; ++t)
            for (int s = 0; s < nSubs; ++s)
                for (int off = 0x30; off < 0x100; off += 4)
                {
                    uint32_t v = 0;
                    if (Rd(subs[s] + off, v) && v == tables[t])
                    {
                        bool dup = false;
                        for (int k = 0; k < g_nTargets; ++k) dup |= g_targets[k] == tables[t];
                        if (!dup && g_nTargets < 4) g_targets[g_nTargets++] = tables[t];
                    }
                }
        // fall back to the last object found (the default one is always the lowest / first)
        if (!g_nTargets) g_targets[g_nTargets++] = tables[nTables - 1];
        g_table = g_targets[0];
        LogF("CrimeTools: %d table object(s), %d subsystem(s), %d target(s), primary %08X", nTables, nSubs, g_nTargets, static_cast<unsigned>(g_table));

        // decide the member order from the first structs: each must look valid in exactly the chosen order
        for (int order = 0; order < 2; ++order)
        {
            int ok = 0;
            for (int i = 0; i < kCrimes; ++i) { Crime c; if (ReadRaw(At(i), order, c)) ++ok; }
            if (ok == kCrimes) { g_order = order; g_have = true; break; }
        }
        if (!g_have) { message = "crime table layout does not match (see the raw values in Debug)"; LogF("CrimeTools: table @ %08X layout mismatch", static_cast<unsigned>(g_table)); return false; }
        // values that came from a saved config are kept; empty ones (all zero) start from the game's own numbers
        for (int i = 0; i < kCrimes; ++i)
        {
            ReadRaw(At(i), g_order, g_orig[i]);
            if (edited[i].maxLevel == 0 && edited[i].change == 0.f && !edited[i].serious) edited[i] = g_orig[i];
        }
        Rd(g_table + 0x168, g_origRun[0]); Rd(g_table + 0x16C, g_origRun[1]); Rd(g_table + 0x170, g_origRun[2]);
        if (runAroundTime == 0.f) runAroundTime = g_origRun[0];
        if (runAroundDistance == 0.f) runAroundDistance = g_origRun[1];
        if (runAroundResetTime == 0.f) runAroundResetTime = g_origRun[2];
        message = "crime table loaded";
        LogF("CrimeTools: table @ %08X, member order %d", static_cast<unsigned>(g_table), g_order);
        return true;
    }

    bool ReadLive(int c, Crime& out) { return g_have && StillLive() && c >= 0 && c < kCrimes && ReadRaw(At(c), g_order, out); }

    void ResetToOriginal()
    {
        if (!Loaded()) return;
        for (int i = 0; i < kCrimes; ++i) { edited[i] = g_orig[i]; WriteAll(i, g_orig[i]); }
        runAroundTime = g_origRun[0]; runAroundDistance = g_origRun[1]; runAroundResetTime = g_origRun[2];
        WriteRunAll(g_origRun[0], g_origRun[1], g_origRun[2]);
        changeScale = 1.f;
        message = "original values restored";
    }

    int TimeToLoseCount() { int32_t n = 0; return Loaded() && Rd(g_table + 0x34, n) && n > 0 && n < 32 ? n : 0; }

    float TimeToLose(int level)
    {
        uint32_t data = 0; float f = 0;
        if (!Loaded() || level < 0 || level >= TimeToLoseCount() || !Rd(g_table + 0x30, data) || data < 0x10000) return 0;
        Rd(data + level * 4, f);
        return f;
    }

    void SetTimeToLose(int level, float seconds)
    {
        uint32_t data = 0;
        if (!Loaded() || level < 0 || level >= TimeToLoseCount() || !Rd(g_table + 0x30, data) || data < 0x10000) return;
        Wr<float>(data + level * 4, seconds);
    }

    void Tick()
    {
        if (!g_have || !StillLive()) { g_have = false; return; }
        if (overrideOn)
        {
            for (int i = 0; i < kCrimes; ++i)
            {
                Crime w = edited[i];
                if (changeScale != 1.f && w.change > 0.f) w.change *= changeScale;
                Crime cur;
                if (ReadRaw(At(i), g_order, cur) && cur.serious == w.serious && cur.maxLevel == w.maxLevel && cur.change == w.change) continue;
                WriteAll(i, w);
            }
            WriteRunAll(runAroundTime, runAroundDistance, runAroundResetTime);
        }
        else if (g_prevOn)
        {
            for (int i = 0; i < kCrimes; ++i) WriteAll(i, g_orig[i]);
            WriteRunAll(g_origRun[0], g_origRun[1], g_origRun[2]);
        }
        g_prevOn = overrideOn;
    }
}


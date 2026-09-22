#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include "Weapons.h"
#include "Overlay.h"
#include "CrashHandler.h"
#include "Log.h"

namespace Weapons
{
    const char* lastMessage = "";

    namespace
    {
        constexpr uint32_t kClassVtable = 0x013A99F8;    // UClass
        constexpr uint32_t kManagerVtable = 0x0135AFA0;  // InventoryManager of the player
        constexpr uintptr_t kCreateInventory = 0x009CA1A0;
        constexpr int kMaxEntries = 32;

        Entry g_entries[kMaxEntries];
        int g_count = 0;
        volatile long g_pending = 0;
        uintptr_t g_pendingClass = 0;
        ULONGLONG g_lastGive = 0;

        template <class T> bool Rd(uintptr_t a, T& out)
        {
            __try { out = *reinterpret_cast<const T*>(a); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool ObjectOk(uintptr_t o)
        {
            uint32_t vt = 0; int32_t mark = 0;
            return o > 0x10000 && o < 0x7FFF0000 && Rd(o, vt) && vt >= 0x1000000 && vt <= 0x1800000 && Rd(o + 0x18, mark) && mark == -1;
        }

        struct Known { uint32_t nameIdx; const char* name; bool unsafe = false; };
        const Known kKnown[] = {
            { 0x18B7, "M79 grenade launcher" }, { 0x18B9, "M79 Gallo" }, { 0x7895, "M79 smoke" },
            { 0x18BB, "Minimi machine gun" }, { 0x18BD, "Beretta" }, { 0x18BF, "Desert Eagle" },
            { 0x18C1, "AK-47" }, { 0x18C3, "M4" }, { 0x18C5, "Spas-12 shotgun" }, { 0x18C7, "MP5" }, { 0x18C9, "UZI" },
            { 0x7894, "Fellipe's golden gun", true }, { 0x7896, "Test BFG", true }, { 0x7897, "Test BFG100", true }, { 0x7898, "Test BFG5", true },
        };

        // Weapon of the player: pawn+0x37C; its class +0x28.
        bool HeldWeaponClass(uintptr_t& cls)
        {
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            uint32_t w = 0, c = 0;
            if (!pawn || !Rd(pawn + 0x37C, w) || !ObjectOk(w) || !Rd(w + 0x28, c) || !ObjectOk(c)) return false;
            cls = c;
            return true;
        }

        bool Manager(uintptr_t& mgr)
        {
            const uintptr_t pawn = Overlay::GetPlayerPawn();
            uint32_t m = 0, vt = 0, owner = 0;
            if (!pawn || !Rd(pawn + 0x378, m) || !ObjectOk(m) || !Rd(m, vt) || vt != kManagerVtable || !Rd(m + 0x90, owner) || owner != pawn) return false;
            mgr = m;
            return true;
        }

        struct C { uintptr_t p; uint32_t super; };

        // Kept in its own function: __try can't share a function with objects that need unwinding.
        void ScanRegion(uintptr_t start, uintptr_t end, std::vector<C>& out)
        {
            __try
            {
                for (uintptr_t p = start; p + 0xD0 <= end; p += 4)
                {
                    if (*reinterpret_cast<const uint32_t*>(p) != kClassVtable) continue;
                    if (*reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                    out.push_back({ p, *reinterpret_cast<const uint32_t*>(p + 0x30) });
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // CreateInventory(class, bDoNotActivate): thiscall, callee pops 8.
        uintptr_t CallCreate(uintptr_t fn, uintptr_t mgr, uintptr_t cls)
        {
            uintptr_t result = 0;
            __asm
            {
                mov eax, fn
                mov ecx, mgr
                mov edx, cls
                push 0        // bDoNotActivate = false: the new weapon becomes the current one
                push edx      // class
                call eax
                mov result, eax
            }
            return result;
        }
    }

    int Count() { return g_count; }
    bool At(int i, Entry& out)
    {
        if (i < 0 || i >= g_count) return false;
        out = g_entries[i];
        return true;
    }
    bool HasPending() { return g_pending != 0; }

    int Scan()
    {
        g_count = 0;
        uintptr_t held = 0;
        if (!HeldWeaponClass(held)) { lastMessage = "hold a weapon first (its class is used to find the weapon base class)"; return 0; }

        // Every UClass in memory with its superclass (UStruct::SuperStruct is at +0x30).
        std::vector<C> classes;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
                ScanRegion(start, end, classes);
            addr = end;
        }

        std::unordered_map<uintptr_t, uint32_t> superMap;
        superMap.reserve(classes.size() * 2);
        for (const C& k : classes) superMap[k.p] = k.super;
        auto superOf = [&](uintptr_t c) -> uintptr_t
        {
            auto it = superMap.find(c);
            return it == superMap.end() ? 0 : it->second;
        };
        auto derives = [&](uintptr_t c, uintptr_t anc)
        {
            for (int n = 0; n < 24 && c; ++n) { if (c == anc) return true; c = superOf(c); }
            return false;
        };

        // The weapon base is the closest ancestor of the held weapon's class that has many loaded subclasses.
        uintptr_t base = 0;
        uintptr_t anc = superOf(held);
        for (int depth = 0; depth < 6 && anc; ++depth, anc = superOf(anc))
        {
            int subs = 0;
            for (const C& k : classes) if (k.p != anc && derives(k.p, anc)) ++subs;
            if (subs >= 8) { base = anc; break; }
        }
        if (!base) { lastMessage = "weapon base class not found"; return 0; }

        std::vector<uintptr_t> found;
        for (const C& k : classes) if (k.p != base && derives(k.p, base)) found.push_back(k.p);
        std::sort(found.begin(), found.end(), [](uintptr_t a, uintptr_t b)
        {
            uint32_t na = 0, nb = 0; Rd(a + 0x20, na); Rd(b + 0x20, nb);
            return na < nb;
        });

        for (uintptr_t c : found)
        {
            if (g_count >= kMaxEntries) break;
            Entry& e = g_entries[g_count];
            e.cls = c; e.nameIdx = 0; e.clip = -1;
            Rd(c + 0x20, e.nameIdx);
            uint32_t cdo = 0;
            if (Rd(c + 0xCC, cdo) && cdo > 0x10000) { int32_t clip = 0; if (Rd(cdo + 0x2F8, clip)) e.clip = clip; }
            const char* name = nullptr;
            e.unsafe = false;
            for (const Known& k : kKnown) if (k.nameIdx == e.nameIdx) { name = k.name; e.unsafe = k.unsafe; }
            if (!name) e.unsafe = true;   // an unknown class is not vetted either
            if (name) snprintf(e.label, sizeof(e.label), "%s  (clip %d)", name, e.clip);
            else snprintf(e.label, sizeof(e.label), "class 0x%X  (clip %d)", e.nameIdx, e.clip);
            ++g_count;
        }
        char msg[64];
        static char keep[64];
        snprintf(msg, sizeof(msg), "found %d weapon classes", g_count);
        strcpy_s(keep, msg);
        lastMessage = keep;
        LogF("Weapons: base 0x%p, %d classes", reinterpret_cast<void*>(base), g_count);
        return g_count;
    }

    bool Give(int index)
    {
        if (index < 0 || index >= g_count) { lastMessage = "scan first"; return false; }
        if (g_pending) { lastMessage = "already queued"; return false; }
        if (g_entries[index].unsafe && !Overlay::unsafeEnabled) { lastMessage = "unfinished weapon: enable Unsafe in Settings (firing it can crash the game)"; return false; }
        g_pendingClass = g_entries[index].cls;
        lastMessage = "queued";
        InterlockedExchange(&g_pending, 1);
        return true;
    }

    void RunPending()
    {
        if (!g_pending) return;
        InterlockedExchange(&g_pending, 0);
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastGive < 700) { lastMessage = "too soon after the previous weapon"; return; }
        g_lastGive = now;

        uintptr_t mgr = 0;
        if (!Manager(mgr)) { lastMessage = "inventory manager not found (pawn+0x378)"; return; }
        uint32_t vt = 0, fn = 0;
        if (!Rd(mgr, vt) || !Rd(vt + 0x2F8, fn) || fn != kCreateInventory) { lastMessage = "unexpected CreateInventory slot"; return; }
        if (!ObjectOk(g_pendingClass)) { lastMessage = "class object is gone"; return; }

        char note[64];
        sprintf_s(note, "give weapon class=%08X", static_cast<unsigned>(g_pendingClass));
        CrashHandler::Note(note);
        uintptr_t item = 0;
        bool threw = false;
        __try { item = CallCreate(fn, mgr, g_pendingClass); }
        __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
        lastMessage = threw ? "CreateInventory raised an exception" : (item ? "weapon added" : "CreateInventory returned null");
    }
}

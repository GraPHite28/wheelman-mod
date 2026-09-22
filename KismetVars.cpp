#include <Windows.h>
#include <cstring>
#include <vector>
#include "KismetVars.h"
#include "Log.h"

namespace KismetVars
{
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

        constexpr uint32_t kVt[3] = { 0x013EE4F0, 0x013EE3D8, 0x013EE1A8 };   // SeqVar_Int, SeqVar_Float, SeqVar_Bool
        constexpr int kValueOffset = 0x84;
        constexpr int kMax = 1200;

        struct Entry { uintptr_t obj; Kind kind; uint32_t snapshot; bool locked; uint32_t lockValue; };
        std::vector<Entry> g_vars;
        Entry g_tmp[kMax];

        // plain function: __try cannot live in a function that owns C++ objects
        void ScanRegion(uintptr_t start, uintptr_t end, int& n)
        {
            __try
            {
                for (uintptr_t p = start; p + 0x100 <= end && n < kMax; p += 4)
                {
                    const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                    if (vt != kVt[0] && vt != kVt[1] && vt != kVt[2]) continue;
                    if (*reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                    Entry e{}; e.obj = p; e.kind = vt == kVt[0] ? KInt : (vt == kVt[1] ? KFloat : KBool);
                    g_tmp[n++] = e;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        bool Live(const Entry& e)
        {
            uint32_t vt = 0; int32_t mark = 0;
            return Rd(e.obj, vt) && vt == kVt[e.kind] && Rd(e.obj + 0x18, mark) && mark == -1;
        }
    }

    int Scan()
    {
        int n = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr && n < kMax)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100) ScanRegion(start, end, n);
            addr = end;
        }
        std::vector<Entry> found(g_tmp, g_tmp + n);        // keep locks and snapshots of variables that are still there
        for (Entry& n : found)
        {
            Rd(n.obj + kValueOffset, n.snapshot);
            for (const Entry& o : g_vars) if (o.obj == n.obj) { n.snapshot = o.snapshot; n.locked = o.locked; n.lockValue = o.lockValue; break; }
        }
        g_vars.swap(found);
        LogF("KismetVars: %d variables", static_cast<int>(g_vars.size()));
        message = "scan done";
        return static_cast<int>(g_vars.size());
    }

    int Count() { return static_cast<int>(g_vars.size()); }

    bool At(int i, Var& out)
    {
        if (i < 0 || i >= static_cast<int>(g_vars.size()) || !Live(g_vars[i])) return false;
        const Entry& e = g_vars[i];
        uint32_t v = 0;
        if (!Rd(e.obj + kValueOffset, v)) return false;
        out = { e.obj, e.kind, v, e.snapshot, v != e.snapshot, e.locked, e.lockValue };
        return true;
    }

    void TakeSnapshot()
    {
        for (Entry& e : g_vars) { uint32_t v = 0; if (Live(e) && Rd(e.obj + kValueOffset, v)) e.snapshot = v; }
        message = "snapshot taken";
    }

    void SetLock(int i, bool on)
    {
        if (i < 0 || i >= static_cast<int>(g_vars.size())) return;
        Entry& e = g_vars[i];
        uint32_t v = 0;
        if (on && Live(e) && Rd(e.obj + kValueOffset, v)) { e.lockValue = v; e.locked = true; }
        else e.locked = false;
    }

    void SetValue(int i, uint32_t raw)
    {
        if (i < 0 || i >= static_cast<int>(g_vars.size())) return;
        Entry& e = g_vars[i];
        if (Live(e)) { Wr<uint32_t>(e.obj + kValueOffset, raw); if (e.locked) e.lockValue = raw; }
    }

    void UnlockAll() { for (Entry& e : g_vars) e.locked = false; }
    int LockedCount() { int n = 0; for (const Entry& e : g_vars) if (e.locked) ++n; return n; }

    void Tick()
    {
        for (Entry& e : g_vars)
        {
            if (!e.locked) continue;
            if (!Live(e)) { e.locked = false; continue; }
            uint32_t v = 0;
            if (Rd(e.obj + kValueOffset, v) && v != e.lockValue) Wr<uint32_t>(e.obj + kValueOffset, e.lockValue);
        }
    }
}

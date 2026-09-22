#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include "LoadGuard.h"
#include "MapTools.h"
#include "Log.h"

namespace MapTools
{
    bool showGaragesOnMap = false;
    bool showAllOnMap = false;
    float hudZoom = 1.f;
    bool cachesAlwaysOpen = false;
    bool showCachesOnMap = false;
    bool markerWritesAllowed = false;
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

        struct ClassEntry { uint32_t vtable; const char* name; };
        const ClassEntry kClasses[] = {
#include "MapClasses.inc"
        };
        constexpr int kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);

        const char* const kIcons[] = {
#include "MapIcons.inc"
        };
        constexpr int kIconCount = sizeof(kIcons) / sizeof(kIcons[0]) - 1;   // the last name is the enum's MAX entry

        int ClassOf(uint32_t vt)
        {
            for (int i = 0; i < kClassCount; ++i) if (kClasses[i].vtable == vt) return i;
            return -1;
        }
        bool IsUnlockable(int cls)
        {
            const char* n = kClasses[cls].name;
            return strcmp(n, "WheelmanUnlockable") == 0 || strcmp(n, "WheelmanGarage") == 0 ||
                   strcmp(n, "WheelmanAmmoCache") == 0 || strcmp(n, "WheelmanWeaponCache") == 0;
        }
        bool IsGarage(int cls) { return strcmp(kClasses[cls].name, "WheelmanGarage") == 0; }
        bool IsCache(int cls) { return strstr(kClasses[cls].name, "Cache") != nullptr; }

        constexpr int kMax = 600;
        uintptr_t g_objs[kMax];
        int g_cls[kMax];
        int g_count = 0;

        bool Live(int i)
        {
            uint32_t vt = 0; int32_t mark = 0;
            return i >= 0 && i < g_count && Rd(g_objs[i], vt) && Rd(g_objs[i] + 0x18, mark) && mark == -1 && ClassOf(vt) == g_cls[i];
        }

        // ---- HUD screens ----
        struct HudClass { uint32_t vtable; const char* name; };
        const HudClass kHudClasses[] = {
            { 0x0131E2A8, "HUDBase" }, { 0x0131E5B0, "HUDBOP" }, { 0x0131E8B8, "HUDContracts" }, { 0x0131EBC0, "HUDEventBase" },
            { 0x0131EEC8, "HUDFailure" }, { 0x0131F1D0, "HUDEventFailure" }, { 0x0131F4D8, "HUDHotPotato" },
            { 0x0131F7E0, "HUDMadeToOrder" }, { 0x0131FAE8, "HUDMission" }, { 0x0131FDF0, "HUDRace" }, { 0x013200F8, "HUDRampage" },
        };
        constexpr int kHudClassCount = sizeof(kHudClasses) / sizeof(kHudClasses[0]);
        constexpr int kMaxHud = 16;
        uintptr_t g_hud[kMaxHud];
        int g_hudCls[kMaxHud];
        int g_hudCount = 0;
        ULONGLONG g_lastKeep = 0;
    }

    int ClassCount() { return kClassCount; }
    const char* ClassName(int c) { return c >= 0 && c < kClassCount ? kClasses[c].name : "?"; }
    const char* IconName(int icon) { return icon >= 0 && icon < kIconCount ? kIcons[icon] : "?"; }
    int IconCount() { return kIconCount; }
    int Count() { return g_count; }

    // The heap walk takes ~150 ms, so it fills private buffers and publishes the result at the end (the list never goes empty
    // or half-filled), and the periodic callers run it on a background thread (ScanAsync) so the game never stalls.
    int Scan()
    {
        const ULONGLONG t0 = GetTickCount64();
        static uintptr_t objs[kMax]; static int cls[kMax]; int count = 0;
        static volatile long running = 0;
        if (InterlockedCompareExchange(&running, 1, 0) != 0) return g_count;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr && count < kMax)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
            {
                __try
                {
                    for (uintptr_t p = start; p + 0x300 <= end && count < kMax; p += 4)
                    {
                        const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                        if (vt < 0x01320000 || vt > 0x01380000) continue;
                        const int c = ClassOf(vt);
                        if (c < 0 || *reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                        objs[count] = p; cls[count++] = c;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = end;
        }
        memcpy(g_objs, objs, sizeof(uintptr_t) * count); memcpy(g_cls, cls, sizeof(int) * count);
        g_count = count;
        LogF("MapTools: found %d markers in %d ms", count, static_cast<int>(GetTickCount64() - t0));
        message = "scan done";
        InterlockedExchange(&running, 0);
        return count;
    }

    static DWORD WINAPI ScanThread(LPVOID) { Scan(); return 0; }
    void ScanAsync()
    {
        if (LoadGuard::Quiet()) return;
        static volatile long started = 0;
        if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
        HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
        if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
        // the thread ends after one scan; allow the next request once it did (checked lazily by the running flag inside Scan)
        InterlockedExchange(&started, 0);
    }

    bool At(int i, Row& out)
    {
        if (!Live(i)) return false;
        const uintptr_t m = g_objs[i];
        out = {};
        out.obj = m; out.cls = g_cls[i]; out.className = kClasses[g_cls[i]].name;
        Rd(m + 0xD4, out.x); Rd(m + 0xD8, out.y); Rd(m + 0xDC, out.z);
        uint8_t ic = 0, uic = 0;
        Rd(m + 0x1CC, ic); Rd(m + 0x1CD, uic);
        out.icon = ic; out.uiIcon = uic;
        Rd(m + 0x1C4, out.rawA); Rd(m + 0x1D4, out.rawB); Rd(m + 0x1D8, out.sizeInWorld);
        out.onMap = (out.rawA & 1) != 0; out.inGame = (out.rawA & 2) != 0; out.disabled = (out.rawB & 4) != 0;
        if (IsUnlockable(out.cls)) { out.hasState = true; int32_t st = 0; Rd(m + 0x1E4, st); out.state = st; }
        return true;
    }

    // Marks the marker as shown on the map: DisplayOnMap on, DisabledOnMap off, and the "last" copies cleared so the
    // map code notices the change; a marker without an icon gets the plain garage icon when it is a garage.
    void MakeVisible(uintptr_t m)
    {
        uint32_t a = 0, b = 0;
        if (!Rd(m + 0x1C4, a) || !Rd(m + 0x1D4, b)) return;
        if (!(a & 1)) Wr<uint32_t>(m + 0x1C4, a | 1);
        const uint32_t nb = b & ~((1u << 2) | (1u << 3) | (1u << 0));
        if (nb != b) Wr<uint32_t>(m + 0x1D4, nb);
    }

    int ShowAllNow(bool onlyGarages)
    {
        int n = 0;
        if (!markerWritesAllowed) { message = "needs the Unsafe switch (it changes the game's own map)"; return 0; }
        for (int i = 0; i < g_count; ++i)
        {
            if (!Live(i)) continue;
            if (onlyGarages && !IsGarage(g_cls[i])) continue;
            uint8_t icon = 0;
            Rd(g_objs[i] + 0x1CC, icon);
            if (icon == 0 && IsGarage(g_cls[i])) Wr<uint8_t>(g_objs[i] + 0x1CC, 69);   // GARAGE_MARKER
            else if (icon == 0) continue;
            MakeVisible(g_objs[i]);
            ++n;
        }
        message = n ? "markers set to be shown on the map" : "no markers changed (scan first)";
        return n;
    }

    // A HUD screen that is really running: the archetypes (default objects) have the mask size fields (+0x380 / +0x384) empty.
    bool HudLive(uintptr_t h)
    {
        float w = 0, hh = 0;
        return Rd(h + 0x380, w) && Rd(h + 0x384, hh) && w > 1.f && hh > 1.f;
    }

    // Minimap zoom multiplier: the game rewrites m_fMapScale (+0x36C) itself, so the multiplier is applied to whatever
    // value the game left there; the value written last is remembered to tell the game's update from ours.
    static void TickHudZoom()
    {
        static float lastWritten = 0.f, base = 0.f;
        static uintptr_t lastHud = 0;
        static bool wasOn = false;
        const bool on = hudZoom > 1.001f || hudZoom < 0.999f;
        if (!on && !wasOn) return;
        for (int i = 0; i < g_hudCount; ++i)
        {
            Hud h{};
            if (!HudAt(i, h) || !HudLive(h.obj)) continue;
            const float cur = GetFloat(h.obj, 0x36C);
            if (h.obj != lastHud || std::fabs(cur - lastWritten) > 1e-7f) { base = cur; lastHud = h.obj; }
            if (!on) { SetFloat(h.obj, 0x36C, base); lastWritten = 0.f; wasOn = false; return; }
            float w = base * hudZoom;
            if (w < 0.002f) w = 0.002f;
            if (w > 0.5f) w = 0.5f;
            SetFloat(h.obj, 0x36C, w);
            lastWritten = w; wasOn = true;
            return;
        }
        wasOn = false;
    }

    // Weapon / ammo caches are WheelmanUnlockable objects like the garages: m_eState (+0x1E4) 2 = available, anything else =
    // used up / cooling down (m_fMinTimeBetweenUses), m_fTimeUntilAvailable at +0x1E8. They have no map icon by default.
    int UnlockCaches()
    {
        int n = 0;
        for (int i = 0; i < g_count; ++i)
        {
            if (!Live(i) || !IsCache(g_cls[i])) continue;
            float x = 0, y = 0; Rd(g_objs[i] + 0xD4, x); Rd(g_objs[i] + 0xD8, y);
            if (x == 0.f && y == 0.f) continue;   // archetype
            int32_t st = 0;
            if (Rd(g_objs[i] + 0x1E4, st) && st != 2) { Wr<int32_t>(g_objs[i] + 0x1E4, 2); Wr<float>(g_objs[i] + 0x1E8, 0.f); ++n; }
        }
        message = n ? "weapon caches set to available" : "nothing to unlock (scan first, or all caches are available)";
        return n;
    }

    static void TickCaches()
    {
        if (!cachesAlwaysOpen && !showCachesOnMap) return;
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last < 500) return;
        last = now;
        for (int i = 0; i < g_count; ++i)
        {
            if (!Live(i) || !IsCache(g_cls[i])) continue;
            float x = 0, y = 0; Rd(g_objs[i] + 0xD4, x); Rd(g_objs[i] + 0xD8, y);
            if (x == 0.f && y == 0.f) continue;
            if (cachesAlwaysOpen)
            {
                int32_t st = 0;
                if (Rd(g_objs[i] + 0x1E4, st) && st != 2) { Wr<int32_t>(g_objs[i] + 0x1E4, 2); Wr<float>(g_objs[i] + 0x1E8, 0.f); }
            }
            if (showCachesOnMap && markerWritesAllowed)
            {
                uint8_t icon = 0; Rd(g_objs[i] + 0x1CC, icon);
                if (icon == 0) Wr<uint8_t>(g_objs[i] + 0x1CC, 47);   // AMMO_STASH_MARKER
                MakeVisible(g_objs[i]);
            }
        }
    }

    void Tick()
    {
        TickHudZoom();
        TickCaches();
        if (!markerWritesAllowed || (!showGaragesOnMap && !showAllOnMap)) return;
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastKeep < 1000) return;
        g_lastKeep = now;
        for (int i = 0; i < g_count; ++i)
        {
            if (!Live(i)) continue;
            if (showAllOnMap || IsGarage(g_cls[i]))
            {
                uint8_t icon = 0; Rd(g_objs[i] + 0x1CC, icon);
                if (icon == 0 && !IsGarage(g_cls[i])) continue;
                if (icon == 0) Wr<uint8_t>(g_objs[i] + 0x1CC, 69);
                MakeVisible(g_objs[i]);
            }
        }
    }

    // ---- HUD ------------------------------------------------------------------------------------------------------
    int ScanHud()
    {
        static uintptr_t hud[kMaxHud]; static int hudCls[kMaxHud]; int hudCount = 0;
        static volatile long running = 0;
        if (InterlockedCompareExchange(&running, 1, 0) != 0) return g_hudCount;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr && hudCount < kMaxHud)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
            {
                __try
                {
                    for (uintptr_t p = start; p + 0x4A0 <= end && hudCount < kMaxHud; p += 4)
                    {
                        const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                        if (vt < 0x01310000 || vt > 0x01330000) continue;
                        int c = -1;
                        for (int k = 0; k < kHudClassCount; ++k) if (kHudClasses[k].vtable == vt) { c = k; break; }
                        if (c < 0 || *reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                        hud[hudCount] = p; hudCls[hudCount++] = c;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = end;
        }
        memcpy(g_hud, hud, sizeof(uintptr_t) * hudCount); memcpy(g_hudCls, hudCls, sizeof(int) * hudCount);
        g_hudCount = hudCount;
        LogF("MapTools: found %d HUD screens", hudCount);
        InterlockedExchange(&running, 0);
        return hudCount;
    }

    static DWORD WINAPI ScanHudThread(LPVOID) { ScanHud(); return 0; }
    void ScanHudAsync()
    {
        HANDLE h = CreateThread(nullptr, 0, ScanHudThread, nullptr, 0, nullptr);
        if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
    }

    int HudCount() { return g_hudCount; }

    bool HudAt(int i, Hud& out)
    {
        if (i < 0 || i >= g_hudCount) return false;
        uint32_t vt = 0; int32_t mark = 0;
        if (!Rd(g_hud[i], vt) || !Rd(g_hud[i] + 0x18, mark) || mark != -1 || vt != kHudClasses[g_hudCls[i]].vtable) return false;
        out = { g_hud[i], kHudClasses[g_hudCls[i]].name };
        return true;
    }

    bool GetBit(uintptr_t h, int off, int bit) { uint32_t v = 0; return Rd(h + off, v) && ((v >> bit) & 1) != 0; }
    void SetBit(uintptr_t h, int off, int bit, bool on)
    {
        uint32_t v = 0;
        if (!Rd(h + off, v)) return;
        Wr<uint32_t>(h + off, on ? (v | (1u << bit)) : (v & ~(1u << bit)));
    }
    float GetFloat(uintptr_t h, int off) { float v = 0; Rd(h + off, v); return v; }
    void SetFloat(uintptr_t h, int off, float v) { Wr<float>(h + off, v); }
    int GetInt(uintptr_t h, int off) { int32_t v = 0; Rd(h + off, v); return v; }
    void SetInt(uintptr_t h, int off, int v) { Wr<int32_t>(h + off, v); }
}

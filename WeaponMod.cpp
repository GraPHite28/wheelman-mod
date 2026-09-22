#include <Windows.h>
#include <cmath>
#include "WeaponMod.h"
#include "Overlay.h"

namespace WeaponMod
{
    bool noSpread = false;
    bool noRecoil = false;
    uintptr_t currentWeapon = 0;

    namespace
    {
        constexpr int kSpreadOffs[] = { 0x364, 0x368, 0x36C, 0x370, 0x37C };
        constexpr int kRecoilOffs[] = { 0x3E0, 0x3E4, 0x3F4, 0x3F8 };
        constexpr int kSpreadCount = sizeof(kSpreadOffs) / sizeof(kSpreadOffs[0]);
        constexpr int kRecoilCount = sizeof(kRecoilOffs) / sizeof(kRecoilOffs[0]);

        struct Saved
        {
            uintptr_t weapon = 0;
            float spread[kSpreadCount] = {};
            float recoil[kRecoilCount] = {};
            bool spreadZeroed = false, recoilZeroed = false;
        };
        Saved g_saved[16];
        int g_next = 0;

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

        // Structural check so a stale / freed weapon is never written to.
        bool ValidWeapon(uintptr_t w)
        {
            uint32_t vt = 0; int32_t mark = 0, maxClip = 0; float base = 0;
            if (w < 0x10000 || w > 0x7FFF0000 || !Rd(w, vt) || vt < 0x1000000 || vt > 0x1800000 || (vt & 3) != 0) return false;
            if (!Rd(w + 0x18, mark) || mark != -1) return false;
            if (!Rd(w + 0x2F8, maxClip) || maxClip < 0 || maxClip > 5000) return false;
            if (!Rd(w + 0x364, base) || !std::isfinite(base)) return false;
            return true;
        }

        Saved* Find(uintptr_t w, bool create)
        {
            for (Saved& s : g_saved) if (s.weapon == w) return &s;
            if (!create) return nullptr;
            Saved& s = g_saved[g_next++ % 16];
            s = Saved();
            s.weapon = w;
            for (int i = 0; i < kSpreadCount; ++i) Rd(w + kSpreadOffs[i], s.spread[i]);
            for (int i = 0; i < kRecoilCount; ++i) Rd(w + kRecoilOffs[i], s.recoil[i]);
            return &s;
        }

        uintptr_t PlayerWeapon()
        {
            uintptr_t pawn = Overlay::GetPlayerPawn();
            if (!pawn) return 0;
            uint32_t w = 0;
            if (!Rd(pawn + 0x37C, w)) return 0;
            return ValidWeapon(w) ? w : 0;
        }
    }

    bool ReadAim(float& base, float& maxOffset, float& perShot, float& current, float& recoil)
    {
        uintptr_t w = currentWeapon;
        return w && Rd(w + 0x364, base) && Rd(w + 0x36C, maxOffset) && Rd(w + 0x370, perShot) && Rd(w + 0x37C, current) && Rd(w + 0x3E0, recoil);
    }

    void Tick()
    {
        uintptr_t w = PlayerWeapon();
        currentWeapon = w;

        // Weapons we changed earlier are restored when the option is switched off (or forgotten when freed).
        for (Saved& s : g_saved)
        {
            if (!s.weapon) continue;
            if (!ValidWeapon(s.weapon)) { s = Saved(); continue; }
            if (s.spreadZeroed && !noSpread)
            {
                for (int i = 0; i < kSpreadCount; ++i) Wr<float>(s.weapon + kSpreadOffs[i], s.spread[i]);
                s.spreadZeroed = false;
            }
            if (s.recoilZeroed && !noRecoil)
            {
                for (int i = 0; i < kRecoilCount; ++i) Wr<float>(s.weapon + kRecoilOffs[i], s.recoil[i]);
                s.recoilZeroed = false;
            }
        }

        if (!w || (!noSpread && !noRecoil)) return;
        Saved* s = Find(w, true);
        if (noSpread)
        {
            for (int i = 0; i < kSpreadCount; ++i) Wr<float>(w + kSpreadOffs[i], 0.f);
            s->spreadZeroed = true;
        }
        if (noRecoil)
        {
            for (int i = 0; i < kRecoilCount; ++i) Wr<float>(w + kRecoilOffs[i], 0.f);
            s->recoilZeroed = true;
        }
    }
}

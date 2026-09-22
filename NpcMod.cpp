#include <Windows.h>
#include "NpcMod.h"
#include "Npc.h"
#include "CrashHandler.h"

namespace NpcMod
{
    bool keepDisarmed = false;
    bool enemiesIgnore = false;
    bool alliesInvincible = false;
    bool alliesKeepHealth = false;
    bool enemiesVulnerable = false;
    bool policeDisarmed = false;
    bool policeIgnore = false;
    bool blindAll = false;
    int statEnemies = 0, statAllies = 0;

    namespace
    {
        volatile long g_disarmRequest = 0, g_healRequest = 0;
        ULONGLONG g_lastTick = 0;

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
        void SetBit(uintptr_t a, int bit, bool on)
        {
            uint32_t v = 0;
            if (!Rd(a, v)) return;
            uint32_t nv = on ? (v | (1u << bit)) : (v & ~(1u << bit));
            if (nv != v) Wr<uint32_t>(a, nv);
        }
        uintptr_t Comp(uintptr_t pawn, int off)
        {
            uint32_t p = 0;
            return (Rd(pawn + off, p) && p > 0x10000 && p < 0x7FFF0000) ? p : 0;
        }

        void Disarm(uintptr_t pawn)
        {
            SetBit(pawn + 0x1E8, 10, true);   // bNoWeaponFiring
            SetBit(pawn + 0x7D0, 1, false);   // m_bShooting
        }

        void Ignore(uintptr_t pawn)
        {
            if (uintptr_t vis = Comp(pawn, 0x54C)) SetBit(vis + 0x80, 0, true);        // m_bBlind
            if (uintptr_t cmb = Comp(pawn, 0x834))
            {
                SetBit(cmb + 0x54, 0, false);   // m_bAllowCombat
                SetBit(cmb + 0x54, 4, false);   // m_bHuntPlayer
                SetBit(cmb + 0x54, 5, false);   // m_bTargetPlayer
                // m_bForgetTarget makes the game drop the target itself. Clearing m_pEnemyTarget (+0x74)
                // by hand crashed the game: the combat code reads target+0xEC (Velocity) without a null check.
                SetBit(cmb + 0x54, 6, true);    // m_bForgetTarget
            }
        }

        void Restore(uintptr_t pawn)
        {
            if (uintptr_t vis = Comp(pawn, 0x54C)) SetBit(vis + 0x80, 0, false);
            if (uintptr_t cmb = Comp(pawn, 0x834))
            {
                SetBit(cmb + 0x54, 0, true);
                SetBit(cmb + 0x54, 6, false);
            }
        }

        void Heal(uintptr_t pawn)
        {
            int32_t hp = 0, maxHp = 0;
            if (Rd(pawn + 0x2AC, hp) && Rd(pawn + 0x6F8, maxHp) && maxHp > 0 && maxHp < 100000 && hp > 0 && hp < maxHp)
                Wr<int32_t>(pawn + 0x2AC, maxHp);
        }
    }

    void SetInvincible(uintptr_t pawn, bool on)
    {
        SetBit(pawn + 0x1E8, 14, on);   // m_bInvincible
        SetBit(pawn + 0x1E8, 15, on);   // m_bKismetInvincible
    }

    void DisarmNow() { CrashHandler::Note("npc DisarmNow"); InterlockedExchange(&g_disarmRequest, 1); }
    void HealAlliesNow() { CrashHandler::Note("npc HealAlliesNow"); InterlockedExchange(&g_healRequest, 1); }

    void Tick()
    {
        static bool prevIgnore = false, prevPoliceIgnore = false, prevBlindAll = false;
        const bool disarmReq = g_disarmRequest != 0, healReq = g_healRequest != 0;
        const bool any = keepDisarmed || enemiesIgnore || alliesInvincible || alliesKeepHealth || enemiesVulnerable || policeDisarmed || policeIgnore || blindAll || prevBlindAll || disarmReq || healReq || prevIgnore || prevPoliceIgnore;
        if (!any) return;
        ULONGLONG now = GetTickCount64();
        if (now - g_lastTick < 150 && !disarmReq && !healReq) return;
        g_lastTick = now;
        InterlockedExchange(&g_disarmRequest, 0);
        InterlockedExchange(&g_healRequest, 0);

        static Npc::Info list[256];
        int n = Npc::Snapshot(list, 256, false);
        statEnemies = statAllies = 0;
        for (int i = 0; i < n; ++i)
        {
            const Npc::Info& p = list[i];
            if (p.health <= 0) continue;
            // Stealth / tailing missions: every AI pawn - mission targets and pedestrians included - gets a blind vision component.
            if (blindAll) { if (uintptr_t vis = Comp(p.address, 0x54C)) SetBit(vis + 0x80, 0, true); }
            else if (prevBlindAll && !(p.enemy && enemiesIgnore) && !(p.police && policeIgnore)) { if (uintptr_t vis = Comp(p.address, 0x54C)) SetBit(vis + 0x80, 0, false); }
            // Protected enemies (bosses, scripted): strip the invincibility bits. Enemies are recognised by how they
            // treat the player (Npc.h), so the protected NPC to rescue and mission allies are left alone.
            if (enemiesVulnerable && p.enemy && p.invincible) SetInvincible(p.address, false);
            if (p.enemy)
            {
                ++statEnemies;
                if (keepDisarmed || disarmReq) Disarm(p.address);
                if (enemiesIgnore) Ignore(p.address);
                else if (prevIgnore) Restore(p.address);
            }
            else if (p.police)
            {
                if (policeDisarmed || disarmReq) Disarm(p.address);
                if (policeIgnore) Ignore(p.address);
                else if (prevPoliceIgnore) Restore(p.address);
            }
            else if (p.friendly)
            {
                ++statAllies;
                if (alliesInvincible)
                {
                    SetBit(p.address + 0x1E8, 14, true);    // m_bInvincible
                    SetBit(p.address + 0x1E8, 15, true);    // m_bKismetInvincible
                }
                if (alliesKeepHealth || healReq) Heal(p.address);
            }
        }
        prevIgnore = enemiesIgnore;
        prevPoliceIgnore = policeIgnore;
        prevBlindAll = blindAll;
    }
}


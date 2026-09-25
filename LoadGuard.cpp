#include <Windows.h>
#include <cstdint>
#include "LoadGuard.h"
#include "Overlay.h"
#include "BuiltinCheats.h"
#include "Log.h"

namespace LoadGuard
{
    bool enabled = true;
    int settleSeconds = 1;
    int slowGraceSeconds = 5;

    namespace
    {
        volatile long g_quiet = 1;          // starts quiet: nothing is known about the world yet
        volatile long g_hard = 1;
        volatile ULONGLONG g_until = 0;     // hard quiet (world change, mod-started teleport / mission command)
        volatile ULONGLONG g_softUntil = 0; // soft quiet (cut-scene slow motion)
        ULONGLONG g_slowSince = 0;          // when the world slow-down (TimeDilation != 1) began
        uintptr_t g_lastPawn = 0, g_lastWi = 0;
        float g_lastTime = 0.f;
        float g_lastDil = 1.f;
        const char* g_reason = "starting";

        template <class T> bool Rd(uintptr_t a, T& out)
        {
            __try { out = *reinterpret_cast<const T*>(a); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    void Hold(int seconds, const char* reason)
    {
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000ull;
        if (until > g_until) g_until = until;
        g_reason = reason;
        g_quiet = g_hard = 1;
        LogF("LoadGuard: hold %d s (%s)", seconds, reason);
    }

    void Update()
    {
        if (!enabled) { g_quiet = 0; g_hard = 0; return; }
        const ULONGLONG now = GetTickCount64();
        bool changed = false, slow = false;
        const uintptr_t pawn = Overlay::GetPlayerPawn();
        if (!pawn) { changed = true; g_lastPawn = 0; g_reason = "no player yet (loading)"; }
        else
        {
            uint32_t wi = 0; float t = 0.f, dil = 1.f;
            if (Rd(pawn + 0x94, wi) && wi > 0x10000 && wi < 0x7FFF0000)
            {
                Rd(static_cast<uintptr_t>(wi) + 0x31C, t);      // WorldInfo.TimeSeconds
                Rd(static_cast<uintptr_t>(wi) + 0x318, dil);    // WorldInfo.TimeDilation
            }
            if (pawn != g_lastPawn) { changed = true; g_reason = "new player pawn"; }
            else if (wi != g_lastWi) { changed = true; g_reason = "new world"; }
            else if (t + 0.5f < g_lastTime) { changed = true; g_reason = "world clock restarted"; }
            // The game slows the world down for its own cut-scenes / focus effects. That is not our slow motion cheat.
            if (BuiltinCheats::timeScale == 1.f && (dil < 0.98f || dil > 1.02f)) { slow = true; g_reason = "cut-scene / slow motion"; }
            g_lastPawn = pawn; g_lastWi = wi; g_lastTime = t; g_lastDil = dil;
        }
        if (changed)
        {
            const ULONGLONG until = now + static_cast<ULONGLONG>(settleSeconds < 1 ? 1 : settleSeconds) * 1000ull;
            if (until > g_until) g_until = until;
            if (!g_quiet) LogF("LoadGuard: quiet (%s) for %d s", g_reason, settleSeconds);
        }
        // Short slow motion (a car-to-car jump, a focus effect) is normal gameplay. A cut-scene or a load keeps the world
        // slowed down for a long time, so only a slow-down that lasts longer than the grace time counts.
        if (!slow) g_slowSince = 0;
        else
        {
            if (!g_slowSince) g_slowSince = now;
            if (now - g_slowSince >= static_cast<ULONGLONG>(slowGraceSeconds < 1 ? 1 : slowGraceSeconds) * 1000ull)
            {
                if (!g_quiet) LogF("LoadGuard: quiet (%s)", g_reason);
                g_softUntil = now + 400;
            }
        }
        const bool hard = now < g_until, soft = now < g_softUntil;
        if (!(hard || soft) && g_quiet) LogF("LoadGuard: active again");
        g_hard = hard ? 1 : 0;
        g_quiet = (hard || soft) ? 1 : 0;
    }

    bool Quiet() { return g_quiet != 0; }
    bool Hard() { return g_hard != 0; }
    int SecondsLeft()
    {
        const ULONGLONG now = GetTickCount64(), u = g_until > g_softUntil ? g_until : g_softUntil;
        return g_quiet && u > now ? static_cast<int>((u - now + 999) / 1000) : 0;
    }
    const char* Reason() { return g_reason; }
    float LastTimeDilation() { return g_lastDil; }
    float LastTimeSeconds() { return g_lastTime; }
}

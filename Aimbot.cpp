#include <Windows.h>
#include <cmath>
#include "imgui/imgui.h"
#include "Aimbot.h"
#include "Npc.h"
#include "Overlay.h"
#include "ESP.h"
#include "D3DHook.h"
#include "Patches.h"
#include "Offsets.h"

namespace Aimbot
{
    bool enabled = false;
    int  aimKey = KeyRightMouse;
    bool onlyEnemies = true;
    float fovPixels = 250.f;
    float smoothing = 0.25f;
    float maxDistance = 100.f;
    float headHeight = 70.f;
    float offsetX = 0.f, offsetY = 0.f;
    bool autoShot = false;
    float shotRadius = 14.f;
    int  shotIntervalMs = 130;
    bool drawFov = true;
    bool useBoneHead = true;
    bool experimental = true;
    uintptr_t statMode = 0;
    int statBoomPitch = 0, statBoomYaw = 0;

    volatile long pendingDx = 0, pendingDy = 0;
    volatile long fireHeld = 0;
    volatile long statStateCalls = 0, statDataCalls = 0, statInjected = 0, statCursorCalls = 0;
    int statTargets = 0;
    bool statAligned = false;
    float statDist = 0.f;
    volatile long statFireInjected = 0;
    float statErrX = 0.f, statErrY = 0.f;

    namespace
    {
        bool g_haveTarget = false;
        float g_targetX = 0.f, g_targetY = 0.f;
        ULONGLONG g_downAt = 0, g_upAt = 0;
        bool g_firing = false;
        bool g_sentDown = false;   // a real (SendInput) left button press is in progress
        float g_accX = 0.f, g_accY = 0.f;

        bool KeyHeld()
        {
            switch (aimKey)
            {
            case KeyRightMouse: return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            case KeyLeftMouse:  return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            case KeyAlt:        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            case KeyShift:      return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            case KeyX:          return (GetAsyncKeyState('X') & 0x8000) != 0;
            case KeyMouse4:     return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
            default:            return true;
            }
        }

        // The fire button goes out on two paths: into the game's DirectInput mouse reads (fireHeld) and as a real
        // SendInput click, for the case where the game takes the buttons from window messages / async key state.
        void SendButton(bool down)
        {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            SendInput(1, &in, sizeof(INPUT));
        }

        void ReleaseShot()
        {
            if (g_firing) { g_firing = false; g_upAt = GetTickCount64(); }
            if (g_sentDown) { SendButton(false); g_sentDown = false; }
            InterlockedExchange(&fireHeld, 0);
        }
    }

    void Tick()
    {
        g_haveTarget = false;
        statTargets = 0;
        if (!enabled || Overlay::wantMouseCapture || ImGui::GetIO().WantCaptureMouse)
        {
            ReleaseShot();
            return;
        }
        HWND fg = GetForegroundWindow();
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) { ReleaseShot(); return; }

        float ox = 0.f, oy = 0.f, oz = 0.f;
        bool haveOrigin = Overlay::GetPlayerPosition(ox, oy, oz);

        const float cx = g_viewportWidth * 0.5f + offsetX, cy = g_viewportHeight * 0.5f + offsetY;
        static Npc::Info list[256];
        int n = Npc::Snapshot(list, 256, useBoneHead);
        float bestDist2 = fovPixels * fovPixels;
        for (int i = 0; i < n; ++i)
        {
            const Npc::Info& p = list[i];
            if (p.health <= 0 || p.friendly) continue;
            if (onlyEnemies && !p.enemy) continue;
            if (haveOrigin)
            {
                float dx = p.x - ox, dy = p.y - oy, dz = p.z - oz;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f;
                if (d > maxDistance) continue;
                if (d < 0.5f) continue;
            }
            float sx, sy;
            bool ok = p.hasBones ? WorldToScreen(p.head[0], p.head[1], p.head[2], sx, sy)
                                 : WorldToScreen(p.x, p.y, p.z + headHeight, sx, sy);
            if (!ok) continue;
            float ex = sx - cx, ey = sy - cy;
            float d2 = ex * ex + ey * ey;
            if (d2 > fovPixels * fovPixels) continue;
            ++statTargets;
            if (d2 < bestDist2) { bestDist2 = d2; g_targetX = sx; g_targetY = sy; g_haveTarget = true; }
        }

        if (!g_haveTarget || !KeyHeld())
        {
            ReleaseShot();
            g_accX = g_accY = 0.f;
            InterlockedExchange(&pendingDx, 0); InterlockedExchange(&pendingDy, 0);
            return;
        }

        // ---- aim: proportional nudge, merged into the game's next mouse read ----
        float ex = g_targetX - cx, ey = g_targetY - cy;
        statErrX = ex; statErrY = ey;
        float dist = std::sqrt(ex * ex + ey * ey);

        if (experimental)
        {
            // Bypass the mouse: the on-foot camera mode keeps its input-driven target in
            // m_DesiredBoomRotation (mode +0x190 pitch, +0x194 yaw, 65536 = 360 degrees; writing
            // it moves the final view rotation at +0x1E8 one-to-one - verified live).
            uintptr_t mode = 0;
            statMode = 0;
            float M[16];
            if (Overlay::GetCameraMode(mode) && GetCandidateViewProjMatrix(M))
            {
                auto rdi = [](uintptr_t a, int32_t& v) { __try { v = *reinterpret_cast<int32_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } };
                int32_t bp = 0, by = 0, fp = 0, fy = 0;
                bool ok = rdi(mode + 0x190, bp) && rdi(mode + 0x194, by) && rdi(mode + 0x1E8, fp) && rdi(mode + 0x1EC, fy);
                // Sanity: the desired boom must lie close to the final view rotation.
                int dyaw = ((by - fy) % 65536 + 65536 + 32768) % 65536 - 32768;
                if (ok && std::abs(dyaw) < 8000 && std::abs(bp - fp) < 8000)
                {
                    statMode = mode; statBoomPitch = bp; statBoomYaw = by;
                    // Pixels per radian at the screen centre from the projection scale (column norms).
                    float px = std::sqrt(M[0] * M[0] + M[4] * M[4] + M[8] * M[8]) * g_viewportWidth * 0.5f;
                    float py = std::sqrt(M[1] * M[1] + M[5] * M[5] + M[9] * M[9]) * g_viewportHeight * 0.5f;
                    if (px > 1.f && py > 1.f)
                    {
                        const float kUnitsPerRad = 65536.f / 6.2831853f;
                        float dYaw = ex / px * kUnitsPerRad * smoothing;
                        float dPitch = -ey / py * kUnitsPerRad * smoothing;
                        if (dYaw > 1500.f) dYaw = 1500.f; if (dYaw < -1500.f) dYaw = -1500.f;
                        if (dPitch > 1500.f) dPitch = 1500.f; if (dPitch < -1500.f) dPitch = -1500.f;
                        g_accX += dYaw; g_accY += dPitch;
                        int iy = static_cast<int>(g_accX), ip = static_cast<int>(g_accY);
                        g_accX -= iy; g_accY -= ip;
                        __try
                        {
                            *reinterpret_cast<int32_t*>(mode + 0x194) = by + iy;
                            *reinterpret_cast<int32_t*>(mode + 0x190) = bp + ip;
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
            }
            InterlockedExchange(&pendingDx, 0); InterlockedExchange(&pendingDy, 0);
        }
        else
        {
        g_accX += ex * smoothing;
        g_accY += ey * smoothing;
        int mx = static_cast<int>(g_accX), my = static_cast<int>(g_accY);
        if (mx > 120) mx = 120; if (mx < -120) mx = -120;
        if (my > 120) my = 120; if (my < -120) my = -120;
        g_accX -= mx; g_accY -= my;
        // Replace (not add) whatever the game hasn't read yet, so slow frames can't pile up.
        InterlockedExchange(&pendingDx, mx);
        InterlockedExchange(&pendingDy, my);
        }

        // ---- auto shot ----
        if (autoShot)
        {
            ULONGLONG now = GetTickCount64();
            bool aligned = dist <= shotRadius;
            statAligned = aligned; statDist = dist;
            if (g_firing)
            {
                if (!aligned || now - g_downAt > 90) ReleaseShot();
            }
            else if (aligned && now - g_upAt >= static_cast<ULONGLONG>(shotIntervalMs))
            {
                g_firing = true; g_downAt = now;
                InterlockedExchange(&fireHeld, 1);
                SendButton(true); g_sentDown = true;
            }
        }
        else ReleaseShot();
    }

    void Draw()
    {
        if (!enabled || !drawFov) return;
        ImDrawList* d = ImGui::GetForegroundDrawList();
        const float cx = g_viewportWidth * 0.5f + offsetX, cy = g_viewportHeight * 0.5f + offsetY;
        d->AddCircle(ImVec2(cx, cy), fovPixels, IM_COL32(255, 255, 255, 70), 64, 1.f);
        if (g_haveTarget)
        {
            d->AddCircle(ImVec2(g_targetX, g_targetY), 6.f, IM_COL32(255, 60, 60, 255), 16, 2.f);
            d->AddLine(ImVec2(cx, cy), ImVec2(g_targetX, g_targetY), IM_COL32(255, 60, 60, 120), 1.f);
        }
    }
}

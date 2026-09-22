#include <Windows.h>
#include <cstdint>
#include <cmath>
#include "MouseLook.h"
#include "Overlay.h"
#include "LoadGuard.h"

namespace MouseLook
{
    bool enabled = false;
    float sensitivity = 0.05f;
    float aimScale = 0.5f;
    bool invertY = false;
    volatile long active = 0;
    volatile long accX = 0, accY = 0;
    const char* status = "off";

    namespace
    {
        float g_remYaw = 0.f, g_remPitch = 0.f;    // fractions of a unit (65536 units = 360 degrees) that did not fit into an integer yet
        constexpr int kPitchLimit = 15000;          // about +-82 degrees
        volatile uintptr_t g_mode = 0;              // the validated on-foot camera mode (found by Tick, used by GameTick)

        bool Rd(uintptr_t a, int32_t& v) { __try { v = *reinterpret_cast<int32_t*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        bool Wr(uintptr_t a, int32_t v) { __try { *reinterpret_cast<int32_t*>(a) = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

        void Deactivate()
        {
            InterlockedExchange(&active, 0); g_mode = 0;
            InterlockedExchange(&accX, 0); InterlockedExchange(&accY, 0);
            g_remYaw = g_remPitch = 0.f;
        }
    }

    // Render thread: decides whether the mode may be used and tells the input hooks to hand over the raw motion.
    void Tick()
    {
        if (!enabled || Overlay::wantMouseCapture || LoadGuard::Quiet())
        {
            Deactivate();
            status = enabled ? "waiting (menu / loading)" : "off";
            return;
        }
        uintptr_t mode = 0;
        if (!Overlay::GetCameraMode(mode))
        {
            Deactivate();
            status = "this camera is not supported (vehicle or cinematic): the game's own mouse";
            return;
        }
        int32_t bp = 0, by = 0, fp = 0, fy = 0;
        const bool ok = Rd(mode + 0x190, bp) && Rd(mode + 0x194, by) && Rd(mode + 0x1E8, fp) && Rd(mode + 0x1EC, fy);
        const int dyaw = ((by - fy) % 65536 + 65536 + 32768) % 65536 - 32768;
        if (!ok || std::abs(dyaw) >= 8000 || std::abs(bp - fp) >= 8000)
        {
            Deactivate();
            status = "camera state not recognised: the game's own mouse";
            return;
        }
        g_mode = mode;
        InterlockedExchange(&active, 1);
        status = "active";
    }

    // Game thread, once per game frame (the police subsystem's update): the mouse motion collected since the last frame is added
    // to the camera's target in one step, in phase with the game's own camera update. Doing it from the render thread at an
    // unrelated moment made the motion uneven.
    void GameTick()
    {
        if (!active) return;
        const uintptr_t mode = g_mode;
        if (!mode) return;
        const long dx = InterlockedExchange(&accX, 0), dy = InterlockedExchange(&accY, 0);
        if (!dx && !dy) return;
        int32_t bp = 0, by = 0;
        if (!Rd(mode + 0x190, bp) || !Rd(mode + 0x194, by)) return;

        const float unitsPerDegree = 65536.0f / 360.0f;
        const float k = sensitivity * ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? aimScale : 1.0f);   // right mouse button = aiming
        g_remYaw += static_cast<float>(dx) * k * unitsPerDegree;
        g_remPitch += static_cast<float>(dy) * k * unitsPerDegree * (invertY ? 1.0f : -1.0f);
        const int iy = static_cast<int>(g_remYaw), ip = static_cast<int>(g_remPitch);
        g_remYaw -= static_cast<float>(iy); g_remPitch -= static_cast<float>(ip);
        int newPitch = bp + ip;
        if (newPitch > kPitchLimit) newPitch = kPitchLimit;
        if (newPitch < -kPitchLimit) newPitch = -kPitchLimit;
        Wr(mode + 0x194, by + iy);
        Wr(mode + 0x190, newPitch);
    }
}

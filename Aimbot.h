#pragma once

// Aimbot on NPC pawns (see Npc.h). Works purely on the screen: the target's head is projected
// with the captured View*Projection matrix and the mouse motion is injected straight into the
// game's DirectInput mouse reads (InputHook.cpp) - SendInput never reached the game. The loop is
// proportional, so it converges whatever the game's sensitivity is. AutoShot holds the injected
// left button while the head is inside a small circle around the crosshair. There is no
// line-of-sight test, so it will also aim through walls.
namespace Aimbot
{
    enum AimKey { KeyRightMouse = 0, KeyLeftMouse, KeyAlt, KeyShift, KeyX, KeyMouse4, KeyAlways };

    extern bool enabled;
    extern int  aimKey;
    extern bool onlyEnemies;
    extern float fovPixels;       // only targets whose head is within this radius of the crosshair
    extern float smoothing;       // 0.05 (slow) .. 1 (snap)
    extern float maxDistance;     // metres
    extern float headHeight;      // Unreal units above the pawn's Location (fallback without bones)
    extern float offsetX, offsetY; // crosshair offset from the screen centre (pixels)
    extern bool autoShot;
    extern float shotRadius;      // pixels
    extern int  shotIntervalMs;
    extern bool drawFov;
    extern bool experimental;     // aim by writing the camera's desired boom rotation instead of injecting mouse motion
    extern uintptr_t statMode;    // camera mode object used by the experimental aim (0 = none)
    extern int statBoomPitch, statBoomYaw;
    extern bool useBoneHead;      // aim at the skeleton's head (falls back to headHeight without bones)

    // Mouse motion / button state waiting to be merged into the game's next DirectInput read.
    extern volatile long pendingDx, pendingDy;
    extern volatile long fireHeld;
    // Diagnostics shown in the Aimbot tab.
    extern volatile long statStateCalls, statDataCalls, statInjected, statCursorCalls;
    extern bool statAligned;      // crosshair within shotRadius of the head last frame
    extern float statDist;
    extern volatile long statFireInjected; // frames the fire button went through DirectInput
    extern int statTargets;       // candidates inside the FOV last frame
    extern float statErrX, statErrY;

    void Tick();   // every frame
    void Draw();   // FOV circle + target marker (ImGui foreground draw list)
}

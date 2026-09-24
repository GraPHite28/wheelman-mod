#pragma once

// Direct mouse camera (experimental). The console-born game turns mouse movement into an emulated controller stick (dead zone,
// acceleration curve, smoothing), which feels sluggish and uneven with a mouse. This mode takes the raw mouse motion out of the
// DirectInput reads (the game then sees no movement) and rotates the on-foot camera itself, the same way the aimbot's experimental
// aiming does: the camera mode keeps its input-driven target in m_DesiredBoomRotation (mode +0x190 pitch, +0x194 yaw, 65536 = 360
// degrees) and the final view rotation follows it one-to-one. Only the on-foot camera is handled; in a vehicle or a cut-scene
// the mouse goes to the game untouched.
namespace MouseLook
{
    extern bool enabled;
    extern float sensitivity;        // degrees of rotation per mouse count
    extern float aimScale;           // multiplier of the sensitivity while the right mouse button (aiming) is held
    extern bool invertY;
    // Bind this to a key in "Hold" mode (right-click the checkbox in the menu) to let go of the mouse for the game's
    // own menus (the PDA, pause) without turning the feature off: the game's PDA cursor rides on the same raw mouse
    // motion this mode normally diverts to itself, so there is no other way for the game to see it while this is on.
    extern bool suspended;
    extern volatile long active;     // 1 while the on-foot camera is valid and the raw motion is taken over (read by the input hooks)
    extern volatile long accX, accY; // raw mouse counts collected by the input hooks since the last Tick
    extern const char* status;

    void Tick();                     // render thread, once per frame: validates the camera and switches the input takeover on / off
    void GameTick();                 // game thread, once per game frame (police subsystem pump): applies the collected motion
}

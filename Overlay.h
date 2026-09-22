#pragma once

namespace Overlay
{
    extern bool visible;         // Insert: show/hide the ImGui window
    extern bool wantMouseCapture; // Home: whether the game's mouse input should be suppressed
    void Draw();

    // Registers every bindable option / config value (Binds.h); call once after ImGui is up.
    void InitFeatures();

    // Settings tab switches. Debug unlocks the Debug tab, Unsafe unlocks vehicle spawning.
    extern bool debugEnabled;
    extern bool unsafeEnabled;

    // A small draggable/scalable speed readout, independent of the main
    // debug window so it can stay on screen while that's hidden. Drag it by
    // its title bar and scroll the mouse wheel over it to resize (both only
    // work while the main overlay is toggled on, since that's what routes
    // mouse input to ImGui instead of the game - see D3DHook.cpp).
    extern bool speedometerVisible;
    void DrawSpeedometer();

    // World-to-screen vehicle markers (health/speed/distance). Also gated by
    // its own "Enable ESP" checkbox on the ESP tab, drawn unconditionally
    // each frame like the speedometer so it works while the menu is hidden.
    void DrawESPOverlay();

    // Per-frame enforcement for the Player tab's god-mode options; safe to
    // call every frame even with the menu hidden.
    void PlayerTick();

    // Player pawn position (Actor.Location); false while the pawn hasn't been located yet.
    bool GetPlayerPosition(float& x, float& y, float& z);
    uintptr_t GetPlayerPawn();   // validated WheelmanPlayerPawn, 0 if not found

    // Current WheelmanCamera mode object (camera+0x47C); false if the camera isn't found yet.
    bool GetCameraMode(uintptr_t& mode);
}

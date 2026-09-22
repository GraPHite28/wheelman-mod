#pragma once

// The overlay's look: black and charcoal panels, amber accent, square corners (the colours of the game's own
// HUD / menus). Call once after ImGui::CreateContext and before the renderer backend is initialised (it adds the font).
namespace Theme
{
    void Apply();
}

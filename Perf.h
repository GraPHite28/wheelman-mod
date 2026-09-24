#pragma once

// Frame limiter. The game (originally a console title) ties parts of its logic to the frame
// rate, so the mod paces EndScene to a fixed FPS (default 60): coarse Sleep(1) waits plus a
// short spin for the last fraction of a millisecond.
namespace Perf
{
    extern bool limitEnabled;
    extern int  targetFps;      // 20..240
    extern float measuredFps;   // smoothed, for the UI

    void Throttle();            // call once per frame, after the real EndScene
    void Resync();              // call after a gap where Throttle() was not called (e.g. LoadGuard was quiet):
                                 // resets the pacing baseline to "now" instead of catching up
}

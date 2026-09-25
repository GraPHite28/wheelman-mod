#pragma once

// Loading protection. While the game loads a level (or plays the cut-scene that follows) the mod must not touch its memory:
// the heap-walking workers that WRITE into live objects (peaceful police components, jack-anywhere records), the per-frame
// writers (police level, patches, marker flags ...) and the overlay itself can race with the loader that frees and rebuilds
// those very objects. The guard turns the whole mod quiet whenever the player pawn is missing or has just changed, or the
// world's clock jumped back (a new world), and keeps it quiet for a settle time afterwards (the cut-scene).
namespace LoadGuard
{
    extern bool enabled;             // Settings -> Loading protection
    extern int slowGraceSeconds;     // a slow-down of the world counts as a cut-scene only after this long (jumps between cars are short)
    extern int settleSeconds;        // extra quiet time after the world is stable again (default 1 s)

    // A mod-started teleport or mission command can start a mission / cut-scene by itself: the mod goes hard-quiet right
    // after it (call from the game thread AFTER the action ran).
    void Hold(int seconds, const char* reason);
    bool Hard();                     // hard quiet: nothing at all (loading, world change). Soft quiet (cut-scene slow motion)
                                     // still lets the tiny top-speed lock run.
    void Update();                  // once per frame from the render hook (cheap; never scans while a scan is running)
    bool Quiet();                    // true while the mod must stay away from the game; safe from any thread
    int SecondsLeft();               // remaining settle time, 0 when active
    const char* Reason();            // why the guard is quiet (for the UI)
    float LastTimeDilation();        // WorldInfo.TimeDilation as last read (1 = normal, 0 = fully paused); for diagnostics
    float LastTimeSeconds();         // WorldInfo.TimeSeconds as last read; for diagnostics - watch whether it keeps advancing
}

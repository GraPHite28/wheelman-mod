#pragma once
#include <cstdint>

// The map: every world marker (class WheelmanMarker and its subclasses: garages, unlockable caches, mission / event
// markers, safe houses, taxi drop-offs, potatoes, checkpoints ...) and the HUD minimap.
//
// WheelmanMarker layout (from the decompiled Default__ groups, offsets of the bit words are derived and shown raw in the
// Map tab so they can be checked in game): +0x1C4 bit0 m_bDisplayOnMap, bit1 m_bDisplayInGame; +0x1CC m_eMapMarker
// (enum MissionMarkers, 81 icon types, 0 = none), +0x1CD m_eUIMapMarker; +0x1D4 bit0 m_bLastDisplayOnMap, bit2
// m_bDisabledOnMap, bit3 m_bLastDisabledOnMap; +0x1DC m_iDisplayOnMapOverride. WheelmanUnlockable (garages, weapon and
// ammo caches): +0x1E4 m_eState (0 locked, 1 unavailable, 2 available, 3 temporarily unavailable).
// HUD screens (WheelmanUIScreenHUDBase and its subclasses): +0x270 bit10 m_bShowMiniMap, bit11 m_bShowGPS, +0x36C
// m_fMapScale, +0x38C m_fMapMarkerScale, +0x39C m_iMaxMapMarkers.
namespace MapTools
{
    struct Row
    {
        uintptr_t obj;
        int cls;                        // index into the class table
        const char* className;
        float x, y, z;
        int icon, uiIcon;               // MissionMarkers values
        bool onMap, inGame, disabled;
        bool hasState; int state;
        uint32_t rawA, rawB;            // +0x1C4 / +0x1D4
        float sizeInWorld;              // m_fSizeInWorld (+0x1D8): size of the police influence zone (cm)
    };

    extern bool showGaragesOnMap;       // keep garages visible on the map (forces the display flags)
    extern bool showAllOnMap;           // same for every marker that has an icon
    // Everything that writes the map flags of the game's markers (keep on the map, show caches, Show-all buttons) changes what
    // the game's own minimap draws, so it only works while the Unsafe switch is on (set by the render hook every frame).
    extern bool markerWritesAllowed;
    extern bool cachesAlwaysOpen;      // weapon / ammo caches are forced to state 2 (available), like "garages always open"
    extern bool showCachesOnMap;        // give the caches the ammo-stash icon and show them on the game's map
    int UnlockCaches();                 // one-shot: every scanned cache -> available
    extern float hudZoom;              // multiplier of the HUD minimap scale (1 = the game's own)
    extern const char* message;

    int Scan();                         // heap walk (button); ~150 ms, blocks the caller
    void ScanAsync();                   // the same on a background thread: the periodic callers use this, the game never stalls
    int Count();
    bool At(int i, Row& out);
    int ClassCount();
    const char* ClassName(int c);
    const char* IconName(int icon);
    int IconCount();
    void MakeVisible(uintptr_t marker);
    int ShowAllNow(bool onlyGarages);   // one-shot, returns the number of markers changed
    void Tick();                        // every frame (render thread)

    // ---- minimap (HUD screens) ----
    struct Hud { uintptr_t obj; const char* className; };
    int ScanHud();
    void ScanHudAsync();
    int HudCount();
    bool HudAt(int i, Hud& out);
    bool HudLive(uintptr_t hud);        // a running HUD screen (not an archetype / default object)
    // reads / writes on a HUD screen object
    bool GetBit(uintptr_t hud, int offset, int bit);
    void SetBit(uintptr_t hud, int offset, int bit, bool on);
    float GetFloat(uintptr_t hud, int offset);
    void SetFloat(uintptr_t hud, int offset, float v);
    int GetInt(uintptr_t hud, int offset);
    void SetInt(uintptr_t hud, int offset, int v);
}

#pragma once
#include "MapTools.h"

struct IDirect3DDevice9;

// External minimap: a movable overlay window (round map, player arrow, heading-up or north-up, zoom on the mouse wheel)
// with the world markers found by MapTools (garages, caches, missions, events ...) and the police officers around.
// Stage 1: icons only; roads and a GPS route are the next stages (see MapTools / MwyTraffic notes).
// World -> map: 1 unit = 1 cm; screen up = the player's heading (rotate) or +X (north-up), screen right = +Y.
// Markers are sorted into categories by their icon (m_eMapMarker, enum MissionMarkers) - every category has its own
// checkbox, colour and shape.
namespace MiniMap
{
    enum Category
    {
        CatGarage, CatStory, CatGang, CatRace, CatHotPotato, CatRampage, CatContracts, CatMadeToOrder, CatTaxi, CatFugitive,
        CatEventRank, CatWeapons, CatVehicle, CatPoliceMarker, CatOther,
        kMarkerCategoryCount,            // the categories above are world markers, the ones below live NPCs
        CatPoliceNpc = kMarkerCategoryCount, CatEnemyNpc, CatAllyNpc, CatSpecialNpc,
        kCategoryCount
    };

    extern bool enabled;
    extern bool rotate;              // heading-up (the map turns with the player)
    extern float rangeMeters;        // radius shown
    extern int sizePx;               // window size
    extern float opacity;
    extern float iconScale;          // multiplier of the marker size
    extern bool onlyGameVisible;     // only markers the game itself shows on its map
    extern bool catOn[kCategoryCount];
    extern float catScale[kCategoryCount];   // icon size multiplier per category (on top of iconScale)
    extern bool showPoliceZone;              // the police influence circle, like on the game's minimap
    extern float zoneScale;                  // multiplier of the zone radius
    extern int autoScanSeconds;      // 0 = only when the Scan button is pressed

    // city map picture behind the markers (MapData\citymap.dxt1 next to the DLL, extracted from the game's UI_PDA package)
    extern bool background;
    extern float bgOpacity;
    extern float mapOriginX, mapOriginY, mapCmPerPx;   // world (cm) -> map pixel calibration
    // lay this map over the game's own minimap: same zoom and rotation
    extern bool square;                    // square minimap instead of the round one
    extern bool matchGame;
    extern bool matchScale;                // with matchGame: take the zoom from the game's minimap (off = own zoom, mouse wheel)
    extern bool matchRotation;             // with matchGame: take the rotation from the game's minimap
    extern float matchZoom;                // fine-tune multiplier of the matched radius
    extern float matchYawOffsetDeg;
    extern bool matchYawInvert;
    void SetDevice(IDirect3DDevice9* device);   // from the render hook
    const char* BackgroundStatus();
    void ReloadBackground();
    bool ReadGameMinimapValues(float& scale, float& maskW, float& yawUnits);   // false while no HUD screen is running

    const char* CategoryLabel(int c);   // English label (translated by the UI layer)
    const char* CategoryKey(int c);     // config key suffix
    unsigned CategoryColor(int c);      // IM_COL32 value
    int CategoryOfRow(const MapTools::Row& r);   // category of a scanned marker
    bool NeedsNpcScan();                // police officers are drawn: the NPC scanner must run

    void DrawGpsTab();               // the GPS tab: marker list + full-size map, click to set a waypoint
    void Draw();                     // every frame from the render hook, also with the menu closed
}

#pragma once
#include <cstdint>

// Vehicle spawner. Raw UWorld::SpawnActor (0x82D5F0) does NOT work on its own: the vehicle's own init (0x489470 ->
// 0x4AE2C0) needs the definition object that the game's vehicle actor factory hands over in the post-spawn callback
// (crash analysis: vehicle+0x354 == NULL, half-built car left in the world, crash on destruction).
//
// The game spawns traffic through a factory object (vtable 0x13398E8) and its method 0x498340 (thiscall, ret 0xC):
//     factory+0x54 = class to spawn, factory+0x5C = vehicle definition (what ends up in vehicle+0xA00),
//     0x498340(ecx = factory, FVector* loc, FRotator* rot, 0)  ->  collision check, SpawnActor with the post-spawn
//     callback, definition hookup (0x4AD840); returns the new actor or 0 (blocked).
// The factory instance is captured when the game itself finishes a vehicle spawn (hook on the factory's post-spawn
// handler 0x486B90 in Patches.cpp), then a spawn temporarily
// points the factory at the model vehicle's class (vehicle+0x28) and definition (vehicle+0xA00) and calls the method
// from the police subsystem's per-frame update (a game-thread context).
namespace Spawner
{
    // Queue a spawn of a copy of `model` (a live vehicle) 9 m in front of the player. False if not possible.
    bool Request(uintptr_t model);
    // Same, for a (class, definition) pair remembered from a spawn the game did itself; works on foot.
    bool RequestPair(uintptr_t cls, uintptr_t def);

    // Vehicle definitions found in memory (all objects with the definition vtable 0x132D2B0). A vehicle's definition is
    // vehicle+0xA00; "populated" ones (+0x34 non-zero) are the loaded ones, the rest are bare archetype stubs.
    // nameIdx / family (the name index of the def's outer group) are stable between game runs, addresses are not, so
    // they are the key for the user's own labels (saved next to the DLL, WheelmanMod_vehicle_labels.txt).
    // body = name index of the object at def+0x34 (the vehicle body/model); several definitions share one body and
    // differ only in the variant object at +0x3C (skin/tuning), which is why different entries can look identical.
    struct DefInfo { uintptr_t def; uint32_t nameIdx, outer, family, body; bool populated; };
    const char* LabelFor(uint32_t nameIdx, uint32_t family);          // "" if none
    void SetLabel(uint32_t nameIdx, uint32_t family, const char* text);   // empty text removes the label
    bool LabelPos(uint32_t nameIdx, uint32_t family, float& x, float& y);  // where the player stood when it was named
    int LabelCount();
    bool LabelAt(int i, uint32_t& nameIdx, uint32_t& family, const char*& text);
    int ScanDefs();                           // rescans memory, returns the count (call on button press, it walks the heap)
    int DefCount();
    bool DefAt(int i, DefInfo& out);
    uintptr_t VehicleClass();                 // class shared by all vehicles (0 if unknown yet)

    struct SeenModel { uintptr_t cls, def; unsigned count; };
    int SeenCount();
    bool SeenAt(int i, SeenModel& out);
    void OnFactorySpawn(uintptr_t factory);   // called from the factory post-spawn hook (game thread)
    bool HasPending();
    void RunPending();               // game thread only
    uintptr_t Factory();             // captured factory instance, 0 until the game has spawned something itself

    // Catalog of every vehicle template found in the game's Transitional_*.xxx packages (VehicleCatalog.inc). The spawn
    // request loads the carrying package if the template is not resident yet, then spawns it through the factory.
    // "special" = train / helicopter / trailer rigs, whose class or coupling differs from a plain car.
    struct CatalogInfo { const char* pkg; const char* path; bool special; const char* name; };   // name = short display name
    int CatalogCount();
    bool CatalogAt(int i, CatalogInfo& out);
    int CatalogSorted(int k);                         // catalog index of the k-th entry in display-name order
    bool RequestCatalog(int index, bool loadOnly);   // game-thread work is queued; false if it can't be queued
    extern const char* catalogMessage;
    extern uintptr_t catalogDef;                      // last template object found (0 if none)

    // Vehicles spawned by the mod are not registered with the game's traffic manager, so nothing ever despawns them
    // (and they are never culled by distance). The mod tracks them itself and removes them with UWorld::DestroyActor
    // (0x82DA40, edi = actor, stack: world, bNetForce) on the game thread.
    extern bool autoRemove;            // remove spawned vehicles that are too far away / over the limit
    extern bool keepNear;              // never auto-remove a vehicle within keepNearMeters of the player
    extern int maxSpawned;             // auto-remove the oldest ones beyond this count
    extern float autoRemoveMeters;     // auto-remove vehicles farther than this from the player
    extern float keepNearMeters;
    extern int removedTotal;
    int SpawnedCount();
    void RequestRemoveAll();           // everything spawned by the mod except the car the player sits in
    void RequestRemoveFar();
    bool DestroyActorNow(uintptr_t actor);   // UWorld::DestroyActor on any actor (game thread only)           // only those farther than keepNearMeters

    extern bool allowStubs;        // allow spawning definitions whose data is not loaded (may crash the game)
    extern int status;              // 0 idle, 1 queued, 2 spawned, 3 failed
    extern uintptr_t lastSpawned;
    extern const char* lastMessage;
}

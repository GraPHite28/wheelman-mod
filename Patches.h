#pragma once
#include <cstdint>
#include <Windows.h>

// Auto-detected via the RamBoom hook: [vehicle+0x560]==1 specifically marks
// the player's own car (confirmed - the original CE cheat skips forcing
// health=1 exactly on that flag, to spare the player's own vehicle).
extern void* g_pVehicle;

// Structural sanity check before ANY write into a vehicle object: the pointers we keep can go stale when a
// vehicle is destroyed or streamed out, and writing into a freed pool block corrupts the game's allocator
// (this caused a General Protection Fault in the pool free function at 0x402930). A live UObject starts with a
// vtable inside the exe's .rdata and has -1 at +0x18; a freed pool block has its first dword replaced by a
// free-list pointer. playerCar additionally requires the player-vehicle flag (+0x560 == 1).
bool IsLiveVehicle(const void* v, bool playerCar = false);

extern void* g_pPlayerPawn;   // edi at the Mana hook - reliably player-scoped

// Derived speed for the auto-detected vehicle. Speed isn't a discovered
// field - it's just computed from position deltas between frames, so it needs
// no new offset at all.
struct Vec3 { float x = 0, y = 0, z = 0; };
extern float g_vehicleSpeed; // world units/second

void UpdateVehicleTelemetry(); // call once per frame from the render hook

// Every nearby vehicle the RamBoom hook has seen recently (not just the
// player's) - a capped, oldest-evicted ring buffer used by the ESP overlay.
// Populated regardless of the +0x560 "is my car" flag.
struct TrackedVehicle { void* ptr = nullptr; DWORD lastSeenTick = 0; };
constexpr int kMaxTrackedVehicles = 24;
extern TrackedVehicle g_vehicleList[kMaxTrackedVehicles];
extern int g_vehicleListCount;

// Countdown timers seen by the game's timer tick (see Patches.cpp). Fields on
// each object: +0xEC bit0 running, +0xF4 duration, +0xFC time remaining.
struct TimerRef { void* ptr = nullptr; DWORD lastSeenTick = 0; };
constexpr int kMaxTimers = 32;
extern TimerRef g_timers[kMaxTimers];
extern int g_timerCount;

// Toggle state, read/written from the overlay.
struct PatchState
{
    bool ramBoom        = false; // ramming other cars always deals bonus damage
    bool infiniteMana   = false; // boost/nitro never depletes
    bool noReload       = false; // ammo never drops below 16
    bool freezeTimer    = false; // mission timer stops counting down
    bool vehicleGodMode = false; // force Vehicle_Health to 40000 every frame
};
extern PatchState g_patches;

// Generic "find the stat" memory scanner, modeled on Cheat Engine's classic
// workflow: snapshot a region, take damage, "next scan (decreased value)" to
// narrow candidates down to the real offset. Works on any object pointer, so
// it's reusable beyond player health (money, ammo, whatever else shows up).
// Note: a single decreased-value hit can be a false positive (some unrelated
// value that happened to change at the same moment) - narrow it down over
// several damage events before trusting a candidate.
constexpr int kScanRangeBytes = 0x2000;
constexpr int kMaxScanCandidates = 64;
extern bool g_scanHasBaseline;
extern int g_scanCandidateOffsets[kMaxScanCandidates];
extern int g_scanCandidateCount;
void ScanSnapshot(void* base);
void ScanNarrow(void* base, bool decreased);

// Dedicated before/after diff tool, purpose-built for finding fields that
// change when doing something specific in-game (e.g. repainting in the
// garage): snapshot the player's own vehicle, do the thing, then compare -
// every dword that changed gets logged with its offset. Unlike
// ScanSnapshot/ScanNarrow above (which only track "did this go up or down"),
// this needs no guess about direction, so it works for anything that just
// changes to some other value - like a color.
void SnapshotVehicleForDiff();
void CompareVehicleDiff();

// Installs the byte patches / detours. Verifies original bytes before touching
// anything; any mismatch is skipped (and logged) rather than risking a
// corrupted patch on a different game build.
void InstallGamePatches();
void RemoveGamePatches();

// Applies/reverts individual toggles based on g_patches; call once per frame
// from the render hook, it's cheap (just flips memory protection + a few bytes).
void SyncGamePatches();


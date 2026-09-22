#pragma once
#include <cstdint>

// The game's own developer cheats (class WheelmanCheatManager) and the garages.
//
// The cheat manager is not instantiated in the shipped game, but its native methods only need "this+0x1C" (the Outer =
// the player controller), so they are called directly with a tiny fake object (verified by disassembly, vtable 0x1354DB0):
//   slot 47 KillMe 0x525C60, 48 KillVehicle 0x525D60, 49 FocusGauge 0x525E50                       (thiscall, no arguments)
//   slot 50 VehiclePerformanceUpgrade 0x525EF0, 51 VehicleMeleeMassUpgrade 0x525F60, 52 VehicleHealthUpgrade 0x525FD0
//       (one int argument = level 0..15, callee pops it; they store the level in the player pawn at +0x175C / +0x175D /
//        +0x175E (m_VehicleUpgradeLevels) and refresh the vehicle)
// God / InfiniteAmmo / SlowMo / Weapons are UnrealScript in the game; here they are the equivalent switches:
//   controller +0x514 bit1 bAmmoGodMode (infinite ammo), WorldInfo (actor +0x94) +0x318 TimeDilation (slow motion).
// Garages: class WheelmanGarage (vtable 0x135FF30) derives from WheelmanUnlockable (m_eState at +0x1E4).
namespace BuiltinCheats
{
    extern bool ammoGod;           // controller.bAmmoGodMode
    extern float timeScale;        // 1 = normal; WorldInfo.TimeDilation is forced to this while it is not 1
    extern int perfLevel, meleeLevel, healthLevel;   // vehicle upgrade levels to apply (0..3)
    extern const char* message;

    void Tick();                   // every frame (render thread): switches
    void RequestKillMe();
    void RequestKillVehicle();
    void RequestFocusGauge();
    void RequestUpgrades();        // apply perfLevel / meleeLevel / healthLevel through the game's functions
    bool Pending();
    void RunPending();             // game thread (police pump)
    int CurrentUpgrade(int which); // 0 performance, 1 melee mass, 2 health; -1 if unknown

    // ---- debug functions of WheelmanPlayerPawn / WheelmanPlayerController (all no-argument virtuals unless noted) ----
    // pawn (vtable 0x1354038): +0x684 AddDefaultInventory, +0x688 FailMission, +0x68C PauseMission, +0x690 ResumeMission,
    //   +0x694 EndMission, +0x698 RetryMission, +0x69C EnableMissions, +0x430 Resurrect(FVector loc, FRotator rot, bool onlyIfDead)
    // controller: +0x458 ToggleRadio, +0x464 ChangeRadioStation(bool), +0x468 ToggleBigMap, +0x46C ToggleMap,
    //   +0x47C ToggleTrafficDebugDisplay
    enum Op {
        OpEnableMissions, OpFailMission, OpPauseMission, OpResumeMission, OpEndMission, OpRetryMission,
        OpResurrect, OpToggleRadio, OpNextStation, OpPrevStation, OpToggleMap, OpToggleBigMap, OpTrafficDebug, OpCount
    };
    void Request(Op op);

    // Flags the game's own cheats / debug commands flip; while a switch is on the bit is forced, when it is turned off
    // the bit is cleared again.
    struct FlagInfo { const char* id; const char* label; bool on; };
    enum FlagId {
        FlagControllerGod,        // controller +0x514 b0  bInvincible
        FlagFakeGod,              // controller +0x4EC b4  FakeGodModeEnabled (health kept above fakeGodMinHealth)
        FlagMissionDebug,         // pawn +0x1634 b0       m_bDebugMission
        FlagCarjackDebug, FlagDamageDebug, FlagMeleeBoostDebug, FlagLocDamageDebug, FlagImpulseDebug,
        FlagVehicleHealth, FlagVehicleDamage, FlagVehicleDamageDetail, FlagForceFeedbackDebug,
        FlagCount
    };
    extern bool flags[FlagCount];
    extern int fakeGodMinHealth;  // controller +0x508
    // Anti-fail (experimental): the player pawn's FailMission (vtable slot +0x688, 0x52D1D0) is replaced by a stub that does nothing,
    // so a mission script that fails the mission through that function has no effect. Restored when the option is switched off.
    extern bool antiFailMission;
    extern bool garagesAlwaysOpen;

    // ---- mission / event markers ----
    // WheelmanMissionObjective and its subclasses (events: street race, taxi, rampage ...) are the markers that start a
    // mission or event when the player drives into them. ObjectiveName (an FString) sits at +0x1F0. The game's own
    // pawn Teleport (vtable slot +0xFC, args like Resurrect: FVector, FRotator, bool) puts the player on a marker.
    struct MarkerInfo { uintptr_t obj; const char* kind; float x, y, z; char name[80]; };
    int ScanMissionMarkers();      // heap walk, from a button
    int MarkerCount();
    bool MarkerAt(int i, MarkerInfo& out);
    void RequestTeleportToMarker(int i);

    struct GarageInfo { uintptr_t obj; float x, y, z; int state; bool inUse, missionGarage; };
    int ScanGarages();             // heap walk, call from a button
    int GarageCount();
    bool GarageAt(int i, GarageInfo& out);
    void SetAllGarageStates(int state);
    int UnlockAllGarages();        // state 2 (available) for every garage that is not; scans first if needed
    extern const char* garageMessage;
}

#pragma once
#include <cstdint>

// Per-frame vehicle options for the player's current vehicle (g_pVehicle).
// Offsets on WheelmanVehicle / MwyVehiclePhysicsComponent are derived from the
// Default__ groups (Tools\decompiler\Layout.ps1) and were checked against a
// live car (max health, min health, collision scaling, jack/boost bits, ...):
//   vehicle +0x6C8 bits: b1 BulletProofPanels, b2 BulletProofGlass, b3 BulletProofTyres,
//                        b9 AllowCarToCarJack, b10 AllowOnFootJack
//   vehicle +0x700 bits: b9 EnableFocusPower, b10 EnableCyclone, b12 EnableBoost
//   vehicle +0x68C CollisionDamageScaling, +0xE64/+0xE68 bullet multipliers (engine / fuel tank)
//   vehicle +0x170 -> MwyVehiclePhysicsComponent:
//     +0x1E0 bits (b5 TyresInvulnerable), +0x250 InitalImpactDamage, +0x254 DamageList (TArray)
//   vehicle +0x4D8 m_Simulations[0] -> sim; sim +0xCC m_pPlayerProfile (tuning values)
namespace VehicleMod
{
    extern bool noDeformation;      // crash damage is never applied to the body shape
    extern bool noCollisionDamage;  // collisions don't hurt the health bar
    extern bool bulletproof;        // tyres, glass, panels and engine/fuel tank are bullet proof
    extern bool invulnerableTyres;
    extern bool noBikeFall;         // the player can't be thrown off a motorcycle (vehicle +0x790 / +0x79C thresholds)
    extern bool jackAnywhere;        // car-to-car jack works from any distance (jack detection boxes + jump arc widened)
    extern const char* jackAnywhereStatus;
    extern bool jackIgnoreOccupants;// car-to-car jack ignores the "other passengers" rules (code patch in 0x4B11B0)
    extern const char* jackGateStatus;
    extern bool jackAny;          // car-to-car and on-foot jacking allowed on every tracked vehicle

    extern bool hornOn;             // the game's own StartHorn / StopHorn on the player's vehicle
    extern bool sirenOn;            // StartSiren / StopSiren (siren sound and lights, on vehicles that have a siren)
    extern bool sirenLightsOn;      // StartSirenLights / StopSirenLights
    bool HornSirenPending();
    void RunHornSirenIfQueued();    // game thread

    void Tick();

    // Repair the player's vehicle the way a garage does it (garage tick 0x544E20 -> 0x546BA0): the vehicle's own
    // reset routine (vfunc +0x5C8 = 0x493400, arg 1: resets every part and the simulation, clears the damage state),
    // then health = max health (vehicle +0x628). Runs on the game thread (police tick), see Police.cpp.
    void RequestExit();     // emergency exit: the player's own LeaveVehicle (pawn vfunc +0x620)
    void RequestRepair();
    // Moves a vehicle (and whoever sits in it) with the physics component's SetLocationAndRotation, the call the
    // garage uses; velocity is reset. Game thread. pos = 3 float bit patterns, rot = 3 int rotator values.
    bool TeleportVehicle(uintptr_t vehicle, const uint32_t* pos, const uint32_t* rot);
    void RequestDelete();    // the player leaves the vehicle, then it is removed from the world (UWorld::DestroyActor)
    bool RepairPending();
    void RunRepair();
    extern const char* repairMessage;

    // Player-model tuning profile (shared by every car of that model); 0 if not resolved.
    uintptr_t Profile();

    // Tuning fields. The profile's own "r*" floats are mostly design-time values;
    // the running physics reads the Havok vehicle objects the profile points to
    // (verified live: profile +0xD0 max torque changes nothing, Havok engine +0x14 does).
    //   profile +0x268 hkpVehicleData, +0x26C Aerodynamics, +0x274 Engine,
    //   +0x27C Steering, +0x280 Transmission
    struct TuningField { const char* group; const char* name; int ptrOffset; int offset; float step; };
    extern const TuningField kTuning[];
    extern const int kTuningCount;
    uintptr_t FieldAddress(uintptr_t profile, int index);   // 0 if the pointer chain is invalid
    bool ProfilePlausible(uintptr_t profile);
    // Top speed lock: the engine's speed cap of the player's vehicle (sim object +0x178, m/s) is rebuilt by mini cut-scenes and
// falls back to the game's value; while the lock is on the chosen value is written back every frame.
extern bool topSpeedLock;
extern float topSpeedMs;                 // 0 = take the vehicle's current limit the first time
bool CurrentTopSpeed(float& ms);         // the live limit of the vehicle the player drives
void KeepTopSpeed();                     // per frame (also cheap enough to run while a cut-scene settles)

void ResetTuning();            // restore the values seen when this profile was first opened
}

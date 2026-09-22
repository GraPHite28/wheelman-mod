#pragma once
#include <cstdint>

// The police crime table (class WheelmanPoliceCrimeTable, vtable 0x136ED68; one live object, referenced by the police
// response subsystem's m_pCrimeTable). Layout (derived from the Default__ group, memory verified by the field checks
// in Load()): +0x30 aTimeToLosePolice (TArray<float>, seconds to lose the police per level, -1 = never), then 25
// PoliceCrimeData structs of 12 bytes from +0x3C in the order of the enum PoliceCrimeSpecific, +0x168 fRunAroundTime,
// +0x16C fRunAroundDistance, +0x170 fRunAroundResetTime, +0x174 RunAround (PoliceCrimeData).
// PoliceCrimeData = { bool bSeriousCrime, int iMaxWantedLevel (the crime can raise the level up to this), float
// fWantedLevelChange (heat per event; negative = the level decays) } - the member order inside the 12 bytes is detected.
namespace CrimeTools
{
    constexpr int kCrimes = 26;      // 25 in the array + RunAround

    struct Crime { int serious; int maxLevel; float change; };   // serious is 0 / 1 (an int so it can be saved in configs)

    extern bool overrideOn;          // keep the edited values written into the table
    extern Crime edited[kCrimes];    // the values to write (saved in configs), initialised from the game's own
    extern float changeScale;        // multiplier applied to positive heat changes while overrideOn
    extern float runAroundTime, runAroundDistance, runAroundResetTime;
    extern const char* message;

    bool Load();                     // find the table, remember the original values; false if not found
    bool Loaded();
    const char* Name(int crime);
    const Crime& Original(int crime);
    bool ReadLive(int crime, Crime& out);
    void ResetToOriginal();          // edited = original, and written back
    void Tick();                     // every frame (render thread): applies / releases the overrides
    int TimeToLoseCount();
    float TimeToLose(int level);
    void SetTimeToLose(int level, float seconds);
}

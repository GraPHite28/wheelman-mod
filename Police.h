#pragma once
#include <cstdint>

// Wanted level control. Everything lives on the game's single live
// WheelmanPoliceResponseSubsystem (WheelmanAI.upk), located by memory scan.
// Layout comes from Default__WheelmanPoliceResponseSubsystem groups:
//   +0x30 bits (b0 m_bDebug, b1 m_bEnabled, b2 m_bFixed)
//   +0x54 m_iResponseLevel  +0x58 m_iMaxResponseLevel (5)
//   +0x5C m_fResponsePercent - the real "heat" value; level == floor(percent)
//   +0x60 m_eLostState (0 = wanted/searching, 1 = clean)  +0x61 m_eLastCrime
//
// Writing the level fields directly does NOT work: the game announces a level
// change (HUD stars, Kismet events, police spawning) from inside its own setter
// at 0x5E70B0 - stdcall (subsystem, level, 0, -1). That setter is called from
// the game thread (the Mana hook in Patches.cpp pumps Police_GamePump).
namespace Police
{
    struct State
    {
        bool found = false;
        int level = 0;
        int maxLevel = 0;
        float percent = 0.f;
        int lostState = 0;
        int lastCrime = 0;
        bool enabled = true;
        uintptr_t address = 0;
    };

    extern bool neverWanted;      // keep the wanted level at 0
    extern bool lockLevel;        // keep the level at lockValue
    extern int lockValue;
    extern bool peaceful;         // police neither shoot nor arrest

    void Tick();                  // every frame from the render thread
    void Refresh();               // (re)locate the subsystem now
    State Read();
    void SetLevel(int level);     // one-shot request
    void LoseNow();               // one-shot: level 0
}

#pragma once
#include <cstdint>

// Player weapon tweaks. Pawn.Weapon = pawn+0x37C -> WheelmanWeapon (verified live: vtable 0x013359C0, ammo
// 0x2F0/0x2F4/0x2F8). Spread is the "aim offset angle" system, recoil the recoil fields:
//   +0x364 base aim offset (deg)   +0x368 precision-aim offset   +0x36C max offset
//   +0x370 increase per shot       +0x37C current offset
//   +0x3E0 recoil amount  +0x3E4 precision recoil  +0x3F4 in-car recoil  +0x3F8 current recoil
namespace WeaponMod
{
    extern bool noSpread;
    extern bool noRecoil;
    extern uintptr_t currentWeapon;   // for the UI; 0 when there is no valid weapon

    void Tick();                      // every frame
    bool ReadAim(float& base, float& maxOffset, float& perShot, float& current, float& recoil);
}

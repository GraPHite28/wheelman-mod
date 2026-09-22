#pragma once
#include <cstdint>

// Giving weapons to the player.
//   * The player's InventoryManager is pawn+0x378 (vtable 0x135AFA0, its +0x90 is the pawn, +0x1C4 the inventory chain,
//     each item's +0x1C4 the next one).
//   * CreateInventory(class, bDoNotActivate) is the manager's vfunc +0x2F8 = 0x9CA1A0 (thiscall, callee pops 8): it
//     spawns an actor of the class, hands it to AddInventory (vfunc +0x324) and, unless bDoNotActivate, makes it the
//     current weapon. It needs the weapon's UClass object.
//   * The weapon UClass objects (vtable 0x13A99F8) are all loaded: 15 classes derive from WheelmanWeapon (found through
//     the class of the weapon in the player's hand: its superclass at +0x30 is the base). A class' default object is at
//     UClass+0xCC; its clip size is +0x2F8.
// Names below come from the Wheelman.upk export order / alphabetical name indices (18B7.. and 7894..7898) and were
// cross-checked with the default clip sizes; the label always shows the clip size so a wrong guess is visible.
namespace Weapons
{
    struct Entry { uintptr_t cls; uint32_t nameIdx; int clip; char label[48]; bool unsafe; };   // unsafe = unfinished test weapon: firing it (especially from a car) crashed the game

    int Scan();                            // rescans memory for the weapon classes (a hitch of a fraction of a second)
    int Count();
    bool At(int i, Entry& out);
    bool Give(int index);                  // queue CreateInventory for entry `index`
    bool HasPending();
    void RunPending();                     // game thread only (police tick)
    extern const char* lastMessage;
}

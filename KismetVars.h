#pragma once
#include <cstdint>

// Kismet variables of the running level (SeqVar_Int / SeqVar_Float / SeqVar_Bool). Mission scripts keep their logic in these:
// strike counters, timers, "spotted" flags. Their value sits at +0x84 (verified on the tailing mission MIS017: the strike counter
// dropped 3 -> 2 -> 1 exactly when the target's marker turned yellow; freezing it at 3 stopped Felipe from noticing the player).
// The names of Kismet variables are not readable from memory, so the tool finds them by what they do: take a snapshot, do
// something in the game, then list what changed and lock the variable that matters.
namespace KismetVars
{
    enum Kind { KInt = 0, KFloat = 1, KBool = 2 };
    struct Var { uintptr_t obj; Kind kind; uint32_t value; uint32_t snapshot; bool changed; bool locked; uint32_t lockValue; };

    int Scan();                     // heap walk on demand (about 150 ms), returns the number of variables
    int Count();
    bool At(int i, Var& out);       // current value, snapshot value and the lock state
    void TakeSnapshot();            // remember every current value
    void SetLock(int i, bool on);   // lock: the current value is written back every frame
    void SetValue(int i, uint32_t raw);
    void UnlockAll();
    int LockedCount();
    void Tick();                    // per frame from the render hook (writes the locked values; not while loading)
    extern const char* message;
}

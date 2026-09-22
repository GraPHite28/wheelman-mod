#pragma once

// The engine's own console commands, run without a console window. The game ships the usual Unreal 3 exec handlers
// (SHADOWQUALITY, TOGGLEAO, TOGGLEBPCF, TOGGLEVSM, DRAWDISTANCE, SHOW <flag>, SETRES, STAT, ...) but has no console UI.
// Three objects handle them (found in the exe from the wide command strings; each handler is
//   BOOL Exec(const TCHAR* Cmd, FOutputDevice& Ar), thiscall, callee pops 8):
//   * viewport client  vtable 0x13FC2A8, Exec 0x884B40   SHOW flags, SETRES, STAT, FPS, PRECACHE, VIEWMODE ...
//   * game engine      vtable 0x13FFCB8, Exec 0x8E9F70   DRAWDISTANCE, STREAMLEVELS, PARTICLEMEMORY, MEMORYSPLIT ...
//   * renderer         vtable 0x13C7748, Exec 0x72B920   SHADOWQUALITY, TOGGLEAO, TOGGLEBPCF, TOGGLEVSM, SHADOWRADIUS ...
// The commands are queued and executed in the per-frame game-thread pump (like the other game calls of the mod).
namespace GameConsole
{
    void Run(const char* command);      // queue one command
    bool HasPending();
    void RunPending();                  // game thread only
    const char* Output();               // what the last command printed, plus which handler took it
    extern const char* status;
}

#pragma once
#include <Windows.h>

// Crash reporter. On an unhandled exception it writes a text report (exception, fault module +
// offset, registers, EBP call chain, code pointers found on the stack, the latest first-chance
// faults, the mod's feature toggles and breadcrumbs) and a minidump into <mod dir>\crashes\.
// Both are named WheelmanMod_crash_<date>_<time>.*; the report of the previous crash is shown in
// the Debug > Crash tab.
namespace CrashHandler
{
    void Install(HMODULE self);      // from DllMain (no loader-lock unsafe calls)
    void Refresh();                  // re-take the unhandled-exception slot (call now and then)
    void Note(const char* what);     // breadcrumb: last actions before a crash
    const char* Dir();               // crash folder ("" until Install)

    // First lines of the newest report that existed when the game started.
    int PreviousReport(const char** lines, int maxLines);
    const char* PreviousReportPath();
}

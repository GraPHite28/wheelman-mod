#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "CrashHandler.h"
#include "Aimbot.h"
#include "Npc.h"
#include "NpcMod.h"
#include "Police.h"
#include "VehicleMod.h"
#include "Patches.h"

namespace
{
    char g_dir[MAX_PATH] = {};
    HMODULE g_self = nullptr;
    volatile LONG g_inHandler = 0;
    volatile LONG g_writing = 0;
    void WriteReport(EXCEPTION_POINTERS* ep, const char* kind);
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;   // whatever the game had installed, chained after our report

    // ---- breadcrumbs ----
    struct Crumb { DWORD tick; char text[80]; };
    Crumb g_crumbs[16];
    volatile LONG g_crumbNext = 0;

    // ---- first-chance fault ring (recorded by the vectored handler) ----
    struct Fault { DWORD code; void* addr; DWORD tick; DWORD tid; ULONG_PTR info0, info1; };
    Fault g_faults[16];
    volatile LONG g_faultNext = 0;

    // ---- previous report ----
    char g_prevPath[MAX_PATH] = {};
    char g_prevLines[40][160] = {};
    int g_prevCount = 0;

    const char* CodeName(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN / fail-fast";
        case 0xE06D7363: return "C++ exception";
        default: return "?";
        }
    }

    bool Interesting(DWORD c)
    {
        switch (c)
        {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_DATATYPE_MISALIGNMENT: case 0xC0000374: case 0xC0000409:
            return true;
        }
        return false;
    }

    void Describe(const void* addr, char* out, size_t n)
    {
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &hm) && hm)
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(hm, path, MAX_PATH);
            const char* base = strrchr(path, '\\');
            sprintf_s(out, n, "%s+0x%X", base ? base + 1 : path,
                      static_cast<unsigned>(reinterpret_cast<const BYTE*>(addr) - reinterpret_cast<const BYTE*>(hm)));
        }
        else sprintf_s(out, n, "(unmapped)");
    }

    bool ReadPtr(uintptr_t a, uintptr_t& v)
    {
        __try { v = *reinterpret_cast<uintptr_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool IsCode(uintptr_t a)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
        return mbi.State == MEM_COMMIT &&
               (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep)
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (Interesting(code))
        {
            LONG i = InterlockedIncrement(&g_faultNext) - 1;
            Fault& f = g_faults[i & 15];
            f.code = code; f.addr = ep->ExceptionRecord->ExceptionAddress; f.tick = GetTickCount(); f.tid = GetCurrentThreadId();
            f.info0 = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
            f.info1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;

            // The game handles its own access violations (its "General protection fault" dialog is a
            // caught exception, so the unhandled filter never runs). Faults inside Wheelman.exe itself
            // are not ours to catch - our guarded reads fault inside the mod DLL - so report those
            // immediately, while the faulting stack is still intact.
            if (code == EXCEPTION_ACCESS_VIOLATION)
            {
                const uintptr_t a = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
                static const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
                static DWORD lastReport = 0;
                DWORD now = GetTickCount();
                if (a >= exeBase && a < exeBase + 0x2EB4000 && now - lastReport > 3000)
                {
                    lastReport = now;
                    WriteReport(ep, "first-chance access violation inside Wheelman.exe (the game's own handler shows the GPF dialog)");
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void Out(FILE* f, const char* fmt, ...)
    {
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f);
    }

    void WriteMinidump(EXCEPTION_POINTERS* ep, const char* path)
    {
        HMODULE dbg = LoadLibraryA("dbghelp.dll");
        if (!dbg) return;
        typedef BOOL(WINAPI * Dump_t)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);
        auto dump = reinterpret_cast<Dump_t>(GetProcAddress(dbg, "MiniDumpWriteDump"));
        if (!dump) return;
        HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return;
        struct { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; } info = { GetCurrentThreadId(), ep, FALSE };
        // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithDataSegs
        dump(GetCurrentProcess(), GetCurrentProcessId(), hf, 0x40 | 0x1000 | 0x1, &info, nullptr, nullptr);
        CloseHandle(hf);
    }

    void WriteReport(EXCEPTION_POINTERS* ep, const char* kind)
    {
        if (InterlockedExchange(&g_writing, 1)) return;

        SYSTEMTIME st; GetLocalTime(&st);
        char stamp[64];
        sprintf_s(stamp, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        CreateDirectoryA(g_dir, nullptr);
        char txt[MAX_PATH], dmp[MAX_PATH];
        sprintf_s(txt, "%s\\WheelmanMod_crash_%s.txt", g_dir, stamp);
        sprintf_s(dmp, "%s\\WheelmanMod_crash_%s.dmp", g_dir, stamp);

        FILE* f = nullptr;
        fopen_s(&f, txt, "w");
        if (f)
        {
            const EXCEPTION_RECORD* er = ep->ExceptionRecord;
            const CONTEXT* c = ep->ContextRecord;
            char where[160];
            Describe(er->ExceptionAddress, where, sizeof(where));
            Out(f, "WheelmanMod crash report  %04d-%02d-%02d %02d:%02d:%02d   (%s)", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, kind);
            Out(f, "Exception 0x%08X (%s) at 0x%p  [%s]  thread %lu", er->ExceptionCode, CodeName(er->ExceptionCode), er->ExceptionAddress, where, GetCurrentThreadId());
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                Out(f, "  %s address 0x%p", er->ExceptionInformation[0] == 0 ? "read of" : (er->ExceptionInformation[0] == 1 ? "write to" : "execute at"),
                    reinterpret_cast<void*>(er->ExceptionInformation[1]));
            Out(f, "  origin: %s", strstr(where, "WheelmanMod") ? "INSIDE THE MOD" : "game / system code");

            Out(f, "\nRegisters");
#ifdef _M_IX86
            Out(f, "  EAX=%08X EBX=%08X ECX=%08X EDX=%08X", c->Eax, c->Ebx, c->Ecx, c->Edx);
            Out(f, "  ESI=%08X EDI=%08X EBP=%08X ESP=%08X", c->Esi, c->Edi, c->Ebp, c->Esp);
            Out(f, "  EIP=%08X EFLAGS=%08X", c->Eip, c->EFlags);
            uintptr_t ebp = c->Ebp, esp = c->Esp;
#else
            uintptr_t ebp = 0, esp = 0;
#endif
            Out(f, "\nCall chain (EBP frames)");
            {
                uintptr_t frame = ebp;
                for (int i = 0; i < 32 && frame > 0x10000 && (frame & 3) == 0; ++i)
                {
                    uintptr_t next = 0, ret = 0;
                    if (!ReadPtr(frame, next) || !ReadPtr(frame + 4, ret)) break;
                    char d[160]; Describe(reinterpret_cast<void*>(ret), d, sizeof(d));
                    Out(f, "  #%02d 0x%08X  %s", i, static_cast<unsigned>(ret), d);
                    if (next <= frame) break;
                    frame = next;
                }
            }
            Out(f, "\nCode pointers found on the stack");
            {
                int printed = 0;
                for (int i = 0; i < 384 && printed < 40; ++i)
                {
                    uintptr_t v = 0;
                    if (!ReadPtr(esp + i * 4, v)) break;
                    if (v > 0x10000 && IsCode(v))
                    {
                        char d[160]; Describe(reinterpret_cast<void*>(v), d, sizeof(d));
                        Out(f, "  [esp+0x%03X] 0x%08X  %s", i * 4, static_cast<unsigned>(v), d);
                        ++printed;
                    }
                }
            }
            Out(f, "\nRecent first-chance faults (newest last; faults inside the mod's own guarded reads are normal)");
            {
                LONG n = g_faultNext;
                for (LONG i = (n > 16 ? n - 16 : 0); i < n; ++i)
                {
                    const Fault& ft = g_faults[i & 15];
                    char d[160]; Describe(ft.addr, d, sizeof(d));
                    Out(f, "  t=%lu tid=%lu 0x%08X (%s) at %s%s", ft.tick, ft.tid, ft.code, CodeName(ft.code), d,
                        ft.code == EXCEPTION_ACCESS_VIOLATION ? (ft.info0 == 0 ? " read" : " write") : "");
                }
            }
            Out(f, "\nBreadcrumbs (oldest first, tick = GetTickCount at the time; now = %lu)", GetTickCount());
            {
                LONG n = g_crumbNext;
                for (LONG i = (n > 16 ? n - 16 : 0); i < n; ++i)
                    Out(f, "  t=%lu  %s", g_crumbs[i & 15].tick, g_crumbs[i & 15].text);
            }
            Out(f, "\nMod state");
            Out(f, "  Aimbot: enabled=%d experimental=%d autoShot=%d boneHead=%d", Aimbot::enabled, Aimbot::experimental, Aimbot::autoShot, Aimbot::useBoneHead);
            Out(f, "  Npc scan=%d   NpcMod: disarm=%d ignore=%d allyInv=%d allyHeal=%d", Npc::scanning, NpcMod::keepDisarmed, NpcMod::enemiesIgnore, NpcMod::alliesInvincible, NpcMod::alliesKeepHealth);
            Out(f, "  Police: neverWanted=%d lock=%d(%d) peaceful=%d", Police::neverWanted, Police::lockLevel, Police::lockValue, Police::peaceful);
            Out(f, "  Vehicle: noDeform=%d noCollDmg=%d bulletproof=%d tyres=%d jackAny=%d   g_pVehicle=0x%p", VehicleMod::noDeformation, VehicleMod::noCollisionDamage, VehicleMod::bulletproof, VehicleMod::invulnerableTyres, VehicleMod::jackAny, g_pVehicle);
            Out(f, "  Patches: ramBoom=%d mana=%d noReload=%d freezeTimer=%d vehGod=%d", g_patches.ramBoom, g_patches.infiniteMana, g_patches.noReload, g_patches.freezeTimer, g_patches.vehicleGodMode);
            Out(f, "\nMinidump: %s", dmp);
            fclose(f);
        }
        WriteMinidump(ep, dmp);
        InterlockedExchange(&g_writing, 0);
    }

    LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep)
    {
        if (InterlockedExchange(&g_inHandler, 1)) return EXCEPTION_CONTINUE_SEARCH;
        WriteReport(ep, "unhandled exception");
        if (g_prevFilter) return g_prevFilter(ep);   // the game's own handler still runs after ours
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void LoadPreviousReport()
    {
        char pattern[MAX_PATH];
        sprintf_s(pattern, "%s\\WheelmanMod_crash_*.txt", g_dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        FILETIME best = {}; char bestName[MAX_PATH] = {};
        do
        {
            if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; strcpy_s(bestName, fd.cFileName); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (!bestName[0]) return;
        sprintf_s(g_prevPath, "%s\\%s", g_dir, bestName);
        FILE* f = nullptr;
        fopen_s(&f, g_prevPath, "r");
        if (!f) return;
        while (g_prevCount < 40 && fgets(g_prevLines[g_prevCount], sizeof(g_prevLines[0]), f))
        {
            size_t n = strlen(g_prevLines[g_prevCount]);
            while (n && (g_prevLines[g_prevCount][n - 1] == '\n' || g_prevLines[g_prevCount][n - 1] == '\r')) g_prevLines[g_prevCount][--n] = 0;
            ++g_prevCount;
        }
        fclose(f);
    }
}

namespace CrashHandler
{
    void Install(HMODULE self)
    {
        g_self = self;
        GetModuleFileNameA(self, g_dir, MAX_PATH);
        char* slash = strrchr(g_dir, '\\');
        if (slash) *slash = 0;
        strcat_s(g_dir, "\\crashes");
        LoadPreviousReport();
        AddVectoredExceptionHandler(1, VectoredHandler);
        Refresh();
    }

    void Refresh()
    {
        // The game may install its own filter after ours; take the slot back and chain to it.
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(UnhandledFilter);
        if (prev && prev != UnhandledFilter) g_prevFilter = prev;
    }

    void Note(const char* what)
    {
        LONG i = InterlockedIncrement(&g_crumbNext) - 1;
        Crumb& c = g_crumbs[i & 15];
        c.tick = GetTickCount();
        strncpy_s(c.text, what, _TRUNCATE);
    }

    const char* Dir() { return g_dir; }

    int PreviousReport(const char** lines, int maxLines)
    {
        int n = g_prevCount < maxLines ? g_prevCount : maxLines;
        for (int i = 0; i < n; ++i) lines[i] = g_prevLines[i];
        return n;
    }

    const char* PreviousReportPath() { return g_prevPath; }
}

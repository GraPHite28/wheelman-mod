#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "GameConsole.h"
#include "Log.h"

namespace GameConsole
{
    const char* status = "";

    namespace
    {
        // Exec is a method of the FExec sub-object: the `this` it wants is the object address plus a fixed offset (seen in the
        // handlers: viewport reads its object at this-0x34, the engine one at this-0x30).
        struct Handler { const char* name; uint32_t vtable; uintptr_t exec; uintptr_t thisAdjust; uintptr_t instance; };
        Handler g_handlers[] = {
            // 0x141BAC8 = the game's own viewport client class (a subclass of the 0x13FC2A8 one; the 0x13FC2A8 instance is the
            // base class' default object: SHOW flags flipped on it changed nothing on screen).
            { "viewport",    0x0141BAC8, 0x00884B40, 0x34, 0 },
            { "engine",      0x013FFCB8, 0x008E9F70, 0x30, 0 },
            { "engine base", 0x013FFCB8, 0x0072B920, 0x00, 0 },   // UEngine::Exec: shadows / ambient occlusion / soft shadow filter (reads UEngine fields at +0x2A4)
        };

        // ---- a do-nothing FOutputDevice that keeps the text -----------------------------------------------------
        char g_out[2048] = "";
        volatile long g_pending = 0;
        char g_queue[256] = "";
        char g_report[2304] = "";

        void __fastcall OutDtor(void*, void*, unsigned) {}
        void __fastcall OutSerialize(void*, void*, const wchar_t* text, int)
        {
            __try
            {
                if (!text) return;
                size_t n = strlen(g_out);
                for (; *text && n + 2 < sizeof(g_out); ++text) g_out[n++] = *text < 128 ? static_cast<char>(*text) : '?';
                if (n + 2 < sizeof(g_out)) g_out[n++] = '\n';
                g_out[n] = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        void __fastcall OutFlush(void*, void*) {}
        void* g_outVtable[8] = {};
        struct FakeOutputDevice { void** vtable; unsigned pad[4]; } g_outDevice = { g_outVtable, {} };

        void InitOutputDevice()
        {
            g_outVtable[0] = reinterpret_cast<void*>(&OutDtor);
            g_outVtable[1] = reinterpret_cast<void*>(&OutSerialize);
            for (int i = 2; i < 8; ++i) g_outVtable[i] = reinterpret_cast<void*>(&OutFlush);
        }

        // ---- finding the handler objects ---------------------------------------------------------------------------
        void ScanRegion(uintptr_t start, uintptr_t end)
        {
            __try
            {
                for (uintptr_t p = start; p + 0x20 <= end; p += 4)
                {
                    const uint32_t vt = *reinterpret_cast<const uint32_t*>(p);
                    if (vt < 0x013C0000 || vt > 0x01440000) continue;
                    for (Handler& h : g_handlers)
                        if (!h.instance && vt == h.vtable && *reinterpret_cast<const int32_t*>(p + 0x18) == -1) h.instance = p;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        bool StillValid(const Handler& h)
        {
            __try { return h.instance && *reinterpret_cast<const uint32_t*>(h.instance) == h.vtable && *reinterpret_cast<const int32_t*>(h.instance + 0x18) == -1; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void FindInstances()
        {
            bool need = false;
            for (Handler& h : g_handlers) { if (!StillValid(h)) { h.instance = 0; need = true; } }
            if (!need) return;
            SYSTEM_INFO si; GetSystemInfo(&si);
            uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
            const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
            while (addr < maxAddr)
            {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
                const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100) ScanRegion(start, end);
                addr = end;
            }
        }

        typedef unsigned(__fastcall* ExecFn)(void* self, void* edx, const wchar_t* cmd, void* ar);

        DWORD g_excCode = 0; void* g_excAddr = nullptr; uintptr_t g_excInfo0 = 0, g_excInfo1 = 0;
        int Filter(EXCEPTION_POINTERS* ep)
        {
            g_excCode = ep->ExceptionRecord->ExceptionCode; g_excAddr = ep->ExceptionRecord->ExceptionAddress;
            g_excInfo0 = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
            g_excInfo1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
            return EXCEPTION_EXECUTE_HANDLER;
        }

        unsigned CallExec(const Handler& h, const wchar_t* cmd)
        {
            __try { return reinterpret_cast<ExecFn>(h.exec)(reinterpret_cast<void*>(h.instance + h.thisAdjust), nullptr, cmd, &g_outDevice); }
            __except (Filter(GetExceptionInformation())) { return 0xDEAD; }
        }
    }

    void Run(const char* command)
    {
        if (!command || !*command) return;
        strncpy_s(g_queue, sizeof(g_queue), command, _TRUNCATE);
        InterlockedExchange(&g_pending, 1);
        status = "queued";
    }

    bool HasPending() { return g_pending != 0; }

    void RunPending()
    {
        if (!InterlockedExchange(&g_pending, 0)) return;
        wchar_t wide[256];
        size_t n = 0;
        for (const char* c = g_queue; *c && n + 1 < 256; ++c) wide[n++] = static_cast<unsigned char>(*c);
        wide[n] = 0;
        g_out[0] = 0;
        InitOutputDevice();
        FindInstances();
        const char* took = nullptr; bool crashed = false; char tried[160] = "";
        for (const Handler& h : g_handlers)
        {
            if (!h.instance) continue;
            const unsigned r = CallExec(h, wide);
            if (r == 0xDEAD)
            {
                // one failing handler must not hide the others: remember it and go on
                char part[64]; snprintf(part, sizeof(part), "%s: exception %08lX at %p; ", h.name, g_excCode, g_excAddr);
                strncat_s(tried, sizeof(tried), part, _TRUNCATE);
                continue;
            }
            if (r) { took = h.name; break; }
        }
        if (!took && tried[0]) { strncat_s(g_out, sizeof(g_out), tried, _TRUNCATE); crashed = true; took = "none"; }
        snprintf(g_report, sizeof(g_report), "%s%s%s%s", took ? (crashed ? "handler raised an exception: " : "handled by the ") : "no handler accepted the command",
                 took ? took : "", took && !crashed ? ":\n" : "\n", g_out);
        status = crashed ? "exception" : (took ? "done" : "not a known command");
        if (crashed)
        {
            char extra[160];
            snprintf(extra, sizeof(extra), "exception %08lX at %p (info %p %p)\n", g_excCode, g_excAddr, reinterpret_cast<void*>(g_excInfo0), reinterpret_cast<void*>(g_excInfo1));
            strncat_s(g_report, sizeof(g_report), extra, _TRUNCATE);
            LogF("GameConsole: '%s' -> %s in %s: %s", g_queue, status, took, extra);
        }
        else
            LogF("GameConsole: '%s' -> %s (%s) instances viewport=%X engine=%X renderer=%X out='%s'", g_queue, status, took ? took : "-",
                 static_cast<unsigned>(g_handlers[0].instance), static_cast<unsigned>(g_handlers[1].instance), static_cast<unsigned>(g_handlers[2].instance), g_out);
    }

    const char* Output() { return g_report; }
}

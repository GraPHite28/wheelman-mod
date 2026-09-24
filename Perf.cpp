#include <Windows.h>
#include "Perf.h"
#include "CrashHandler.h"

#pragma comment(lib, "winmm.lib")

namespace Perf
{
    bool limitEnabled = true;
    int  targetFps = 60;
    float measuredFps = 0.f;

    namespace
    {
        LARGE_INTEGER g_freq = {}, g_last = {}, g_lastFrame = {};
        bool g_init = false;
        DWORD g_lastRefresh = 0;
    }

    void Resync()
    {
        if (!g_init) return;
        QueryPerformanceCounter(&g_last);
        g_lastFrame = g_last;
    }

    void Throttle()
    {
        if (!g_init)
        {
            QueryPerformanceFrequency(&g_freq);
            QueryPerformanceCounter(&g_last);
            g_lastFrame = g_last;
            timeBeginPeriod(1);              // 1 ms Sleep granularity
            g_init = true;
            return;
        }

        // Take the crash-filter slot back now and then (the game may replace it).
        DWORD tick = GetTickCount();
        if (tick - g_lastRefresh > 3000) { g_lastRefresh = tick; CrashHandler::Refresh(); }

        if (limitEnabled)
        {
            int fps = targetFps < 20 ? 20 : (targetFps > 240 ? 240 : targetFps);
            const LONGLONG interval = g_freq.QuadPart / fps;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            while (now.QuadPart - g_last.QuadPart < interval)
            {
                LONGLONG remaining = interval - (now.QuadPart - g_last.QuadPart);
                if (remaining > g_freq.QuadPart / 500) Sleep(1);   // more than 2 ms left
                else YieldProcessor();                             // spin the rest
                QueryPerformanceCounter(&now);
            }
            // Keep the cadence, but never try to catch up after a long stall.
            g_last.QuadPart += interval;
            if (now.QuadPart - g_last.QuadPart > interval) g_last = now;
        }
        else
        {
            QueryPerformanceCounter(&g_last);
        }

        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        double dt = static_cast<double>(t.QuadPart - g_lastFrame.QuadPart) / g_freq.QuadPart;
        g_lastFrame = t;
        if (dt > 0.0001)
        {
            float inst = static_cast<float>(1.0 / dt);
            measuredFps = measuredFps <= 0.f ? inst : measuredFps * 0.95f + inst * 0.05f;
        }
    }
}

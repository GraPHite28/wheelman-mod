#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include "Log.h"

namespace
{
    char g_logPath[MAX_PATH] = {};

    const char* LogPath()
    {
        if (g_logPath[0] == '\0')
        {
            char tempDir[MAX_PATH] = {};
            GetTempPathA(MAX_PATH, tempDir);
            sprintf_s(g_logPath, "%sWheelmanMod_log.txt", tempDir);
        }
        return g_logPath;
    }
}

void LogInit()
{
    FILE* f = nullptr;
    fopen_s(&f, LogPath(), "w");
    if (f)
    {
        fprintf(f, "WheelmanMod log started, path=%s\n", LogPath());
        fclose(f);
    }
}

void LogF(const char* fmt, ...)
{
    FILE* f = nullptr;
    fopen_s(&f, LogPath(), "a");
    if (!f) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

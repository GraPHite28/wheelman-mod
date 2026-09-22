#include <Windows.h>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include "Lang.h"

// Defined in imgui\imgui_draw.cpp (WheelmanMod patch).
extern bool (*ImGuiTextTranslateHook)(const char** begin, const char** end);

namespace Lang
{
    int current = EN;
    bool stripTechnical = true;

    namespace
    {
        struct Entry { const char* en; const char* ru; };
        const Entry kRu[] = {
#include "Lang_ru.inc"
        };

        struct Value { const char* text; size_t len; };
        std::unordered_map<std::string_view, Value> g_map;

        // Technical remarks in brackets ("(+0x2AC)", "(m_bIsEnemy)", "(class WheelmanGarage)", "(HUDBase)") are hidden from the
        // user unless the Debug switch is on. The text is copied into a small ring of buffers, the original is untouched.
        bool IsTechnicalGroup(const char* s, size_t n)
        {
            auto starts = [&](const char* p) { const size_t l = strlen(p); return n >= l && strncmp(s, p, l) == 0; };
            if (starts("+0x") || starts("m_") || starts("class ") || starts("\xD0\xBA\xD0\xBB\xD0\xB0\xD1\x81\xD1\x81 ")) return true;                 // "класс "
            if (starts("captures the factory") || starts("\xD0\xB7\xD0\xB0\xD1\x85\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8B\xD0\xB2\xD0\xB0\xD0\xB5\xD1\x82 \xD1\x84\xD0\xB0\xD0\xB1\xD1\x80\xD0\xB8\xD0\xBA\xD1\x83")) return true; // "захватывает фабрику"
            if (n >= 6 && n <= 40)
            {
                bool ident = true, mixed = false;
                for (size_t i = 0; i < n; ++i)
                {
                    const char c = s[i];
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { ident = false; break; }
                    if (i > 0 && ((c >= 'A' && c <= 'Z') || c == '_')) mixed = true;
                }
                if (ident && mixed) return true;     // CamelCase / snake_case identifiers: "HUDBase", "Debug_Mission", "EnableMissions"
            }
            return false;
        }

        bool StripTechnical(const char** begin, const char** end)
        {
            const char* b = *begin; const char* e = *end;
            if (!memchr(b, '(', static_cast<size_t>(e - b))) return false;
            static char ring[4][512]; static int ringPos = 0;
            char* out = ring[ringPos = (ringPos + 1) & 3];
            size_t o = 0; bool changed = false;
            for (const char* p = b; p < e; )
            {
                if (*p == '(')
                {
                    const char* close = static_cast<const char*>(memchr(p + 1, ')', static_cast<size_t>(e - p - 1)));
                    if (close && IsTechnicalGroup(p + 1, static_cast<size_t>(close - p - 1)))
                    {
                        if (o > 0 && out[o - 1] == ' ') --o;         // the space before the bracket goes too
                        p = close + 1; changed = true; continue;
                    }
                }
                if (o + 1 >= sizeof(ring[0])) return false;
                out[o++] = *p++;
            }
            if (!changed) return false;
            *begin = out; *end = out + o;
            return true;
        }

        // Called for every string ImGui measures or draws (hundreds per frame): one hash lookup, no allocation.
        bool Hook(const char** begin, const char** end)
        {
            bool changed = false;
            const char* b = *begin;
            const char* e = *end ? *end : b + strlen(b);
            const size_t n = static_cast<size_t>(e - b);
            if (n == 0 || n > 400) return false;
            if (current == RU)
            {
                auto it = g_map.find(std::string_view(b, n));
                if (it != g_map.end()) { b = it->second.text; e = b + it->second.len; changed = true; }
            }
            if (stripTechnical && StripTechnical(&b, &e)) changed = true;
            if (!changed) return false;
            *begin = b; *end = e;
            return true;
        }
    }

    void Init()
    {
        if (!g_map.empty()) return;
        g_map.reserve(sizeof(kRu) / sizeof(kRu[0]) * 2);
        for (const Entry& e : kRu) g_map[std::string_view(e.en)] = { e.ru, strlen(e.ru) };
        ImGuiTextTranslateHook = &Hook;
    }

    int DetectFromCommandLine()
    {
        const wchar_t* cl = GetCommandLineW();
        if (!cl) return EN;
        const wchar_t* p = wcsstr(cl, L"-language=");
        if (!p) p = wcsstr(cl, L"-LANGUAGE=");
        if (!p) return EN;
        p += 10;
        return (_wcsnicmp(p, L"rus", 3) == 0 || _wcsnicmp(p, L"ru", 2) == 0) ? RU : EN;
    }

    const char* Code() { return current == RU ? "ru" : "en"; }

    bool SetFromCode(const char* code)
    {
        if (!code) return false;
        if (_stricmp(code, "ru") == 0) { current = RU; return true; }
        if (_stricmp(code, "en") == 0) { current = EN; return true; }
        return false;
    }

    const char* Tr(const char* english)
    {
        if (current != RU || !english) return english;
        auto it = g_map.find(std::string_view(english));
        return it == g_map.end() ? english : it->second.text;
    }
}

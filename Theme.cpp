#include <Windows.h>
#include "imgui/imgui.h"
#include <cstdio>
#include "Theme.h"
#include "Binds.h"

namespace Theme
{
    namespace
    {
        ImVec4 C(float r, float g, float b, float a = 1.f) { return ImVec4(r, g, b, a); }
    }

    void Apply()
    {
        ImGuiIO& io = ImGui::GetIO();
        // Window positions and sizes (main menu, speedometer, binds window) are kept between runs in an ini next to the DLL.
        // ImGui opens the file through a UTF-8 -> UTF-16 conversion, but the folder here contains Cyrillic letters and
        // the ANSI path from GetModuleFileNameA is not UTF-8, so the file could never be opened: build the path from
        // the wide API and convert it to UTF-8 explicitly.
        static char iniPath[MAX_PATH * 4];
        wchar_t wpath[MAX_PATH] = {};
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Apply), &hm);
        GetModuleFileNameW(hm, wpath, MAX_PATH);
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) slash[1] = 0; else wpath[0] = 0;
        wcscat_s(wpath, L"WheelmanMod_windows.ini");
        if (WideCharToMultiByte(CP_UTF8, 0, wpath, -1, iniPath, sizeof(iniPath), nullptr, nullptr) > 0)
            io.IniFilename = iniPath;
        else
            io.IniFilename = nullptr;

        // A condensed industrial font if Windows has it (Bahnschrift ships with Windows 10+), else ImGui's default.
        const char* fontPath = "C:\\Windows\\Fonts\\bahnschrift.ttf";
        const bool haveMain = GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES;
        if (haveMain)
            io.Fonts->AddFontFromFileTTF(fontPath, 16.f);
        // Cyrillic (Russian UI): a regular Windows font fills in the glyphs the main font lacks; if the main font is
        // missing it becomes the main font (ImGui's built-in one has no Cyrillic).
        for (const char* fallback : { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\tahoma.ttf" })
        {
            if (GetFileAttributesA(fallback) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig cfg;
            cfg.MergeMode = haveMain;
            io.Fonts->AddFontFromFileTTF(fallback, 16.f, &cfg);
            break;
        }

        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 0.f;
        s.ChildRounding = 0.f;
        s.FrameRounding = 0.f;
        s.PopupRounding = 0.f;
        s.ScrollbarRounding = 0.f;
        s.GrabRounding = 0.f;
        s.TabRounding = 0.f;
        s.WindowBorderSize = 1.f;
        s.ChildBorderSize = 1.f;
        s.FrameBorderSize = 0.f;
        s.PopupBorderSize = 1.f;
        s.WindowPadding = ImVec2(12.f, 10.f);
        s.FramePadding = ImVec2(8.f, 4.f);
        s.ItemSpacing = ImVec2(8.f, 6.f);
        s.ItemInnerSpacing = ImVec2(6.f, 4.f);
        s.ScrollbarSize = 12.f;
        s.GrabMinSize = 10.f;
        s.SeparatorTextBorderSize = 2.f;
        s.SeparatorTextAlign = ImVec2(0.f, 0.5f);
        s.SeparatorTextPadding = ImVec2(10.f, 2.f);
        s.WindowTitleAlign = ImVec2(0.f, 0.5f);
        s.TabBarBorderSize = 2.f;

        const ImVec4 accent = C(1.00f, 0.62f, 0.10f);        // amber
        const ImVec4 accentHi = C(1.00f, 0.72f, 0.26f);
        const ImVec4 accentLo = C(0.78f, 0.44f, 0.04f);
        const ImVec4 accentDim = C(0.42f, 0.24f, 0.03f);
        const ImVec4 bg = C(0.050f, 0.050f, 0.055f, 0.96f);
        const ImVec4 panel = C(0.095f, 0.092f, 0.090f);
        const ImVec4 panelHi = C(0.150f, 0.140f, 0.125f);
        const ImVec4 text = C(0.93f, 0.92f, 0.89f);
        const ImVec4 textDim = C(0.56f, 0.53f, 0.48f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = textDim;
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_ChildBg] = C(0.07f, 0.07f, 0.075f, 0.6f);
        c[ImGuiCol_PopupBg] = C(0.07f, 0.07f, 0.075f, 0.98f);
        c[ImGuiCol_Border] = accentDim;
        c[ImGuiCol_BorderShadow] = C(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = panel;
        c[ImGuiCol_FrameBgHovered] = panelHi;
        c[ImGuiCol_FrameBgActive] = C(0.20f, 0.17f, 0.12f);
        c[ImGuiCol_TitleBg] = C(0.09f, 0.06f, 0.02f);
        c[ImGuiCol_TitleBgActive] = accentLo;
        c[ImGuiCol_TitleBgCollapsed] = C(0.09f, 0.06f, 0.02f, 0.8f);
        c[ImGuiCol_MenuBarBg] = panel;
        c[ImGuiCol_ScrollbarBg] = C(0.03f, 0.03f, 0.035f, 0.8f);
        c[ImGuiCol_ScrollbarGrab] = accentDim;
        c[ImGuiCol_ScrollbarGrabHovered] = accentLo;
        c[ImGuiCol_ScrollbarGrabActive] = accent;
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accentHi;
        c[ImGuiCol_Button] = C(0.17f, 0.12f, 0.06f);
        c[ImGuiCol_ButtonHovered] = accentLo;
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_Header] = C(0.30f, 0.18f, 0.05f);
        c[ImGuiCol_HeaderHovered] = accentLo;
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Separator] = accentDim;
        c[ImGuiCol_SeparatorHovered] = accentLo;
        c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_ResizeGrip] = accentDim;
        c[ImGuiCol_ResizeGripHovered] = accentLo;
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = C(0.11f, 0.09f, 0.07f);
        c[ImGuiCol_TabHovered] = accentLo;
        c[ImGuiCol_TabSelected] = C(0.62f, 0.35f, 0.04f);
        c[ImGuiCol_TabDimmed] = C(0.09f, 0.08f, 0.07f);
        c[ImGuiCol_TabDimmedSelected] = C(0.36f, 0.21f, 0.04f);
        c[ImGuiCol_TabSelectedOverline] = accentHi;
        c[ImGuiCol_TabDimmedSelectedOverline] = accent;
        c[ImGuiCol_TableHeaderBg] = C(0.16f, 0.10f, 0.03f);
        c[ImGuiCol_TableBorderStrong] = accentDim;
        c[ImGuiCol_TableBorderLight] = C(0.20f, 0.13f, 0.05f);
        c[ImGuiCol_TableRowBg] = C(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt] = C(1, 1, 1, 0.03f);
        c[ImGuiCol_TextSelectedBg] = C(1.00f, 0.62f, 0.10f, 0.35f);
        c[ImGuiCol_DragDropTarget] = accentHi;
        c[ImGuiCol_NavHighlight] = accent;
        c[ImGuiCol_PlotLines] = accent;
        c[ImGuiCol_PlotHistogram] = accent;
    }
}

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "imgui/imgui.h"
#include "Binds.h"
#include "Lang.h"
#include "Log.h"

namespace Binds
{
    int menuKey = VK_INSERT;
    int cursorKey = VK_END;
    bool showWindow = true;
    bool showAll = false;
    char autoloadConfig[64] = "";
    const char* lastMessage = "";

    namespace
    {
        struct Feature { std::string id, label, group; bool* flag = nullptr; void (*action)() = nullptr; };
        struct NumVar { std::string id; float* f = nullptr; int* i = nullptr; };
        struct Bind { int feature = -1; int vk = 0; int mode = Toggle; bool prevDown = false; };

        std::vector<Feature> g_features;
        std::vector<NumVar> g_nums;
        std::vector<Bind> g_binds;

        int g_capture = -1;                 // feature waiting for a key (-1 = none)
        int g_sysCapture = 0;               // system key waiting for a key (1 menu, 2 cursor)
        bool g_captureFromSettings = false; // started from the Settings tab (no popup to watch)
        bool g_capturePrev[256] = {};

        const ImVec4 kAccent(1.00f, 0.66f, 0.16f, 1.f);

        int FindById(const char* id)
        {
            for (size_t i = 0; i < g_features.size(); ++i) if (g_features[i].id == id) return static_cast<int>(i);
            return -1;
        }
        int FindByFlag(const bool* flag)
        {
            if (!flag) return -1;
            for (size_t i = 0; i < g_features.size(); ++i) if (g_features[i].flag == flag) return static_cast<int>(i);
            return -1;
        }
        Bind* FindBind(int feature)
        {
            for (Bind& b : g_binds) if (b.feature == feature) return &b;
            return nullptr;
        }
        void RemoveBind(int feature)
        {
            for (size_t i = 0; i < g_binds.size(); ++i)
                if (g_binds[i].feature == feature) { g_binds.erase(g_binds.begin() + i); return; }
        }
        void SetBind(int feature, int vk, int mode)
        {
            if (Bind* b = FindBind(feature)) { b->vk = vk; return; }
            Bind b; b.feature = feature; b.vk = vk; b.mode = mode;
            g_binds.push_back(b);
        }

        const char* KeyName(int vk)
        {
            static char buf[48];
            switch (vk)
            {
            case VK_MBUTTON: return "Mouse 3";
            case VK_XBUTTON1: return "Mouse 4";
            case VK_XBUTTON2: return "Mouse 5";
            case VK_LBUTTON: return "Mouse 1";
            case VK_RBUTTON: return "Mouse 2";
            }
            const UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            LONG lparam = static_cast<LONG>(sc << 16);
            switch (vk)
            {
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
            case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_NUMLOCK:
                lparam |= 1 << 24; break;
            }
            if (GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0) return buf;
            snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
            return buf;
        }

        void StartCapture(int feature, bool fromSettings)
        {
            g_capture = feature;
            g_captureFromSettings = fromSettings;
            for (int k = 0; k < 256; ++k) g_capturePrev[k] = (GetAsyncKeyState(k) & 0x8000) != 0;   // ignore what is already down
        }

        void BindMenu(int feature)
        {
            if (feature < 0) return;
            if (ImGui::BeginPopupContextItem())
            {
                const Feature& f = g_features[feature];
                ImGui::TextUnformatted(f.label.c_str());
                ImGui::Separator();
                Bind* b = FindBind(feature);
                if (g_capture == feature) ImGui::TextColored(kAccent, "Press a key or mouse button...  (Esc = cancel)");
                else
                {
                    char btn[80];
                    snprintf(btn, sizeof(btn), "Key: %s##bindkey", b ? KeyName(b->vk) : "not set");
                    if (ImGui::Button(btn, ImVec2(240, 0))) StartCapture(feature, false);
                }
                if (b && f.flag)
                {
                    if (ImGui::RadioButton("Toggle", b->mode == Toggle)) b->mode = Toggle;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Hold", b->mode == Hold)) b->mode = Hold;
                }
                else if (b) ImGui::TextDisabled("action: runs on every press");
                if (b && ImGui::Button("Clear bind", ImVec2(240, 0))) RemoveBind(feature);
                ImGui::EndPopup();
            }
            else if (g_capture == feature && !g_captureFromSettings) g_capture = -1;   // popup closed: stop listening
        }

        void Marker(int feature)
        {
            if (feature < 0) return;
            const Bind* b = FindBind(feature);
            if (!b) return;
            ImGui::SameLine();
            ImGui::TextColored(kAccent, b->mode == Hold && g_features[feature].flag ? "[%s hold]" : "[%s]", KeyName(b->vk));
        }

        bool GameFocused()
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId();
        }

        std::string Sanitize(const char* name)
        {
            std::string s;
            for (const char* p = name; *p; ++p)
                if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == ' ')
                    s += *p;
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        }

        std::string DllDir()
        {
            static std::string dir;
            if (dir.empty())
            {
                HMODULE hm = nullptr;
                char path[MAX_PATH] = {};
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&DllDir), &hm);
                GetModuleFileNameA(hm, path, MAX_PATH);
                char* slash = strrchr(path, '\\');
                if (slash) slash[1] = 0; else path[0] = 0;
                dir = path;
            }
            return dir;
        }

        std::string ConfigPath(const std::string& name) { return std::string(ConfigDir()) + name + ".cfg"; }
        std::string SettingsPath() { return DllDir() + "WheelmanMod_settings.txt"; }
    }

    // ---- registry ---------------------------------------------------------
    void RegisterToggle(const char* id, const char* label, const char* group, bool* flag)
    {
        Feature f; f.id = id; f.label = label; f.group = group; f.flag = flag;
        g_features.push_back(f);
    }
    void RegisterAction(const char* id, const char* label, const char* group, void (*fn)())
    {
        Feature f; f.id = id; f.label = label; f.group = group; f.action = fn;
        g_features.push_back(f);
    }
    void RegisterFloat(const char* id, float* value) { NumVar n; n.id = id; n.f = value; g_nums.push_back(n); }
    void RegisterInt(const char* id, int* value) { NumVar n; n.id = id; n.i = value; g_nums.push_back(n); }

    // ---- UI helpers --------------------------------------------------------
    bool Check(const char* label, bool* value)
    {
        const bool changed = ImGui::Checkbox(label, value);
        const int fi = FindByFlag(value);
        if (fi >= 0) { BindMenu(fi); Marker(fi); }
        return changed;
    }

    bool Action(const char* label, const char* id)
    {
        const bool pressed = ImGui::Button(label);
        const int fi = FindById(id);
        if (fi >= 0)
        {
            if (pressed && g_features[fi].action) g_features[fi].action();
            BindMenu(fi);
            Marker(fi);
        }
        return pressed;
    }

    // ---- per frame ---------------------------------------------------------
    const char* KeyNameOf(int vk) { return KeyName(vk); }
    void BeginSystemKeyCapture(int which) { g_sysCapture = which; for (int k = 0; k < 256; ++k) g_capturePrev[k] = (GetAsyncKeyState(k) & 0x8000) != 0; }
    int SystemKeyCapturing() { return g_sysCapture; }

    void Tick()
    {
        if (g_sysCapture && GameFocused())
        {
            for (int vk = 3; vk < 256; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
                const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down && !g_capturePrev[vk])
                {
                    if (vk != VK_ESCAPE) { (g_sysCapture == 1 ? menuKey : cursorKey) = vk; SaveSettings(); }
                    g_sysCapture = 0;
                    return;
                }
                g_capturePrev[vk] = down;
            }
            return;
        }
        if (!GameFocused())
        {
            // Hold binds must not stay on when the window loses focus while the key is down.
            for (Bind& b : g_binds)
                if (b.prevDown && b.mode == Hold && b.feature >= 0 && g_features[b.feature].flag) { *g_features[b.feature].flag = false; b.prevDown = false; }
            return;
        }

        if (g_capture >= 0)
        {
            for (int vk = 3; vk < 256; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;   // generic modifiers: the L/R variants are bindable
                const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down && !g_capturePrev[vk])
                {
                    if (vk == VK_ESCAPE) { g_capture = -1; return; }
                    if (g_capture < static_cast<int>(g_features.size()))
                    {
                        SetBind(g_capture, vk, Toggle);
                        if (Bind* b = FindBind(g_capture)) b->prevDown = true;   // the press that set it must not fire it
                    }
                    g_capture = -1;
                    return;
                }
                g_capturePrev[vk] = down;
            }
            return;
        }

        if (ImGui::GetIO().WantTextInput) return;

        for (Bind& b : g_binds)
        {
            if (b.feature < 0 || b.feature >= static_cast<int>(g_features.size())) continue;
            const Feature& f = g_features[b.feature];
            const bool down = (GetAsyncKeyState(b.vk) & 0x8000) != 0;
            if (f.action)
            {
                if (down && !b.prevDown) f.action();
            }
            else if (f.flag)
            {
                if (b.mode == Hold) *f.flag = down;
                else if (down && !b.prevDown) *f.flag = !*f.flag;
            }
            b.prevDown = down;
        }
    }

    void DrawWindow(bool menuOpen)
    {
        if (!showWindow) return;

        struct Row { const Feature* f; const Bind* b; bool on; };
        std::vector<Row> rows;
        for (const Bind& b : g_binds)
        {
            if (b.feature < 0 || b.feature >= static_cast<int>(g_features.size())) continue;
            const Feature& f = g_features[b.feature];
            const bool on = f.flag ? *f.flag : false;
            if (showAll || on) rows.push_back({ &f, &b, on });
        }
        if (rows.empty() && !menuOpen) return;

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 250.f, 40.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(190.f, 0.f), ImVec2(600.f, 800.f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;   // position is saved
        ImGui::Begin("BINDS", nullptr, flags);
        if (rows.empty()) ImGui::TextDisabled("no active binds");
        for (const Row& r : rows)
        {
            const char* key = KeyName(r.b->vk);
            if (r.f->action)
                ImGui::TextDisabled("%s  [%s]  %s", TR(r.f->label.c_str()), key, TR("action"));
            else if (r.on)
                ImGui::TextColored(kAccent, "%s  [%s%s]", TR(r.f->label.c_str()), key, r.b->mode == Hold ? TR(" hold") : "");
            else
                ImGui::TextDisabled("%s  [%s%s]  false", TR(r.f->label.c_str()), key, r.b->mode == Hold ? TR(" hold") : "");
        }
        if (menuOpen)
        {
            ImGui::Separator();
            if (ImGui::Checkbox("show all binds", &showAll)) SaveSettings();
        }
        ImGui::End();
    }

    void DrawSettingsBinds()
    {
        if (g_capture >= 0 && g_captureFromSettings && g_capture < static_cast<int>(g_features.size()))
            ImGui::TextColored(kAccent, TR("Press a key or mouse button for \"%s\"...  (Esc = cancel)"), TR(g_features[g_capture].label.c_str()));

        // Add a bind for any registered feature.
        static int addPick = 0;
        if (addPick >= static_cast<int>(g_features.size())) addPick = 0;
        if (!g_features.empty())
        {
            ImGui::SetNextItemWidth(300);
            const Feature& cur = g_features[addPick];
            char curLabel[160];
            snprintf(curLabel, sizeof(curLabel), "%s / %s", TR(cur.group.c_str()), TR(cur.label.c_str()));
            if (ImGui::BeginCombo("Function", curLabel))
            {
                for (int i = 0; i < static_cast<int>(g_features.size()); ++i)
                {
                    char l[160];
                    snprintf(l, sizeof(l), "%s / %s", TR(g_features[i].group.c_str()), TR(g_features[i].label.c_str()));
                    ImGui::PushID(i); if (ImGui::Selectable(l, i == addPick)) addPick = i; if (i == addPick) ImGui::SetItemDefaultFocus(); ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Bind a key...")) StartCapture(addPick, true);
        }
        ImGui::TextDisabled("You can also right-click any option in the menu to bind it.");

        if (g_binds.empty()) { ImGui::TextDisabled("(no binds yet)"); return; }
        if (ImGui::BeginTable("bindtable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 26.f);
            ImGui::TableHeadersRow();
            int remove = -1;
            for (size_t i = 0; i < g_binds.size(); ++i)
            {
                Bind& b = g_binds[i];
                if (b.feature < 0 || b.feature >= static_cast<int>(g_features.size())) continue;
                const Feature& f = g_features[b.feature];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f.label.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(KeyName(b.vk));
                ImGui::TableNextColumn();
                if (f.flag)
                {
                    if (ImGui::RadioButton("Toggle", b.mode == Toggle)) b.mode = Toggle;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Hold", b.mode == Hold)) b.mode = Hold;
                }
                else ImGui::TextDisabled("action");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("X")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (remove >= 0) g_binds.erase(g_binds.begin() + remove);
        }
    }

    // ---- configs -----------------------------------------------------------
    const char* DataDir() { return DllDir().c_str(); }

    const char* ConfigDir()
    {
        static std::string dir;
        if (dir.empty()) { dir = DllDir() + "configs\\"; CreateDirectoryA(dir.c_str(), nullptr); }
        return dir.c_str();
    }

    std::vector<std::string> ListConfigs()
    {
        std::vector<std::string> out;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((std::string(ConfigDir()) + "*.cfg").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do
        {
            std::string n = fd.cFileName;
            if (n.size() > 4) out.push_back(n.substr(0, n.size() - 4));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return out;
    }

    bool SaveConfig(const char* rawName)
    {
        const std::string name = Sanitize(rawName);
        if (name.empty()) { lastMessage = "enter a config name (letters, digits, space, _ -)"; return false; }
        FILE* f = nullptr;
        fopen_s(&f, ConfigPath(name).c_str(), "w");
        if (!f) { lastMessage = "can't write the config file"; return false; }
        fprintf(f, "WheelmanMod config\n");
        for (const Feature& ft : g_features)
            if (ft.flag) fprintf(f, "T\t%s\t%d\n", ft.id.c_str(), *ft.flag ? 1 : 0);
        for (const NumVar& n : g_nums)
        {
            if (n.f) fprintf(f, "F\t%s\t%.6g\n", n.id.c_str(), *n.f);
            if (n.i) fprintf(f, "I\t%s\t%d\n", n.id.c_str(), *n.i);
        }
        for (const Bind& b : g_binds)
            if (b.feature >= 0 && b.feature < static_cast<int>(g_features.size()))
                fprintf(f, "B\t%s\t%d\t%d\n", g_features[b.feature].id.c_str(), b.vk, b.mode);
        fclose(f);
        lastMessage = "saved";
        return true;
    }

    bool LoadConfig(const char* rawName)
    {
        const std::string name = Sanitize(rawName);
        FILE* f = nullptr;
        fopen_s(&f, ConfigPath(name).c_str(), "r");
        if (!f) { lastMessage = "config not found"; return false; }
        std::vector<Bind> binds;
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            for (char* c = line; *c; ++c) if (*c == '\n' || *c == '\r') *c = 0;
            char* tok[4] = {};
            int n = 0;
            for (char* p = line; n < 4;)
            {
                tok[n++] = p;
                char* tab = strchr(p, '\t');
                if (!tab) break;
                *tab = 0;
                p = tab + 1;
            }
            if (n < 3 || !tok[0][0]) continue;
            const char kind = tok[0][0];
            if (kind == 'T')
            {
                const int fi = FindById(tok[1]);
                if (fi >= 0 && g_features[fi].flag) *g_features[fi].flag = atoi(tok[2]) != 0;
            }
            else if (kind == 'F' || kind == 'I')
            {
                for (NumVar& v : g_nums)
                    if (v.id == tok[1])
                    {
                        if (kind == 'F' && v.f) *v.f = static_cast<float>(atof(tok[2]));
                        if (kind == 'I' && v.i) *v.i = atoi(tok[2]);
                    }
            }
            else if (kind == 'B' && n >= 4)
            {
                const int fi = FindById(tok[1]);
                if (fi >= 0)
                {
                    Bind b; b.feature = fi; b.vk = atoi(tok[2]); b.mode = atoi(tok[3]) == Hold ? Hold : Toggle;
                    if (b.vk > 0 && b.vk < 256) binds.push_back(b);
                }
            }
        }
        fclose(f);
        g_binds = binds;
        lastMessage = "loaded";
        return true;
    }

    bool DeleteConfig(const char* rawName)
    {
        const std::string name = Sanitize(rawName);
        if (name.empty()) return false;
        const bool ok = DeleteFileA(ConfigPath(name).c_str()) != 0;
        lastMessage = ok ? "deleted" : "can't delete";
        return ok;
    }

    void SaveSettings()
    {
        FILE* f = nullptr;
        fopen_s(&f, SettingsPath().c_str(), "w");
        if (!f) return;
        fprintf(f, "showWindow=%d\nshowAll=%d\nautoload=%s\nlanguage=%s\nmenukey=%d\ncursorkey=%d\n", showWindow ? 1 : 0, showAll ? 1 : 0, autoloadConfig, Lang::Code(), menuKey, cursorKey);
        fclose(f);
    }

    void Init()
    {
        Lang::Init();
        bool haveLanguage = false;
        FILE* f = nullptr;
        fopen_s(&f, SettingsPath().c_str(), "r");
        if (f)
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                for (char* c = line; *c; ++c) if (*c == '\n' || *c == '\r') *c = 0;
                if (strncmp(line, "showWindow=", 11) == 0) showWindow = atoi(line + 11) != 0;
                else if (strncmp(line, "showAll=", 8) == 0) showAll = atoi(line + 8) != 0;
                else if (strncmp(line, "autoload=", 9) == 0) strncpy_s(autoloadConfig, line + 9, _TRUNCATE);
                else if (strncmp(line, "language=", 9) == 0) haveLanguage = Lang::SetFromCode(line + 9);
                else if (strncmp(line, "menukey=", 8) == 0) { const int v = atoi(line + 8); if (v > 2 && v < 256) menuKey = v; }
                else if (strncmp(line, "cursorkey=", 10) == 0) { const int v = atoi(line + 10); if (v > 2 && v < 256) cursorKey = v; }
            }
            fclose(f);
        }
        // First start: no saved language yet - take it from the game's -language= argument (set by the launcher).
        if (!haveLanguage) { Lang::current = Lang::DetectFromCommandLine(); SaveSettings(); }
        // A config called "default" is loaded automatically unless another one was chosen for auto-load.
        if (autoloadConfig[0]) LoadConfig(autoloadConfig);
        else
        {
            FILE* d = nullptr;
            fopen_s(&d, ConfigPath("default").c_str(), "r");
            if (d) { fclose(d); LoadConfig("default"); }
        }
    }
}

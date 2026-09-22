#pragma once
#include <string>
#include <vector>

// Feature registry, key binds, the small "active binds" window and the config files.
//
// Every option that can be bound (a bool toggle or a one-shot action) is registered once at start-up
// (Overlay::InitFeatures). The UI draws it through Binds::Check / Binds::Action, which add a right-click menu:
//   Key: <press a key>   Mode: Toggle | Hold   Clear
// A bind is one key (keyboard or the extra mouse buttons) for one feature:
//   Toggle - each press flips the option (actions run on every press),
//   Hold   - the option is on while the key is held and off otherwise.
namespace Binds
{
    enum Mode { Toggle = 0, Hold = 1 };

    // ---- registry ---------------------------------------------------------
    void RegisterToggle(const char* id, const char* label, const char* group, bool* flag);
    void RegisterAction(const char* id, const char* label, const char* group, void (*fn)());
    void RegisterFloat(const char* id, float* value);   // saved in configs, no bind
    void RegisterInt(const char* id, int* value);

    // ---- UI helpers --------------------------------------------------------
    // Like ImGui::Checkbox / ImGui::Button, plus the bind menu when the target is registered.
    bool Check(const char* label, bool* value);
    bool Action(const char* label, const char* id);      // runs the registered action when pressed

    // ---- per frame ---------------------------------------------------------
    void Init();                       // after the features are registered: loads the settings and the auto-load config
    void Tick();                       // reads the keys, applies the binds
    void DrawWindow(bool menuOpen);    // the small active-binds window
    void DrawSettingsBinds();          // table of all binds (Settings tab)

    // ---- system keys (Settings -> Hotkeys); saved in WheelmanMod_settings.txt ----
    extern int menuKey;                // opens / closes the menu (default Insert)
    extern int cursorKey;              // captures / releases the mouse for the overlay (default End; Home is used by the game)
    const char* KeyNameOf(int vk);
    void BeginSystemKeyCapture(int which);   // 1 = menu key, 2 = cursor key: the next key press is taken
    int SystemKeyCapturing();                // 0 when idle

    extern bool showWindow;            // show the binds window
    extern bool showAll;               // also list inactive binds (marked "false")

    // ---- configs -----------------------------------------------------------
    const char* DataDir();             // folder of the DLL (settings, window layout and configs live here)
    const char* ConfigDir();
    std::vector<std::string> ListConfigs();
    bool SaveConfig(const char* name);
    bool LoadConfig(const char* name);
    bool DeleteConfig(const char* name);
    extern char autoloadConfig[64];    // config loaded at start ("" = none)
    void SaveSettings();               // window options + auto-load name (WheelmanMod_settings.txt)
    extern const char* lastMessage;
}

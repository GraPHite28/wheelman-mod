#pragma once

// UI language (English / Russian).
//
// Translation happens where ImGui draws and measures text (hook in imgui_draw.cpp, ImGuiTextTranslateHook): every
// string that exactly matches an English entry of Lang_ru.inc is replaced by its Russian text. Widget IDs are hashed
// from the English label, so configs, binds and window layout are unaffected by the language.
// printf-style format strings can't be matched after formatting, so their call sites wrap the format in TR().
// New UI text: nothing to do for English; for Russian add a pair to Lang_ru.inc.
//
// The language is stored in WheelmanMod_settings.txt. On the very first start (no setting yet) it is taken from the
// game's -language= argument, which the launcher passes (rus -> Russian, anything else -> English).
namespace Lang
{
    enum { EN = 0, RU = 1 };

    extern int current;
    extern bool stripTechnical;          // hide technical bracket remarks like "(+0x2AC)" / "(m_bIsEnemy)" (true unless the Debug switch is on)
    void Init();                         // builds the dictionary and installs the ImGui hook
    int DetectFromCommandLine();         // EN / RU from the process command line (-language=rus)
    const char* Code();                  // "en" / "ru"
    bool SetFromCode(const char* code);  // false if unknown
    const char* Tr(const char* english); // the Russian text of a format string / label (or the input itself)
}

#define TR(s) Lang::Tr(s)

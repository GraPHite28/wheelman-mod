// Wheelman Mod Launcher - a small Win32 GUI that starts Wheelman.exe suspended, injects the mod DLL and resumes it.
// Same injection procedure as Injector.exe, plus: game language (Russian / English), game exe path, mod DLL path.
// Settings are stored in Launcher.ini next to this exe (UTF-16, so Cyrillic paths survive).
#include <Windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <cstdarg>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    enum Lang { RU = 0, EN = 1 };

    enum Ctl
    {
        IDC_LANG_LABEL = 100, IDC_LANG, IDC_EXE_LABEL, IDC_EXE, IDC_EXE_BROWSE, IDC_DLL_LABEL, IDC_DLL, IDC_DLL_BROWSE,
        IDC_ARGS_LABEL, IDC_ARGS, IDC_CLOSE, IDC_LAUNCH, IDC_LOG, IDC_MAPDATA, IDC_LANG_NOTE
    };

    enum Str
    {
        S_TITLE, S_LANG, S_EXE, S_DLL, S_ARGS, S_BROWSE, S_CLOSE, S_LAUNCH, S_FILTER_EXE, S_FILTER_DLL, S_PICK_EXE, S_PICK_DLL,
        S_ERR_NOEXE, S_ERR_NODLL, S_ERR_NOT32_EXE, S_ERR_NOT32_DLL, S_STARTING, S_CREATE_FAIL, S_INJECT_FAIL, S_LOAD_FAIL,
        S_DONE, S_ARGS_HINT, S_FOUND_EXE,
        S_MAPDATA, S_MAPDATA_NOBUILDER, S_MAPDATA_START, S_MAPDATA_OK, S_MAPDATA_FAIL, S_MAPDATA_MISSING, S_LANG_NOTE,
        S_COUNT
    };

    const wchar_t* const kText[2][S_COUNT] = {
        { // Russian
            L"Wheelman Mod - загрузчик",
            L"Язык игры:",
            L"Путь к Wheelman.exe:",
            L"Путь к DLL мода (WheelmanMod.dll):",
            L"Дополнительные параметры игры (необязательно):",
            L"Обзор...",
            L"Закрыть загрузчик после успешного запуска",
            L"Запустить игру с модом",
            L"Wheelman.exe\0Wheelman.exe\0Программы (*.exe)\0*.exe\0Все файлы\0*.*\0",
            L"WheelmanMod.dll\0WheelmanMod*.dll\0Библиотеки (*.dll)\0*.dll\0Все файлы\0*.*\0",
            L"Выберите Wheelman.exe",
            L"Выберите DLL мода",
            L"Не найден файл игры: %s",
            L"Не найдена DLL мода: %s",
            L"Wheelman.exe должен быть 32-битным (x86): %s",
            L"DLL мода должна быть 32-битной (x86): %s",
            L"Запуск: %s",
            L"Не удалось запустить игру (ошибка %lu).",
            L"Не удалось внедрить DLL (ошибка %lu, %s). Процесс игры завершён.",
            L"LoadLibrary вернул NULL - DLL не загрузилась (проверьте Visual C++ Redistributable x86 и путь). Процесс игры завершён.",
            L"Готово: мод загружен (0x%08X). В игре нажмите Insert, чтобы открыть меню.",
            L"Например: -windowed",
            L"Найдена игра: %s",
            L"Подготовить карту и иконки из моей игры",
            L"Не найден MapDataBuilder.exe (ожидается рядом с загрузчиком, в папке tools). Он входит в релиз мода.",
            L"Подготовка данных карты из ваших файлов игры (это займёт около минуты)...",
            L"Готово: карта города, иконки и маска дорог созданы в %s.",
            L"Не удалось подготовить данные карты (код %lu). Смотрите сообщения выше.",
            L"Данные карты (MapData) не найдены рядом с DLL: внешняя миникарта будет без картинки города и иконок. Нажмите «Подготовить карту и иконки из моей игры».",
            L"Английская версия игры (-language=int) не проверялась."
        },
        { // English
            L"Wheelman Mod - Launcher",
            L"Game language:",
            L"Path to Wheelman.exe:",
            L"Path to the mod DLL (WheelmanMod.dll):",
            L"Extra game arguments (optional):",
            L"Browse...",
            L"Close the launcher after a successful start",
            L"Launch game with mod",
            L"Wheelman.exe\0Wheelman.exe\0Programs (*.exe)\0*.exe\0All files\0*.*\0",
            L"WheelmanMod.dll\0WheelmanMod*.dll\0Libraries (*.dll)\0*.dll\0All files\0*.*\0",
            L"Select Wheelman.exe",
            L"Select the mod DLL",
            L"Game executable not found: %s",
            L"Mod DLL not found: %s",
            L"Wheelman.exe must be 32-bit (x86): %s",
            L"The mod DLL must be 32-bit (x86): %s",
            L"Starting: %s",
            L"Could not start the game (error %lu).",
            L"Could not inject the DLL (error %lu, %s). The game process was terminated.",
            L"LoadLibrary returned NULL - the DLL did not load (check Visual C++ Redistributable x86 and the path). The game process was terminated.",
            L"Done: mod loaded (0x%08X). Press Insert in game to open the menu.",
            L"For example: -windowed",
            L"Game found: %s",
            L"Prepare the map and icons from my game",
            L"MapDataBuilder.exe not found (expected next to the launcher, in the tools folder). It is part of the mod release.",
            L"Preparing the map data from your game files (takes about a minute)...",
            L"Done: the city map, the icons and the road mask were created in %s.",
            L"Could not prepare the map data (code %lu). See the messages above.",
            L"The map data (MapData) was not found next to the DLL: the external minimap will have no city picture and no icons. Press \"Prepare the map and icons from my game\".",
            L"The English game version (-language=int) has not been tested."
        }
    };

    // Value of the game's -language= switch for each entry of the language combo.
    const wchar_t* const kLangArg[2] = { L"rus", L"int" };

    HWND g_wnd = nullptr;
    HFONT g_font = nullptr;
    int g_dpi = 96;
    Lang g_lang = RU;
    std::wstring g_iniPath;

    int Sc(int v) { return MulDiv(v, g_dpi, 96); }
    const wchar_t* T(Str s) { return kText[g_lang][s]; }

    std::wstring Fmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[2048];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        return buf;
    }

    std::wstring SelfDir()
    {
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        std::wstring s = p;
        size_t k = s.find_last_of(L"\\/");
        return k == std::wstring::npos ? L"" : s.substr(0, k + 1);
    }

    bool Exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

    std::wstring GetText(int id)
    {
        HWND h = GetDlgItem(g_wnd, id);
        int n = GetWindowTextLengthW(h);
        std::wstring s(n + 1, L'\0');
        GetWindowTextW(h, &s[0], n + 1);
        s.resize(n);
        // trim whitespace and surrounding quotes (paths pasted from Explorer)
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'"')) s.pop_back();
        size_t a = 0;
        while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'"')) ++a;
        return s.substr(a);
    }

    void Log(const std::wstring& line)
    {
        HWND h = GetDlgItem(g_wnd, IDC_LOG);
        SendMessageW(h, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
        std::wstring s = line + L"\r\n";
        SendMessageW(h, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(s.c_str()));
    }

    // Returns 1 when the PE file is 32-bit x86, 0 when it is something else, -1 when it cannot be read.
    int IsPE32(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return -1;
        int result = -1;
        IMAGE_DOS_HEADER dos = {};
        DWORD got = 0;
        if (ReadFile(f, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE)
        {
            SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN);
            DWORD sig = 0;
            IMAGE_FILE_HEADER fh = {};
            if (ReadFile(f, &sig, 4, &got, nullptr) && got == 4 && sig == IMAGE_NT_SIGNATURE &&
                ReadFile(f, &fh, sizeof(fh), &got, nullptr) && got == sizeof(fh))
                result = fh.Machine == IMAGE_FILE_MACHINE_I386 ? 1 : 0;
        }
        CloseHandle(f);
        return result;
    }

    // ---- settings ------------------------------------------------------------------------------------------------
    void EnsureIni()
    {
        if (Exists(g_iniPath)) return;
        HANDLE f = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        const WORD bom = 0xFEFF;   // UTF-16 LE BOM: makes the *PrivateProfile*W functions keep the file in Unicode
        DWORD w = 0;
        WriteFile(f, &bom, 2, &w, nullptr);
        CloseHandle(f);
    }

    std::wstring IniGet(const wchar_t* key, const wchar_t* def = L"")
    {
        wchar_t buf[2048] = {};
        GetPrivateProfileStringW(L"Launcher", key, def, buf, _countof(buf), g_iniPath.c_str());
        return buf;
    }

    void IniSet(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"Launcher", key, v.c_str(), g_iniPath.c_str()); }

    void SaveSettings()
    {
        EnsureIni();
        IniSet(L"Language", g_lang == RU ? L"ru" : L"en");
        IniSet(L"GameExe", GetText(IDC_EXE));
        IniSet(L"ModDll", GetText(IDC_DLL));
        IniSet(L"ExtraArgs", GetText(IDC_ARGS));
        IniSet(L"CloseAfterLaunch", SendMessageW(GetDlgItem(g_wnd, IDC_CLOSE), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    }

    std::wstring FindGameExe()
    {
        std::wstring self = SelfDir();
        if (Exists(self + L"Wheelman.exe")) return self + L"Wheelman.exe";
        static const wchar_t* const kRel[] = {
            L"WheelMan\\Binaries\\Wheelman.exe", L"Games\\WheelMan\\Binaries\\Wheelman.exe", L"Games\\Wheelman\\Binaries\\Wheelman.exe",
            L"Program Files (x86)\\Steam\\steamapps\\common\\Wheelman\\Binaries\\Wheelman.exe",
            L"Program Files (x86)\\Wheelman\\Binaries\\Wheelman.exe", L"Program Files\\Wheelman\\Binaries\\Wheelman.exe" };
        const DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; ++d)
        {
            if (!(drives & (1u << d))) continue;
            std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + d)) + L":\\";
            if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
            for (const wchar_t* rel : kRel)
                if (Exists(root + rel)) return root + rel;
        }
        return L"";
    }

    // ---- UI ----------------------------------------------------------------------------------------------------------
    void ApplyLanguage()
    {
        SetWindowTextW(g_wnd, T(S_TITLE));
        SetDlgItemTextW(g_wnd, IDC_LANG_LABEL, T(S_LANG));
        SetDlgItemTextW(g_wnd, IDC_EXE_LABEL, T(S_EXE));
        SetDlgItemTextW(g_wnd, IDC_DLL_LABEL, T(S_DLL));
        SetDlgItemTextW(g_wnd, IDC_ARGS_LABEL, T(S_ARGS));
        SetDlgItemTextW(g_wnd, IDC_EXE_BROWSE, T(S_BROWSE));
        SetDlgItemTextW(g_wnd, IDC_DLL_BROWSE, T(S_BROWSE));
        SetDlgItemTextW(g_wnd, IDC_CLOSE, T(S_CLOSE));
        SetDlgItemTextW(g_wnd, IDC_LAUNCH, T(S_LAUNCH));
        SetDlgItemTextW(g_wnd, IDC_MAPDATA, T(S_MAPDATA));
        SetDlgItemTextW(g_wnd, IDC_LANG_NOTE, T(S_LANG_NOTE));
        SendMessageW(GetDlgItem(g_wnd, IDC_ARGS), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(T(S_ARGS_HINT)));
    }

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int x, int y, int w, int h, int id)
    {
        return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, Sc(x), Sc(y), Sc(w), Sc(h), g_wnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    }

    void Browse(int editId, Str filterStr, Str titleStr)
    {
        wchar_t file[MAX_PATH] = {};
        std::wstring cur = GetText(editId);
        wcsncpy_s(file, cur.c_str(), _TRUNCATE);
        std::wstring dir;
        size_t k = cur.find_last_of(L"\\/");
        if (k != std::wstring::npos) dir = cur.substr(0, k);
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = g_wnd;
        ofn.lpstrFilter = T(filterStr);   // "name\0pattern\0..." pairs; the first one matches the expected file name
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = T(titleStr);
        ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn)) SetDlgItemTextW(g_wnd, editId, file);
    }

    // ---- launch ------------------------------------------------------------------------------------------------------
    void Launch()
    {
        const std::wstring exe = GetText(IDC_EXE), dll = GetText(IDC_DLL), extra = GetText(IDC_ARGS);
        SetDlgItemTextW(g_wnd, IDC_LOG, L"");
        if (!Exists(exe)) { Log(Fmt(T(S_ERR_NOEXE), exe.c_str())); return; }
        if (!Exists(dll)) { Log(Fmt(T(S_ERR_NODLL), dll.c_str())); return; }
        if (IsPE32(exe) == 0) { Log(Fmt(T(S_ERR_NOT32_EXE), exe.c_str())); return; }
        if (IsPE32(dll) == 0) { Log(Fmt(T(S_ERR_NOT32_DLL), dll.c_str())); return; }
        SaveSettings();

        std::wstring workDir = exe.substr(0, exe.find_last_of(L"\\/"));
        // Without -language the game's bootstrap crashes (see Injector.cpp), so it is always passed.
        std::wstring cmd = L"\"" + exe + L"\" -language=" + kLangArg[g_lang];
        if (!extra.empty()) cmd += L" " + extra;
        Log(Fmt(T(S_STARTING), cmd.c_str()));

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, workDir.c_str(), &si, &pi))
        {
            Log(Fmt(T(S_CREATE_FAIL), GetLastError()));
            return;
        }

        auto fail = [&](const std::wstring& msg)
        {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Log(msg);
        };

        const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
        LPVOID remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) { fail(Fmt(T(S_INJECT_FAIL), GetLastError(), L"VirtualAllocEx")); return; }
        if (!WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr)) { fail(Fmt(T(S_INJECT_FAIL), GetLastError(), L"WriteProcessMemory")); return; }

        // kernel32 sits at the same base in every 32-bit process of a boot session, so our own LoadLibraryW address is valid there.
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE th = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLib, remote, 0, nullptr);
        if (!th) { fail(Fmt(T(S_INJECT_FAIL), GetLastError(), L"CreateRemoteThread")); return; }
        const DWORD wait = WaitForSingleObject(th, 20000);
        DWORD module = 0;
        GetExitCodeThread(th, &module);
        CloseHandle(th);
        if (wait == WAIT_OBJECT_0) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
        if (wait != WAIT_OBJECT_0 || module == 0) { fail(T(S_LOAD_FAIL)); return; }

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(Fmt(T(S_DONE), module));
        if (SendMessageW(GetDlgItem(g_wnd, IDC_CLOSE), BM_GETCHECK, 0, 0) == BST_CHECKED) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }

    // ---- map data (city map picture, marker icons, road mask) built from the player's own game files ---------------------
    // MapDataBuilder.exe (tools\MapDataBuilder in the source tree, tools\ or next to the launcher in a release) does the work;
    // it runs on a worker thread and its console output is shown in the log.
    constexpr UINT WM_MAPDATA_LINE = WM_APP + 1, WM_MAPDATA_DONE = WM_APP + 2;
    std::wstring g_mapDataOut;
    bool g_mapDataBusy = false;

    std::wstring FindMapBuilder()
    {
        const std::wstring self = SelfDir();
        const std::wstring cand[] = { self + L"tools\\MapDataBuilder.exe", self + L"MapDataBuilder.exe", self + L"..\\tools\\MapDataBuilder.exe",
                                      self + L"..\\..\\tools\\MapDataBuilder\\bin\\MapDataBuilder.exe", self + L"..\\tools\\MapDataBuilder\\bin\\MapDataBuilder.exe" };
        for (const std::wstring& c : cand)
            if (Exists(c)) { wchar_t full[MAX_PATH] = {}; GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr); return full; }
        return L"";
    }

    std::wstring MapDataDir()
    {
        std::wstring dll = GetText(IDC_DLL);
        size_t k = dll.find_last_of(L"\\/");
        return (k == std::wstring::npos ? SelfDir() : dll.substr(0, k + 1)) + L"MapData";
    }

    struct MapJob { std::wstring builder, exe, out; };

    DWORD WINAPI MapDataThread(LPVOID param)
    {
        MapJob* job = static_cast<MapJob*>(param);
        DWORD code = 1;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE rd = nullptr, wr = nullptr;
        if (CreatePipe(&rd, &wr, &sa, 0))
        {
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            si.hStdOutput = wr; si.hStdError = wr;
            PROCESS_INFORMATION pi = {};
            std::wstring cmd = L"\"" + job->builder + L"\" \"" + job->exe + L"\" \"" + job->out + L"\"";
            const std::wstring workDir = job->builder.substr(0, job->builder.find_last_of(L"\\/"));
            if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi))
            {
                CloseHandle(wr); wr = nullptr;
                std::string pending; char buf[512]; DWORD got = 0;
                auto flushLine = [&](const std::string& line)
                {
                    if (line.empty()) return;
                    int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
                    std::wstring* w = new std::wstring(n, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), &(*w)[0], n);
                    PostMessageW(g_wnd, WM_MAPDATA_LINE, 0, reinterpret_cast<LPARAM>(w));
                };
                while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
                {
                    pending.append(buf, got);
                    size_t nl;
                    while ((nl = pending.find('\n')) != std::string::npos)
                    {
                        std::string line = pending.substr(0, nl);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        // the builder prints a lot of per-sprite lines; only its step markers and problems are worth showing
                        if (line.rfind("[", 0) == 0 || line.rfind("DONE", 0) == 0 || line.rfind("ERROR", 0) == 0 || line.find("skipped") != std::string::npos) flushLine(line);
                        pending.erase(0, nl + 1);
                    }
                }
                WaitForSingleObject(pi.hProcess, 60000);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            }
            if (wr) CloseHandle(wr);
            CloseHandle(rd);
        }
        delete job;
        PostMessageW(g_wnd, WM_MAPDATA_DONE, code, 0);
        return 0;
    }

    void PrepareMapData()
    {
        if (g_mapDataBusy) return;
        const std::wstring exe = GetText(IDC_EXE);
        SetDlgItemTextW(g_wnd, IDC_LOG, L"");
        if (!Exists(exe)) { Log(Fmt(T(S_ERR_NOEXE), exe.c_str())); return; }
        const std::wstring builder = FindMapBuilder();
        if (builder.empty()) { Log(T(S_MAPDATA_NOBUILDER)); return; }
        SaveSettings();
        g_mapDataOut = MapDataDir();
        Log(T(S_MAPDATA_START));
        MapJob* job = new MapJob{ builder, exe, g_mapDataOut };
        g_mapDataBusy = true;
        EnableWindow(GetDlgItem(g_wnd, IDC_MAPDATA), FALSE);
        HANDLE t = CreateThread(nullptr, 0, MapDataThread, job, 0, nullptr);
        if (t) CloseHandle(t);
        else { g_mapDataBusy = false; EnableWindow(GetDlgItem(g_wnd, IDC_MAPDATA), TRUE); delete job; }
    }

    void OnCreate()
    {
        g_iniPath = SelfDir() + L"Launcher.ini";

        const std::wstring lang = IniGet(L"Language");
        if (lang == L"en") g_lang = EN;
        else if (lang == L"ru") g_lang = RU;
        else g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? RU : EN;

        Make(L"STATIC", L"", 0, 0, 16, 14, 528, 18, IDC_LANG_LABEL);
        HWND combo = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 16, 34, 220, 200, IDC_LANG);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Русский (-language=rus)"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English (-language=int)"));
        SendMessageW(combo, CB_SETCURSEL, g_lang, 0);
        Make(L"STATIC", L"", 0, 0, 250, 38, 294, 34, IDC_LANG_NOTE);

        Make(L"STATIC", L"", 0, 0, 16, 70, 528, 18, IDC_EXE_LABEL);
        Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 16, 90, 436, 24, IDC_EXE);
        Make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 0, 460, 89, 84, 26, IDC_EXE_BROWSE);

        Make(L"STATIC", L"", 0, 0, 16, 126, 528, 18, IDC_DLL_LABEL);
        Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 16, 146, 436, 24, IDC_DLL);
        Make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 0, 460, 145, 84, 26, IDC_DLL_BROWSE);

        Make(L"STATIC", L"", 0, 0, 16, 182, 528, 18, IDC_ARGS_LABEL);
        Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 16, 202, 528, 24, IDC_ARGS);

        Make(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 16, 238, 528, 20, IDC_CLOSE);
        Make(L"BUTTON", L"", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 16, 266, 528, 38, IDC_LAUNCH);
        Make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 0, 16, 312, 528, 30, IDC_MAPDATA);
        Make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE, 16, 352, 528, 110, IDC_LOG);

        EnumChildWindows(g_wnd, [](HWND h, LPARAM) -> BOOL { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE); return TRUE; }, 0);
        // The launch button is bold to stand out.
        LOGFONTW lf = {};
        GetObjectW(g_font, sizeof(lf), &lf);
        lf.lfWeight = FW_SEMIBOLD;
        SendDlgItemMessageW(g_wnd, IDC_LAUNCH, WM_SETFONT, reinterpret_cast<WPARAM>(CreateFontIndirectW(&lf)), TRUE);

        std::wstring exe = IniGet(L"GameExe");
        bool found = false;
        if (!Exists(exe)) { exe = FindGameExe(); found = !exe.empty(); }
        std::wstring dll = IniGet(L"ModDll");
        if (!Exists(dll))
        {
            // next to the launcher first, then the layout of the source tree (Launcher\bin -> ..\..\bin\Win32\Release)
            const std::wstring self = SelfDir();
            const std::wstring cand[] = { self + L"WheelmanMod.dll", self + L"..\\WheelmanMod.dll", self + L"Win32\\Release\\WheelmanMod.dll", self + L"..\\..\\bin\\Win32\\Release\\WheelmanMod.dll" };
            dll = cand[0];
            for (const std::wstring& c : cand)
                if (Exists(c)) { wchar_t full[MAX_PATH] = {}; GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr); dll = full; break; }
        }
        SetDlgItemTextW(g_wnd, IDC_EXE, exe.c_str());
        SetDlgItemTextW(g_wnd, IDC_DLL, dll.c_str());
        SetDlgItemTextW(g_wnd, IDC_ARGS, IniGet(L"ExtraArgs").c_str());
        SendDlgItemMessageW(g_wnd, IDC_CLOSE, BM_SETCHECK, IniGet(L"CloseAfterLaunch", L"1") == L"0" ? BST_UNCHECKED : BST_CHECKED, 0);
        ApplyLanguage();
        if (found) Log(Fmt(T(S_FOUND_EXE), exe.c_str()));
        if (!Exists(MapDataDir() + L"\\citymap.dxt1")) Log(T(S_MAPDATA_MISSING));
        SetFocus(GetDlgItem(g_wnd, IDC_LAUNCH));
    }

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_CREATE:
            g_wnd = h;
            OnCreate();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case IDC_LANG:
                if (HIWORD(w) == CBN_SELCHANGE)
                {
                    g_lang = SendDlgItemMessageW(h, IDC_LANG, CB_GETCURSEL, 0, 0) == 1 ? EN : RU;
                    ApplyLanguage();
                }
                break;
            case IDC_EXE_BROWSE: Browse(IDC_EXE, S_FILTER_EXE, S_PICK_EXE); break;
            case IDC_DLL_BROWSE: Browse(IDC_DLL, S_FILTER_DLL, S_PICK_DLL); break;
            case IDC_LAUNCH: Launch(); break;
            case IDC_MAPDATA: PrepareMapData(); break;
            }
            return 0;
        case WM_MAPDATA_LINE:
        {
            std::wstring* line = reinterpret_cast<std::wstring*>(l);
            Log(*line);
            delete line;
            return 0;
        }
        case WM_MAPDATA_DONE:
            g_mapDataBusy = false;
            EnableWindow(GetDlgItem(h, IDC_MAPDATA), TRUE);
            Log(w == 0 ? Fmt(T(S_MAPDATA_OK), g_mapDataOut.c_str()) : Fmt(T(S_MAPDATA_FAIL), static_cast<unsigned long>(w)));
            return 0;
        case WM_CLOSE:
            SaveSettings();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -MulDiv(9, g_dpi, 72);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"WheelmanModLauncher";
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, Sc(560), Sc(480) };
    AdjustWindowRect(&rc, style, FALSE);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Wheelman Mod", style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                               (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (!wnd) return 1;
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}

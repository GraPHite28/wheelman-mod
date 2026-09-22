#include <Windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "imgui/imgui.h"
#include "MiniMap.h"
#include "Binds.h"
#include "Log.h"
#include "Nav.h"
#include "Police.h"
#include <algorithm>
#include <vector>
#include "MapTools.h"
#include "Npc.h"
#include "Overlay.h"
#include "Lang.h"

namespace MiniMap
{
    bool enabled = false;
    bool rotate = true;
    float rangeMeters = 400.f;
    int sizePx = 260;
    float opacity = 0.75f;
    float iconScale = 1.f;
    bool onlyGameVisible = false;
    bool catOn[kCategoryCount] = {
        true,  true,  false, true,  true,  true,  true,  true,  true,  true,
        true,  true,  false, false, false, true,  true,  true,  true,
    };
    // the mission icon (a big round badge) looks larger than the pins: it starts a bit smaller
    float catScale[kCategoryCount] = {
        1.f, 0.75f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
        1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
    };
    bool showPoliceZone = true;
    float zoneScale = 1.f;
    int autoScanSeconds = 3;   // mission markers appear while a mission runs, so the list is refreshed regularly
    bool background = true;
    float bgOpacity = 0.9f;
    // world -> map picture: u(px) = (Y - originY) / cmPerPx, v(px) = (originX - X) / cmPerPx. Fitted so the event markers
    // lie on the roads of the 6144x4096 city map (UI_PDA pda_I140 + pda_I142); tunable in the Map tab.
    float mapOriginX = 384610.f, mapOriginY = -856220.f, mapCmPerPx = 225.f;
    bool square = false;
    bool matchGame = false;
    bool matchScale = true;
    bool matchRotation = true;
    float matchZoom = 1.f;
    float matchYawOffsetDeg = 0.f;
    bool matchYawInvert = false;
    float gameMapScale = 0.f, gameMapYawUnits = 0.f;

    namespace
    {
        IDirect3DDevice9* g_device = nullptr;
        IDirect3DTexture9* g_mapTex = nullptr;
        bool g_mapTried = false;
        std::string g_mapStatus = "not loaded";
        constexpr int kMapW = 6144, kMapH = 4096;

        // MapData\citymap.dxt1: 'MAP1', width, height, then DXT1 blocks row by row (see Tools\decompiler\TexDump).
        void EnsureMapTexture()
        {
            if (g_mapTex || g_mapTried || !g_device) return;
            g_mapTried = true;
            std::string path = std::string(Binds::DataDir()) + "MapData\\citymap.dxt1";
            HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) { g_mapStatus = "MapData\\citymap.dxt1 not found next to the DLL"; return; }
            const DWORD expect = 12 + kMapW / 4 * (kMapH / 4) * 8;
            const DWORD size = GetFileSize(f, nullptr);
            std::string data;
            if (size == expect) { data.resize(size); DWORD got = 0; ReadFile(f, &data[0], size, &got, nullptr); if (got != size) data.clear(); }
            CloseHandle(f);
            if (data.empty() || memcmp(data.data(), "MAP1", 4) != 0) { g_mapStatus = "citymap.dxt1 has a wrong size or header"; return; }
            D3DCAPS9 caps{};
            if (SUCCEEDED(g_device->GetDeviceCaps(&caps)) && (caps.MaxTextureWidth < kMapW || caps.MaxTextureHeight < kMapH))
            { g_mapStatus = "the graphics card does not support a 6144x4096 texture"; return; }
            HRESULT hr = g_device->CreateTexture(kMapW, kMapH, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &g_mapTex, nullptr);
            if (FAILED(hr) || !g_mapTex) { g_mapTex = nullptr; g_mapStatus = "CreateTexture failed (DXT1 6144x4096)"; return; }
            D3DLOCKED_RECT lr{};
            if (FAILED(g_mapTex->LockRect(0, &lr, nullptr, 0))) { g_mapTex->Release(); g_mapTex = nullptr; g_mapStatus = "LockRect failed"; return; }
            const int rowBytes = kMapW / 4 * 8;
            for (int r = 0; r < kMapH / 4; ++r)
                memcpy(static_cast<char*>(lr.pBits) + static_cast<size_t>(r) * lr.Pitch, data.data() + 12 + static_cast<size_t>(r) * rowBytes, rowBytes);
            g_mapTex->UnlockRect(0);
            g_mapStatus = "loaded";
            LogF("MiniMap: city map texture loaded");
        }

        // the running HUD screen: map scale (+0x36C), mask width (+0x380) and the previous map yaw (+0x358, rotator units)
        float g_maskW = 0.f;
        bool ReadGameMinimap()
        {
            static ULONGLONG lastScan = 0;
            const ULONGLONG now = GetTickCount64();
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int i = 0; i < MapTools::HudCount(); ++i)
                {
                    MapTools::Hud h{};
                    if (!MapTools::HudAt(i, h) || !MapTools::HudLive(h.obj)) continue;
                    gameMapScale = MapTools::GetFloat(h.obj, 0x36C);
                    gameMapYawUnits = MapTools::GetFloat(h.obj, 0x358);
                    g_maskW = MapTools::GetFloat(h.obj, 0x380);
                    return gameMapScale > 1e-5f && g_maskW > 1.f;
                }
                if (pass == 0 && matchGame && now - lastScan > 10000) { lastScan = now; MapTools::ScanHudAsync(); } break;
            }
            return false;
        }

        template <class T> bool Rd(uintptr_t a, T& out)
        {
            __try { out = *reinterpret_cast<const T*>(a); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // shape: 0 square, 1 diamond, 2 triangle, 3 circle, 4 hexagon; stick: sits on the map edge when out of range
        struct CatInfo { const char* label; const char* key; ImU32 color; int shape; bool stick; };
        const CatInfo kCats[kCategoryCount] = {
            { "Garages and shops",        "garages",   IM_COL32(70, 200, 255, 255),  0, false },
            { "Story missions",           "story",     IM_COL32(60, 225, 90, 255),   1, true  },
            { "Gang territory markers",   "gangs",     IM_COL32(200, 90, 220, 255),  3, false },
            { "Races and checkpoints",    "races",     IM_COL32(255, 120, 60, 255),  1, false },
            { "Hot potato",               "potato",    IM_COL32(255, 70, 70, 255),   4, false },
            { "Rampage",                  "rampage",   IM_COL32(220, 40, 40, 255),   2, false },
            { "Contracts",                "contracts", IM_COL32(90, 230, 110, 255),  0, false },
            { "Made to order",            "order",     IM_COL32(240, 200, 120, 255), 4, false },
            { "Taxi",                     "taxi",      IM_COL32(255, 235, 60, 255),  3, false },
            { "Fugitive",                 "fugitive",  IM_COL32(120, 150, 255, 255), 2, false },
            { "Event ranks",              "ranks",     IM_COL32(255, 180, 230, 255), 1, false },
            { "Weapons and ammo",         "weapons",   IM_COL32(255, 140, 40, 255),  2, false },
            { "Vehicles",                 "vehicles",  IM_COL32(150, 220, 255, 255), 0, false },
            { "Police markers",           "policemk",  IM_COL32(60, 110, 255, 255),  4, false },
            { "Other markers",            "other",     IM_COL32(190, 190, 190, 255), 3, false },
            { "Police officers (live)",   "police",    IM_COL32(70, 120, 255, 255),  3, false },
            { "Enemies (live)",           "enemies",   IM_COL32(255, 60, 50, 255),   3, false },
            { "Allies (live)",            "allies",    IM_COL32(70, 230, 100, 255),  3, false },
            { "Special enemies (live)",   "special",   IM_COL32(255, 170, 40, 255),  1, false },
        };

        int CategoryOfName(const char* n)
        {
            if (strstr(n, "POLICE")) return CatPoliceMarker;
            if (strstr(n, "GARAGE") || strstr(n, "TUNING_SHOP") || strstr(n, "WORKSHOP")) return CatGarage;
            if (strstr(n, "ROMANIAN") || strstr(n, "LOS_LANTOS") || strstr(n, "CHULOS") || strstr(n, "NEUTRAL")) return CatGang;
            if (strstr(n, "HOT_POTATO")) return CatHotPotato;
            if (strstr(n, "RAMPAGE")) return CatRampage;
            if (strstr(n, "CONTRACTS")) return CatContracts;
            if (strstr(n, "MADE_TO_ORDER")) return CatMadeToOrder;
            if (strstr(n, "TAXI")) return CatTaxi;
            if (strstr(n, "FUGITIVE")) return CatFugitive;
            if (strstr(n, "EVENT_RANK")) return CatEventRank;
            if (strstr(n, "RACE") || strstr(n, "CHECKPOINT") || strstr(n, "FINISH") || strstr(n, "THE_HARD_PATH")) return CatRace;
            if (strstr(n, "AMMO") || strstr(n, "AK47") || strstr(n, "COLT") || strstr(n, "GRENADE") || strstr(n, "MINIMI") ||
                strstr(n, "MP5") || strstr(n, "SHOTGUN") || strstr(n, "UZI") || strstr(n, "BERETTA")) return CatWeapons;
            if (strstr(n, "VEHICLE")) return CatVehicle;
            if (strstr(n, "MISSION") || strstr(n, "BOSS") || strstr(n, "NPC_MARKER")) return CatStory;
            return CatOther;
        }

        // by the marker's icon; a marker with no icon (plain MAP_MARKER) falls back to its class
        int CategoryOf(const MapTools::Row& r)
        {
            static signed char cache[128];
            static bool init = false;
            if (!init) { for (int i = 0; i < 128; ++i) cache[i] = -1; init = true; }
            int icon = r.icon ? r.icon : r.uiIcon;
            if (icon <= 0 || icon >= 128 || icon >= MapTools::IconCount())
                icon = 0;
            if (icon == 0 || (icon == 2 && r.icon == 0))
            {
                if (strstr(r.className, "Cache") || strcmp(r.className, "WheelmanUnlockable") == 0) return CatWeapons;
                if (strstr(r.className, "Garage")) return CatGarage;
                if (strstr(r.className, "Objective") || strstr(r.className, "EventMarker")) return CatStory;
                return CatOther;
            }
            if (cache[icon] < 0) cache[icon] = static_cast<signed char>(CategoryOfName(MapTools::IconName(icon)));
            return cache[icon];
        }

        ULONGLONG g_lastScan = 0;
        bool g_scannedOnce = false;

        void DrawShape(ImDrawList* dl, int shape, ImVec2 p, float sz, ImU32 col)
        {
            const ImU32 black = IM_COL32(0, 0, 0, 220);
            switch (shape)
            {
            case 0: dl->AddRectFilled(ImVec2(p.x - sz, p.y - sz), ImVec2(p.x + sz, p.y + sz), col); dl->AddRect(ImVec2(p.x - sz, p.y - sz), ImVec2(p.x + sz, p.y + sz), black); break;
            case 1:
            {
                const ImVec2 a(p.x, p.y - sz - 1), b(p.x + sz + 1, p.y), c(p.x, p.y + sz + 1), d(p.x - sz - 1, p.y);
                dl->AddQuadFilled(a, b, c, d, col); dl->AddQuad(a, b, c, d, black); break;
            }
            case 2:
            {
                const ImVec2 a(p.x, p.y - sz - 1), b(p.x - sz - 1, p.y + sz), c(p.x + sz + 1, p.y + sz);
                dl->AddTriangleFilled(a, b, c, col); dl->AddTriangle(a, b, c, black); break;
            }
            case 4: dl->AddNgonFilled(p, sz + 1, col, 6); dl->AddNgon(p, sz + 1, black, 6); break;
            default: dl->AddCircleFilled(p, sz, col, 16); dl->AddCircle(p, sz, black, 16); break;
            }
        }
    }

    namespace
    {
        // heading: the vehicle's yaw while driving, else the pawn's (rotator yaw, 65536 = a full turn)
        float PlayerHeadingRad()
        {
            int32_t yaw = 0;
            if (const uintptr_t pawn = Overlay::GetPlayerPawn())
            {
                uint32_t veh = 0;
                if (Rd(pawn + 0x4BC, veh) && veh > 0x10000 && Rd(static_cast<uintptr_t>(veh) + 0xE4, yaw)) {}
                else Rd(pawn + 0xE4, yaw);
            }
            return static_cast<float>(yaw) * (6.2831853f / 65536.f);
        }

        // ---- marker icons: the game's own Scaleform art (UI_PDA sprites rendered offline by Tools\decompiler\SwfIcons) ----
        IDirect3DTexture9* g_iconTex = nullptr;
        bool g_iconTried = false;
        int g_iconCols = 8, g_iconCell = 64, g_iconCount = 0;
        // atlas order = the sprite list given to SwfIcons
        enum Sprite { SpHotPotato, SpRampage, SpRomanian, SpContracts, SpRace, SpFugitive, SpMadeToOrder, SpTaxi, SpLosLantos, SpChulos, SpNeutral,
                      SpRankC, SpRankB, SpRankA, SpRankS, SpPolice, SpFinish, SpCheckpoint, SpMissionWorld, SpBoss, SpAk47, SpColt, SpGrenade,
                      SpMinimi, SpMp5, SpShotgun, SpUzi, SpBeretta, SpGarage, SpMapMarker, SpMission, SpPoliceInfluence };

        void EnsureIconTexture()
        {
            if (g_iconTex || g_iconTried || !g_device) return;
            g_iconTried = true;
            const std::string path = std::string(Binds::DataDir()) + "MapData\\icons.bgra";
            HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;
            const DWORD size = GetFileSize(f, nullptr);
            std::string d(size, '\0'); DWORD got = 0;
            ReadFile(f, &d[0], size, &got, nullptr);
            CloseHandle(f);
            if (got != size || size < 16 || memcmp(d.data(), "ICON", 4) != 0) return;
            int cols, cell, count;
            memcpy(&cols, d.data() + 4, 4); memcpy(&cell, d.data() + 8, 4); memcpy(&count, d.data() + 12, 4);
            const int w = cols * cell, h = ((count + cols - 1) / cols) * cell;
            if (cols <= 0 || cell <= 0 || static_cast<size_t>(w) * h * 4 + 16 != size) return;
            if (FAILED(g_device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_iconTex, nullptr)) || !g_iconTex) { g_iconTex = nullptr; return; }
            D3DLOCKED_RECT lr{};
            if (FAILED(g_iconTex->LockRect(0, &lr, nullptr, 0))) { g_iconTex->Release(); g_iconTex = nullptr; return; }
            for (int y = 0; y < h; ++y) memcpy(static_cast<char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, d.data() + 16 + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
            g_iconTex->UnlockRect(0);
            g_iconCols = cols; g_iconCell = cell; g_iconCount = count;
            LogF("MiniMap: %d marker icons loaded", count);
        }

        int SpriteOfName(const char* n)
        {
            if (strstr(n, "POLICE_INFLUENCE")) return SpPoliceInfluence;
            if (strstr(n, "POLICE")) return SpPolice;
            if (strstr(n, "GARAGE") || strstr(n, "TUNING_SHOP") || strstr(n, "WORKSHOP")) return SpGarage;
            if (strstr(n, "ROMANIAN")) return SpRomanian;
            if (strstr(n, "LOS_LANTOS")) return SpLosLantos;
            if (strstr(n, "CHULOS")) return SpChulos;
            if (strstr(n, "NEUTRAL")) return SpNeutral;
            if (strstr(n, "HOT_POTATO")) return SpHotPotato;
            if (strstr(n, "RAMPAGE")) return SpRampage;
            if (strstr(n, "CONTRACTS")) return SpContracts;
            if (strstr(n, "MADE_TO_ORDER")) return SpMadeToOrder;
            if (strstr(n, "TAXI")) return SpTaxi;
            if (strstr(n, "FUGITIVE")) return SpFugitive;
            if (strstr(n, "EVENT_RANK_S")) return SpRankS;
            if (strstr(n, "EVENT_RANK_A")) return SpRankA;
            if (strstr(n, "EVENT_RANK_B")) return SpRankB;
            if (strstr(n, "EVENT_RANK_C")) return SpRankC;
            if (strstr(n, "FINISH")) return SpFinish;
            if (strstr(n, "RACE_CHECKPOINT")) return SpRace;
            if (strstr(n, "CHECKPOINT")) return SpCheckpoint;
            if (strstr(n, "THE_HARD_PATH") || strstr(n, "RACE")) return SpRace;
            if (strstr(n, "BOSS")) return SpBoss;
            if (strstr(n, "AK47")) return SpAk47;
            if (strstr(n, "COLT")) return SpColt;
            if (strstr(n, "GRENADE")) return SpGrenade;
            if (strstr(n, "MINIMI")) return SpMinimi;
            if (strstr(n, "MP5")) return SpMp5;
            if (strstr(n, "SHOTGUN")) return SpShotgun;
            if (strstr(n, "UZI")) return SpUzi;
            if (strstr(n, "BERETTA")) return SpBeretta;
            if (strstr(n, "AMMO")) return SpAk47;
            if (strstr(n, "VEHICLE") || strstr(n, "MISSION_WORLD")) return SpMissionWorld;
            if (strstr(n, "MISSION") || strstr(n, "NPC")) return SpMission;
            return SpMapMarker;
        }

        // sprite of a marker row: by its icon, else by its class (weapon caches have no icon)
        int SpriteFor(const MapTools::Row& r)
        {
            static signed char cache[128];
            static bool init = false;
            if (!init) { for (int i = 0; i < 128; ++i) cache[i] = -1; init = true; }
            const int icon = r.icon ? r.icon : r.uiIcon;
            if (icon > 0 && icon < 128 && icon < MapTools::IconCount() && !(icon == 2 && r.icon == 0))
            {
                if (cache[icon] < 0) cache[icon] = static_cast<signed char>(SpriteOfName(MapTools::IconName(icon)));
                return cache[icon];
            }
            if (strstr(r.className, "Cache") || strcmp(r.className, "WheelmanUnlockable") == 0) return SpAk47;
            if (strstr(r.className, "Garage")) return SpGarage;
            if (strstr(r.className, "Objective") || strstr(r.className, "EventMarker")) return SpMission;
            return SpMapMarker;
        }

        // half = the old shape's half size. The atlas cells keep every sprite's own origin (pin tip / badge centre) at 40% x 75% of the cell,\r\n        // so the icon is placed with that point exactly on the marker position; a cell is drawn 4.5 times the half size wide.
        void DrawMarkerIcon(ImDrawList* dl, int sprite, int shape, ImVec2 p, float half, ImU32 tint, float dirX = 0.f, float dirY = -1.f)
        {
            EnsureIconTexture();
            if (!g_iconTex || sprite < 0 || sprite >= g_iconCount) { DrawShape(dl, shape, p, half, tint); return; }
            const float s = half * 4.5f;
            const int cx = sprite % g_iconCols, cy = sprite / g_iconCols;
            const float W = static_cast<float>(g_iconCols * g_iconCell), H = static_cast<float>(((g_iconCount + g_iconCols - 1) / g_iconCols) * g_iconCell);
            const ImVec2 uv0(cx * g_iconCell / W, cy * g_iconCell / H), uv1((cx + 1) * g_iconCell / W, (cy + 1) * g_iconCell / H);
            const ImTextureID tex = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_iconTex));
            if (dirX == 0.f && dirY == -1.f)
            {
                dl->AddImage(tex, ImVec2(p.x - s * 0.4f, p.y - s * 0.75f), ImVec2(p.x + s * 0.6f, p.y + s * 0.25f), uv0, uv1, tint);
                return;
            }
            // rotated: turn the cell about the origin so that the sprite's "up" points along (dirX, dirY)
            const float sn = dirX, cs = -dirY;
            auto rot = [&](float lx, float ly) { return ImVec2(p.x + lx * cs - ly * sn, p.y + lx * sn + ly * cs); };
            dl->AddImageQuad(tex, rot(-s * 0.4f, -s * 0.75f), rot(s * 0.6f, -s * 0.75f), rot(s * 0.6f, s * 0.25f), rot(-s * 0.4f, s * 0.25f),
                             ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y), ImVec2(uv0.x, uv1.y), tint);
        }

        // readable marker name from the icon enum name: HOT_POTATO_MARKER -> "Hot potato"
        void PrettyIcon(int icon, char* out, size_t n)
        {
            const char* s = MapTools::IconName(icon);
            char tmp[64]; size_t k = 0;
            for (const char* p = s; *p && k + 1 < sizeof(tmp); ++p) tmp[k++] = *p == '_' ? ' ' : static_cast<char>(k == 0 ? *p : tolower(static_cast<unsigned char>(*p)));
            tmp[k] = 0;
            char* m = strstr(tmp, " marker");
            if (m) *m = 0;
            snprintf(out, n, "%s", tmp);
        }
    }

    const char* CategoryLabel(int c) { return c >= 0 && c < kCategoryCount ? kCats[c].label : "?"; }
    const char* CategoryKey(int c) { return c >= 0 && c < kCategoryCount ? kCats[c].key : "?"; }
    unsigned CategoryColor(int c) { return c >= 0 && c < kCategoryCount ? kCats[c].color : 0xFFFFFFFFu; }
    int CategoryOfRow(const MapTools::Row& r) { return CategoryOf(r); }
    void SetDevice(IDirect3DDevice9* d) { g_device = d; }
    const char* BackgroundStatus() { return g_mapStatus.c_str(); }
    void ReloadBackground()
    {
        if (g_mapTex) { g_mapTex->Release(); g_mapTex = nullptr; }
        g_mapTried = false; g_mapStatus = "not loaded";
    }
    bool ReadGameMinimapValues(float& scale, float& maskW, float& yawUnits)
    {
        const bool ok = ReadGameMinimap();
        scale = gameMapScale; maskW = g_maskW; yawUnits = gameMapYawUnits;
        return ok;
    }
    static ULONGLONG g_gpsTick = 0;
    bool NeedsNpcScan() { return (catOn[CatPoliceNpc] || catOn[CatEnemyNpc] || catOn[CatAllyNpc] || catOn[CatSpecialNpc]) && (enabled || GetTickCount64() - g_gpsTick < 2000); }

    namespace
    {
        // live NPCs: enemy / ally / police dots. The game's own minimap draws these with its marker sprite tinted by
        // colour, so the same sprite is used here.
        int NpcCategory(const Npc::Info& n)
        {
            if (n.health <= 0) return -1;
            if (n.police) return CatPoliceNpc;
            if (n.enemy && n.invincible) return CatSpecialNpc;   // protected enemies (bosses, scripted targets): the game marks them specially
            if (n.enemy) return CatEnemyNpc;
            if (n.friendly) return CatAllyNpc;
            return -1;
        }
        // police dots flash red / blue every ~0.6 s while the wanted level is up, plain blue otherwise
        ImU32 PoliceTint()
        {
            static ULONGLONG lastRead = 0; static bool wanted = false;
            const ULONGLONG now = GetTickCount64();
            if (now - lastRead > 250) { lastRead = now; const Police::State st = Police::Read(); wanted = st.found && st.level > 0; }
            if (!wanted) return IM_COL32(60, 110, 255, 255);
            return ((now / 600) & 1) ? IM_COL32(255, 45, 45, 255) : IM_COL32(60, 110, 255, 255);
        }

        // (dirX, dirY) = the NPC's heading on the screen (unit vector); the sprite's chevron points that way
        void DrawNpcDot(ImDrawList* dl, int cat, ImVec2 p, float sc, float dirX, float dirY)
        {
            const ImU32 col = cat == CatPoliceNpc ? PoliceTint() : kCats[cat].color;
            if (cat == CatSpecialNpc && g_iconTex && g_iconCount > SpBoss)
                DrawMarkerIcon(dl, SpBoss, 1, p, 5.5f * sc * catScale[cat], IM_COL32(255, 255, 255, 255));   // the game's boss badge, upright
            else if (g_iconTex && g_iconCount > SpMapMarker)
                DrawMarkerIcon(dl, SpMapMarker, 3, p, 3.6f * sc * catScale[cat], col, dirX, dirY);
            else
            {
                dl->AddCircleFilled(p, 3.5f * sc * catScale[cat], col, 12);
                dl->AddCircle(p, 3.5f * sc * catScale[cat], IM_COL32(0, 0, 0, 220), 12, 1.5f);
            }
        }
    }

    // ---- GPS tab: marker list + full-size map -------------------------------------------------------------------------
    void DrawGpsTab()
    {
        g_gpsTick = GetTickCount64();
        EnsureMapTexture();
        float px = 0, py = 0, pz = 0;
        const bool havePos = Overlay::GetPlayerPosition(px, py, pz);
        const float yawRad = PlayerHeadingRad();

        if (Nav::HasWaypoint())
        {
            ImGui::Text(TR("Waypoint: %s   route %.0f m"), Nav::WaypointName(), Nav::RouteMeters());
            if (!Nav::OnRoads()) { ImGui::SameLine(); ImGui::TextDisabled("(straight line - no road path found)"); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear waypoint")) Nav::Clear();
        }
        else ImGui::TextDisabled("No waypoint: click the map (or pick a marker in the list) to set one. Right-click on the minimap works too.");
        Binds::Check("Show the route on the maps", &Nav::showRoute);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::SliderFloat("Arrival distance (m)", &Nav::arriveMeters, 10.f, 150.f, "%.0f");
        if (!Nav::Available()) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "GPS: %s", Nav::Status());
        if (!g_mapTex) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), TR("Map picture: %s"), g_mapStatus.c_str());

        static int filterCat = -1;
        static char search[40] = "";
        static bool followMe = true;
        static float zoom = 0.22f, cxp = 3000.f, cyp = 1800.f;   // screen px per map px; view centre in map px

        ImGui::BeginChild("gpslist", ImVec2(330, 0), true);
        {
            char cur[64]; snprintf(cur, sizeof(cur), "%s", filterCat < 0 ? TR("All types") : TR(CategoryLabel(filterCat)));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##gpsfilter", cur))
            {
                if (ImGui::Selectable(TR("All types"), filterCat < 0)) filterCat = -1;
                for (int c = 0; c < kMarkerCategoryCount; ++c)
                {
                    ImGui::PushID(c);
                    if (ImGui::Selectable(TR(CategoryLabel(c)), filterCat == c)) filterCat = c;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##gpssearch", "search", search, sizeof(search));
            if (ImGui::Button("Scan markers##gps")) MapTools::ScanAsync();
            if (MapTools::Count() == 0) ImGui::TextDisabled("press Scan markers while in the world");

            struct Item { float dist; MapTools::Row r; int cat; char name[64]; };
            static std::vector<Item> items;
            items.clear();
            for (int i = 0; i < MapTools::Count(); ++i)
            {
                MapTools::Row r{};
                if (!MapTools::At(i, r) || (r.x == 0.f && r.y == 0.f && r.z == 0.f)) continue;
                const int cat = CategoryOf(r);
                if (filterCat >= 0 && cat != filterCat) continue;
                Item it; it.r = r; it.cat = cat;
                const int icon = r.icon ? r.icon : r.uiIcon;
                if (icon > 0) PrettyIcon(icon, it.name, sizeof(it.name));
                else snprintf(it.name, sizeof(it.name), "%s", r.className + (strncmp(r.className, "Wheelman", 8) == 0 ? 8 : 0));
                if (search[0])
                {
                    char hay[160]; snprintf(hay, sizeof(hay), "%s %s %s", it.name, CategoryLabel(cat), r.className);
                    std::string h(hay), q(search);
                    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
                    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
                    if (h.find(q) == std::string::npos) continue;
                }
                it.dist = havePos ? std::sqrt((r.x - px) * (r.x - px) + (r.y - py) * (r.y - py)) * 0.01f : 0.f;
                items.push_back(it);
            }
            std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
            ImGui::TextDisabled(TR("shown: %d of %d markers"), static_cast<int>(items.size()), MapTools::Count());
            ImDrawList* ldl = ImGui::GetWindowDrawList();
            for (size_t i = 0; i < items.size() && i < 400; ++i)
            {
                const Item& it = items[i];
                ImGui::PushID(static_cast<int>(i));
                const bool go = ImGui::Selectable("##row", false, 0, ImVec2(0, 22));
                const ImVec2 rmin = ImGui::GetItemRectMin();
                DrawMarkerIcon(ldl, SpriteFor(it.r), kCats[it.cat].shape, ImVec2(rmin.x + 12.f, rmin.y + 11.f), 6.f, kCats[it.cat].color);
                ldl->AddText(ImVec2(rmin.x + 26.f, rmin.y + 1.f), ImGui::GetColorU32(ImGuiCol_Text), it.name);
                char sub[80]; snprintf(sub, sizeof(sub), "%s", TR(CategoryLabel(it.cat)));
                ldl->AddText(ImVec2(rmin.x + 26.f, rmin.y + 11.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub);
                char dbuf[24]; if (havePos) snprintf(dbuf, sizeof(dbuf), "%.0f m", it.dist); else dbuf[0] = 0;
                const float tw = ImGui::CalcTextSize(dbuf).x;
                ldl->AddText(ImVec2(rmin.x + ImGui::GetContentRegionAvail().x - tw - 4.f, rmin.y + 5.f), ImGui::GetColorU32(ImGuiCol_Text), dbuf);
                if (go) { char nm[80]; snprintf(nm, sizeof(nm), "%s", it.name); Nav::SetWaypoint(it.r.x, it.r.y, nm); }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { followMe = false; cxp = (it.r.y - mapOriginY) / mapCmPerPx; cyp = (mapOriginX - it.r.x) / mapCmPerPx; }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("gpsmap", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImGui::Checkbox("Follow me", &followMe);
            ImGui::SameLine();
            ImGui::TextDisabled("wheel = zoom, drag = move, click / right-click = waypoint, double-click a list row = show it");
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("bigmap", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool hovered = ImGui::IsItemHovered();
            ImGuiIO& io = ImGui::GetIO();
            const ImVec2 mid(p0.x + avail.x * 0.5f, p0.y + avail.y * 0.5f);
            auto toMapPx = [&](float wx, float wy) { return ImVec2((wy - mapOriginY) / mapCmPerPx, (mapOriginX - wx) / mapCmPerPx); };
            if (followMe && havePos) { const ImVec2 m = toMapPx(px, py); cxp = m.x; cyp = m.y; }
            if (hovered && io.MouseWheel != 0.f)
            {
                const float before = zoom, f = io.MouseWheel > 0 ? 1.18f : 1.f / 1.18f;
                zoom = zoom * f; if (zoom > 2.5f) zoom = 2.5f; if (zoom < 0.08f) zoom = 0.08f;
                if (!followMe)   // keep the map point under the cursor fixed
                {
                    cxp += (io.MousePos.x - mid.x) * (1.f / before - 1.f / zoom);
                    cyp += (io.MousePos.y - mid.y) * (1.f / before - 1.f / zoom);
                }
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f))
            {
                followMe = false;
                cxp -= io.MouseDelta.x / zoom; cyp -= io.MouseDelta.y / zoom;
            }
            auto scr = [&](float mx, float my) { return ImVec2(mid.x + (mx - cxp) * zoom, mid.y + (my - cyp) * zoom); };
            auto worldToScr = [&](float wx, float wy) { const ImVec2 m = toMapPx(wx, wy); return scr(m.x, m.y); };

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), true);
            dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), IM_COL32(8, 14, 28, 255));
            if (g_mapTex)
                dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_mapTex)), scr(0, 0), scr(static_cast<float>(kMapW), static_cast<float>(kMapH)));

            const ImVec2 mouse = io.MousePos;
            float bestD = 100.f; char tip[160] = {}; bool haveTip = false;
            const float sc = iconScale < 0.3f ? 0.3f : (iconScale > 4.f ? 4.f : iconScale);
            const MapTools::Row* picked = nullptr; static MapTools::Row pickedRow; char pickedName[64] = {};
            for (int i = 0; i < MapTools::Count(); ++i)
            {
                MapTools::Row r{};
                if (!MapTools::At(i, r) || (r.x == 0.f && r.y == 0.f && r.z == 0.f)) continue;
                const int cat = CategoryOf(r);
                if (showPoliceZone && r.sizeInWorld > 100.f && (r.icon == 24 || r.uiIcon == 24) && r.onMap && !r.disabled)
                {
                    const ImVec2 zc = worldToScr(r.x, r.y);
                    const float zr = r.sizeInWorld * zoneScale / mapCmPerPx * zoom;
                    dl->AddCircleFilled(zc, zr, IM_COL32(60, 110, 255, 45), 64);
                    dl->AddCircle(zc, zr, IM_COL32(90, 140, 255, 200), 64, 2.f);
                }
                if (!catOn[cat]) continue;
                if (cat == CatStory && havePos && std::fabs(r.x - px) < 500.f && std::fabs(r.y - py) < 500.f) continue;   // a mission marker riding on the player
                const ImVec2 p = worldToScr(r.x, r.y);
                if (p.x < p0.x - 10 || p.y < p0.y - 10 || p.x > p0.x + avail.x + 10 || p.y > p0.y + avail.y + 10) continue;
                ImU32 col = kCats[cat].color;
                if (r.hasState && r.state != 2) col = (col & 0x00FFFFFFu) | 0x80000000u;
                DrawMarkerIcon(dl, SpriteFor(r), kCats[cat].shape, p, 5.5f * sc * catScale[cat], col);
                const float mdx = mouse.x - p.x, mdy = mouse.y - p.y;
                if (hovered && mdx * mdx + mdy * mdy < bestD)
                {
                    bestD = mdx * mdx + mdy * mdy; haveTip = true; pickedRow = r; picked = &pickedRow;
                    const int icon = r.icon ? r.icon : r.uiIcon;
                    if (icon > 0) PrettyIcon(icon, pickedName, sizeof(pickedName));
                    else snprintf(pickedName, sizeof(pickedName), "%s", r.className + (strncmp(r.className, "Wheelman", 8) == 0 ? 8 : 0));
                    snprintf(tip, sizeof(tip), "%s  (%s)\nclick to navigate", pickedName, TR(CategoryLabel(cat)));
                }
            }
            if ((catOn[CatPoliceNpc] || catOn[CatEnemyNpc] || catOn[CatAllyNpc] || catOn[CatSpecialNpc]) && havePos)
            {
                static Npc::Info npcs[256];
                const int n = Npc::Snapshot(npcs, 256, false);
                for (int i = 0; i < n; ++i)
                {
                    const int nc = NpcCategory(npcs[i]);
                    if (nc < 0 || !catOn[nc]) continue;
                    const ImVec2 np = worldToScr(npcs[i].x, npcs[i].y);
                    const ImVec2 nf = worldToScr(npcs[i].x + std::cos(npcs[i].yaw) * 100.f, npcs[i].y + std::sin(npcs[i].yaw) * 100.f);
                    float fx = nf.x - np.x, fy = nf.y - np.y; const float fl = std::sqrt(fx * fx + fy * fy);
                    if (fl > 1e-4f) { fx /= fl; fy /= fl; } else { fx = 0.f; fy = -1.f; }
                    DrawNpcDot(dl, nc, np, sc, fx, fy);
                }
            }
            if (Nav::HasWaypoint())
            {
                if (Nav::showRoute)
                {
                    const std::vector<Nav::Point>& rt = Nav::Route();
                    static std::vector<ImVec2> pts;
                    pts.clear();
                    for (const Nav::Point& p : rt) pts.push_back(worldToScr(p.x, p.y));
                    if (pts.size() >= 2)
                    {
                        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(0, 0, 0, 210), 0, 6.f);
                        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), Nav::OnRoads() ? IM_COL32(255, 60, 220, 255) : IM_COL32(255, 160, 60, 255), 0, 3.5f);
                    }
                }
                const Nav::Point w = Nav::Waypoint();
                const ImVec2 wp2 = worldToScr(w.x, w.y);
                dl->AddCircleFilled(wp2, 7.f, IM_COL32(255, 60, 220, 255), 16);
                dl->AddCircle(wp2, 7.f, IM_COL32(255, 255, 255, 255), 16, 2.f);
            }
            if (havePos)
            {
                const ImVec2 pp = worldToScr(px, py);
                const float ux = std::sin(yawRad), uy = -std::cos(yawRad);
                const ImVec2 tip2(pp.x + ux * 12.f, pp.y + uy * 12.f);
                const ImVec2 l(pp.x - uy * 7.f - ux * 7.f, pp.y + ux * 7.f - uy * 7.f), rr(pp.x + uy * 7.f - ux * 7.f, pp.y - ux * 7.f - uy * 7.f);
                dl->AddTriangleFilled(tip2, l, rr, IM_COL32(255, 255, 255, 255));
                dl->AddTriangle(tip2, l, rr, IM_COL32(0, 0, 0, 255), 1.5f);
            }
            dl->PopClipRect();

            // click = waypoint (on the marker under the cursor, else on the free spot); a drag only moves the map
            if (hovered)
            {
                const bool leftClick = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).x +
                                       ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y < 16.f;
                if (leftClick || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    if (picked) Nav::SetWaypoint(picked->x, picked->y, pickedName);
                    else
                    {
                        const float mx = cxp + (mouse.x - mid.x) / zoom, my = cyp + (mouse.y - mid.y) / zoom;
                        Nav::SetWaypoint(mapOriginX - my * mapCmPerPx, mapOriginY + mx * mapCmPerPx, "Map point");
                    }
                }
                if (haveTip) ImGui::SetTooltip("%s", tip);
            }
        }
        ImGui::EndChild();
    }

    void Draw()
    {
        float px = 0, py = 0, pz = 0;
        const bool havePos = Overlay::GetPlayerPosition(px, py, pz);
        if (havePos) Nav::Update(px, py);   // the route also lives while only the big map is used
        if (!enabled) return;

        float yawRad = PlayerHeadingRad();

        // "match the game's minimap": zoom and rotation follow the HUD minimap so that this one can be laid over it
        float range = rangeMeters;
        bool rotateNow = rotate;
        bool matched = false;
        if (matchGame && ReadGameMinimap())
        {
            if (matchScale)
            {
                // m_fMapScale grows when the game's minimap zooms OUT, so the radius grows with it (16 = m per unit of mask width at a typical scale)
                range = (g_maskW * 0.5f * gameMapScale) * 16.f * (matchZoom < 0.05f ? 0.05f : matchZoom);
                matched = true;   // the zoom comes from the game: the mouse wheel is ignored
            }
            if (matchRotation)
            {
                float deg = gameMapYawUnits * (360.f / 65536.f) * (matchYawInvert ? -1.f : 1.f) + matchYawOffsetDeg;
                yawRad = deg * (3.14159265f / 180.f);
                rotateNow = true;
            }
        }

        // markers are collected by a heap walk: once when the map first opens in the world, then every autoScanSeconds
        const ULONGLONG now = GetTickCount64();
        if (havePos && ((!g_scannedOnce && now - g_lastScan > 5000) || (autoScanSeconds > 0 && now - g_lastScan > static_cast<ULONGLONG>(autoScanSeconds) * 1000)))
        {
            g_lastScan = now; g_scannedOnce = true;
            MapTools::ScanAsync();
        }

        const float size = static_cast<float>(sizePx < 120 ? 120 : (sizePx > 800 ? 800 : sizePx));
        ImGui::SetNextWindowSize(ImVec2(size, size), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(30, 200), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Minimap", nullptr, flags);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 center(wp.x + size * 0.5f, wp.y + size * 0.5f);
        const float radius = size * 0.5f - 4.f;

        if (ImGui::IsWindowHovered() && !matched)
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f)
            {
                rangeMeters *= wheel > 0 ? 0.87f : 1.15f;
                if (rangeMeters < 60.f) rangeMeters = 60.f;
                if (rangeMeters > 4000.f) rangeMeters = 4000.f;
            }
        }

        const ImU32 bg = ImGui::GetColorU32(ImVec4(0.03f, 0.035f, 0.04f, opacity));
        // square mode: the visible area is |x|,|y| <= radius around the centre instead of a circle
        auto inside = [&](float ddx, float ddy, float margin)
        {
            if (square) return std::fabs(ddx) <= radius - margin && std::fabs(ddy) <= radius - margin;
            return ddx * ddx + ddy * ddy <= (radius - margin) * (radius - margin);
        };
        // pull an off-map offset back onto the border
        auto toEdge = [&](float ddx, float ddy, float margin)
        {
            const float ax = std::fabs(ddx), ay = std::fabs(ddy);
            const float d = square ? (ax > ay ? ax : ay) : std::sqrt(ddx * ddx + ddy * ddy);
            const float f = d > 1e-4f ? (radius - margin) / d : 1.f;
            return ImVec2(center.x + ddx * f, center.y + ddy * f);
        };
        if (square) dl->AddRectFilled(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), bg, 6.f);
        else dl->AddCircleFilled(center, radius, bg, 64);
        if (background && havePos)
        {
            EnsureMapTexture();
            if (g_mapTex)
            {
                // a fan of triangles over the circle; every rim vertex gets the map UV of the world point under it
                const float cy = std::cos(yawRad), sy = std::sin(yawRad);
                const float pxPerCmScreen = radius / (range * 100.f);
                auto uvAt = [&](float sx, float sy2)
                {
                    const float right = (sx - center.x) / pxPerCmScreen, up = (center.y - sy2) / pxPerCmScreen;   // cm
                    float dx, dy;
                    if (rotateNow) { dx = up * cy - right * sy; dy = up * sy + right * cy; }
                    else { dx = up; dy = right; }
                    const float worldX = px + dx, worldY = py + dy;
                    return ImVec2((worldY - mapOriginY) / mapCmPerPx / kMapW, (mapOriginX - worldX) / mapCmPerPx / kMapH);
                };
                constexpr int N = 72;
                const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>((bgOpacity < 0.f ? 0.f : (bgOpacity > 1.f ? 1.f : bgOpacity)) * 255.f));
                if (square)
                {
                    const ImVec2 a(center.x - radius, center.y - radius), b(center.x + radius, center.y + radius);
                    dl->AddImageQuad(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_mapTex)), a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y),
                                     uvAt(a.x, a.y), uvAt(b.x, a.y), uvAt(b.x, b.y), uvAt(a.x, b.y), tint);
                }
                else {
                dl->PushTexture(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_mapTex)));
                dl->PrimReserve(N * 3, N + 1);
                const ImVec2 cuv = uvAt(center.x, center.y);
                dl->PrimWriteVtx(center, cuv, tint);
                const unsigned base = dl->_VtxCurrentIdx - 1;
                for (int i = 0; i < N; ++i)
                {
                    const float a = i * (6.2831853f / N);
                    const ImVec2 p(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius);
                    dl->PrimWriteVtx(p, uvAt(p.x, p.y), tint);
                }
                for (int i = 0; i < N; ++i)
                {
                    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % N));
                }
                dl->PopTexture();
                }
            }
        }
        // range rings
        const ImU32 ring = ImGui::GetColorU32(ImVec4(1.f, 0.62f, 0.10f, 0.18f));
        const ImU32 border = ImGui::GetColorU32(ImVec4(1.f, 0.62f, 0.10f, 0.85f));
        if (square)
        {
            dl->AddRect(ImVec2(center.x - radius * 0.5f, center.y - radius * 0.5f), ImVec2(center.x + radius * 0.5f, center.y + radius * 0.5f), ring);
            dl->AddRect(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), border, 6.f, 0, 2.f);
        }
        else
        {
            dl->AddCircle(center, radius * 0.5f, ring, 48);
            dl->AddCircle(center, radius, border, 64, 2.f);
        }
        dl->PushClipRect(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), true);

        const float c = std::cos(yawRad), s = std::sin(yawRad);
        const float pxPerM = radius / range;
        // world offset (cm) -> screen offset
        auto toScreen = [&](float dxcm, float dycm)
        {
            const float dx = dxcm * 0.01f, dy = dycm * 0.01f;
            float right, up;
            if (rotateNow) { up = dx * c + dy * s; right = -dx * s + dy * c; }
            else { up = dx; right = dy; }
            return ImVec2(center.x + right * pxPerM, center.y - up * pxPerM);
        };

        const ImU32 black = IM_COL32(0, 0, 0, 220);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        float bestD = 10.f * 10.f; char tip[160] = {}; bool haveTip = false;
        const float sc = iconScale < 0.3f ? 0.3f : (iconScale > 4.f ? 4.f : iconScale);
        if (havePos)
        {
            for (int i = 0; i < MapTools::Count(); ++i)
            {
                MapTools::Row r{};
                if (!MapTools::At(i, r) || (r.x == 0.f && r.y == 0.f && r.z == 0.f)) continue;
                const int cat = CategoryOf(r);
                if (showPoliceZone && r.sizeInWorld > 100.f && (r.icon == 24 || r.uiIcon == 24) && r.onMap && !r.disabled)
                {
                    // police influence zone, like on the game's minimap: a translucent circle
                    const ImVec2 zc = toScreen(r.x - px, r.y - py);
                    const float zr = r.sizeInWorld * 0.01f * zoneScale * pxPerM;
                    dl->AddCircleFilled(zc, zr, IM_COL32(60, 110, 255, 45), 64);
                    dl->AddCircle(zc, zr, IM_COL32(90, 140, 255, 200), 64, 2.f);
                }
                if (!catOn[cat]) continue;
                if (onlyGameVisible && (!r.onMap || r.disabled)) continue;
                if (cat == CatStory && std::fabs(r.x - px) < 500.f && std::fabs(r.y - py) < 500.f) continue;   // a mission marker riding on the player
                ImVec2 p = toScreen(r.x - px, r.y - py);
                if (!inside(p.x - center.x, p.y - center.y, 6.f))
                {
                    if (!kCats[cat].stick) continue;
                    p = toEdge(p.x - center.x, p.y - center.y, 6.f);
                }
                ImU32 col = kCats[cat].color;
                if (r.hasState && r.state != 2) col = (col & 0x00FFFFFFu) | 0x80000000u;   // closed garages / caches are faded
                float sz = 5.f * sc * catScale[cat];
                DrawMarkerIcon(dl, SpriteFor(r), kCats[cat].shape, p, sz, col);
                const float mdx = mouse.x - p.x, mdy = mouse.y - p.y;
                if (mdx * mdx + mdy * mdy < bestD && ImGui::IsWindowHovered())
                {
                    bestD = mdx * mdx + mdy * mdy; haveTip = true;
                    const float meters = std::sqrt((r.x - px) * (r.x - px) + (r.y - py) * (r.y - py)) * 0.01f;
                    const int icon = r.icon ? r.icon : r.uiIcon;
                    snprintf(tip, sizeof(tip), "%s  %.0f m%s\n%s", MapTools::IconName(icon), meters,
                             r.hasState ? (r.state == 2 ? "  [available]" : "  [closed]") : "",
                             r.className + (strncmp(r.className, "Wheelman", 8) == 0 ? 8 : 0));
                }
            }
            if (catOn[CatPoliceNpc] || catOn[CatEnemyNpc] || catOn[CatAllyNpc] || catOn[CatSpecialNpc])
            {
                static Npc::Info npcs[256];
                const int n = Npc::Snapshot(npcs, 256, false);
                int shownCops = 0;
                for (int i = 0; i < n; ++i)
                {
                    const int nc = NpcCategory(npcs[i]);
                    if (nc < 0 || !catOn[nc]) continue;
                    const ImVec2 p = toScreen(npcs[i].x - px, npcs[i].y - py);
                    if (!inside(p.x - center.x, p.y - center.y, 4.f)) continue;
                    if (nc == CatPoliceNpc) ++shownCops;
                    const ImVec2 nf = toScreen(npcs[i].x - px + std::cos(npcs[i].yaw) * 100.f, npcs[i].y - py + std::sin(npcs[i].yaw) * 100.f);
                    float fx = nf.x - p.x, fy = nf.y - p.y; const float fl = std::sqrt(fx * fx + fy * fy);
                    if (fl > 1e-4f) { fx /= fl; fy /= fl; } else { fx = 0.f; fy = -1.f; }
                    DrawNpcDot(dl, nc, p, sc, fx, fy);
                }
                if (shownCops) { char cb[32]; snprintf(cb, sizeof(cb), "police: %d", shownCops); dl->AddText(ImVec2(wp.x + 8.f, wp.y + 6.f), IM_COL32(120, 160, 255, 230), cb); }
            }
        }
        // GPS route and waypoint
        if (havePos && Nav::HasWaypoint())
        {
            if (Nav::showRoute)
            {
                const std::vector<Nav::Point>& rt = Nav::Route();
                if (rt.size() >= 2)
                {
                    static std::vector<ImVec2> pts;
                    pts.clear();
                    for (const Nav::Point& p : rt) pts.push_back(toScreen(p.x - px, p.y - py));
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(0, 0, 0, 200), 0, 5.f * sc);
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), Nav::OnRoads() ? IM_COL32(255, 60, 220, 255) : IM_COL32(255, 160, 60, 255), 0, 3.f * sc);
                }
            }
            const Nav::Point w = Nav::Waypoint();
            ImVec2 wpos = toScreen(w.x - px, w.y - py);
            const float wdx = wpos.x - center.x, wdy = wpos.y - center.y, wd = std::sqrt(wdx * wdx + wdy * wdy);
            (void)wd;
            if (!inside(wdx, wdy, 8.f)) wpos = toEdge(wdx, wdy, 8.f);
            dl->AddCircleFilled(wpos, 6.f * sc, IM_COL32(255, 60, 220, 255), 16);
            dl->AddCircle(wpos, 6.f * sc, IM_COL32(255, 255, 255, 255), 16, 2.f);
        }
        // right-click on the minimap sets a waypoint there
        if (havePos && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const float rx = (mouse.x - center.x) / pxPerM, uy = (center.y - mouse.y) / pxPerM;   // metres
            float dx, dy;
            if (rotateNow) { dx = uy * c - rx * s; dy = uy * s + rx * c; }
            else { dx = uy; dy = rx; }
            Nav::SetWaypoint(px + dx * 100.f, py + dy * 100.f, "Map point");
        }
        dl->PopClipRect();

        // player arrow: always points along the heading (up when the map rotates)
        {
            // north-up: the arrow turns with the heading (forward = (cos, sin) in (X, Y) -> screen (sin, -cos))
            const float ux = rotateNow ? 0.f : std::sin(yawRad), uy = rotateNow ? -1.f : -std::cos(yawRad);
            const ImVec2 tip2(center.x + ux * 11.f, center.y + uy * 11.f);
            const ImVec2 l(center.x - uy * 6.f - ux * 6.f, center.y + ux * 6.f - uy * 6.f);
            const ImVec2 r(center.x + uy * 6.f - ux * 6.f, center.y - ux * 6.f - uy * 6.f);
            dl->AddTriangleFilled(tip2, l, r, IM_COL32(255, 255, 255, 255));
            dl->AddTriangle(tip2, l, r, IM_COL32(0, 0, 0, 255), 1.5f);
        }
        // compass "N": where +X (north) lies on the screen
        {
            // world +X on the screen: (right, up) = (-sin, cos) when the map turns, straight up otherwise
            const float nx = rotateNow ? -std::sin(yawRad) : 0.f;
            const float ny = rotateNow ? -std::cos(yawRad) : -1.f;
            const ImVec2 np = toEdge(nx * radius, ny * radius, 9.f);   // the north marker sits on the border (round or square)
            dl->AddCircleFilled(np, 7.f, IM_COL32(0, 0, 0, 200), 16);
            dl->AddText(ImVec2(np.x - 4.f, np.y - 7.f), IM_COL32(255, 160, 40, 255), "N");
        }
        char label[48];
        snprintf(label, sizeof(label), "%.0f m", range);
        dl->AddText(ImVec2(wp.x + 8.f, wp.y + size - 20.f), IM_COL32(255, 200, 120, 230), label);
        if (havePos && Nav::HasWaypoint())
        {
            char rb[48]; snprintf(rb, sizeof(rb), "GPS %.0f m", Nav::RouteMeters());
            dl->AddText(ImVec2(wp.x + size - 82.f, wp.y + size - 20.f), IM_COL32(255, 120, 235, 240), rb);
        }
        if (!havePos) dl->AddText(ImVec2(center.x - 40.f, center.y - 7.f), IM_COL32(200, 200, 200, 230), TR("no player yet"));
        if (haveTip) ImGui::SetTooltip("%s", tip);
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

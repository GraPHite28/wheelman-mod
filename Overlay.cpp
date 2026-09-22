#include <Windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "imgui/imgui.h"
#include "Overlay.h"
#include "Patches.h"
#include "Offsets.h"
#include "ESP.h"
#include "D3DHook.h"
#include "Police.h"
#include "VehicleMod.h"
#include "BuiltinCheats.h"
#include "MapTools.h"
#include "MiniMap.h"
#include "Nav.h"
#include "LoadGuard.h"
#include "KismetVars.h"
#include "GameConsole.h"
#include "GfxBoost.h"
#include "PostFx.h"
#include "MouseLook.h"
#include "CrimeTools.h"
#include "Npc.h"
#include "Aimbot.h"
#include "NpcMod.h"
#include "Perf.h"
#include "CrashHandler.h"
#include "WeaponMod.h"
#include "Spawner.h"
#include "Binds.h"
#include "Lang.h"
#include "Weapons.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

namespace
{
    // Minimal safety net: g_pVehicle/g_pPlayerPawn can go stale the moment the
    // player leaves the car / respawns. Reading/writing through them is
    // wrapped in SEH so a stale pointer shows "n/a" instead of crashing the game.
    bool SafeReadInt(void* base, int offset, int32_t& out)
    {
        __try { out = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(base) + offset); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeReadFloat(void* base, int offset, float& out)
    {
        __try { out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + offset); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeReadByte(void* base, int offset, uint8_t& out)
    {
        __try { out = *(reinterpret_cast<uint8_t*>(base) + offset); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeWriteByte(void* base, int offset, uint8_t value)
    {
        __try { *(reinterpret_cast<uint8_t*>(base) + offset) = value; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeWriteInt(void* base, int offset, int32_t value)
    {
        __try { *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(base) + offset) = value; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeWriteFloat(void* base, int offset, float value)
    {
        __try { *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + offset) = value; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // CONFIRMED paint color index - see Offsets::Vehicle_PaintObjectPtr for
    // the full story. Not in the flat Fields tab since that only reads
    // directly off the vehicle, not through a pointer.
    bool ReadPaintIndex(void* vehicle, int32_t& out)
    {
        void* target = nullptr;
        if (!SafeReadInt(vehicle, Offsets::Vehicle_PaintObjectPtr, reinterpret_cast<int32_t&>(target))) return false;
        if (!target) return false;
        return SafeReadInt(target, Offsets::Vehicle_PaintIndexOffset, out);
    }

    // Unlike the physics mirrors (position/velocity), this field lives on a
    // separately-allocated paint/material object, not on the vehicle's own
    // Havok-synced block - worth trying a live write to see if the renderer
    // just reads it directly (no reallocation/game-logic round-trip needed).
    bool WritePaintIndex(void* vehicle, int32_t value)
    {
        void* target = nullptr;
        if (!SafeReadInt(vehicle, Offsets::Vehicle_PaintObjectPtr, reinterpret_cast<int32_t&>(target))) return false;
        if (!target) return false;
        return SafeWriteInt(target, Offsets::Vehicle_PaintIndexOffset, value);
    }

    // Every dword in this range on the player's own vehicle can be toggled
    // on in the Fields tab; whichever ones are on get shown - live, on every
    // vehicle the ESP draws, not just the player's - so specific numbers can
    // be compared visually between different real cars (e.g. two different
    // colors of the same model). Range covers everything looked at so far,
    // from just past position/velocity through the tail end of what the
    // repaint diff test touched (see Patches.cpp SnapshotVehicleForDiff).
    constexpr int kFieldRangeStart = 0x100;
    constexpr int kFieldRangeEnd = 0xA80;
    constexpr int kFieldCount = (kFieldRangeEnd - kFieldRangeStart) / 4;
    bool g_fieldShown[kFieldCount] = {};

    // Same raw-dword browser as above, but for the player pawn - reuses the
    // exact same range the CE-style ScanSnapshot/ScanNarrow scanner already
    // covers (Patches.h kScanRangeBytes), so any candidate offset that
    // scanner narrows down to can immediately be ticked here to watch it
    // live while playing (jumping, taking damage, picking up ammo/boost,
    // etc.) instead of only seeing a bare "+0xNNN" bullet in the candidate
    // list. No ESP cross-reference here (there's only one local player) -
    // the checkbox instead just pins a field to the top so it isn't lost
    // while scrolling past ~2000 others.
    constexpr int kPlayerFieldRangeStart = 0x0;
    constexpr int kPlayerFieldRangeEnd = kScanRangeBytes;
    constexpr int kPlayerFieldCount = (kPlayerFieldRangeEnd - kPlayerFieldRangeStart) / 4;
    bool g_playerFieldPinned[kPlayerFieldCount] = {};

    // Confirmed by live visual correlation while driving (see Offsets.h) -
    // 3/4 are known to exist in the game's own code but weren't observed
    // among ordinary traffic, so their real-world meaning is still unknown.
    const char* VehicleCategoryLabel(uint8_t category)
    {
        switch (category)
        {
        case 0: return "Car";
        case 1: return "Moto";
        case 2: return "Truck";
        default:
        {
            static char buf[16];
            snprintf(buf, sizeof(buf), "Type %u", category);
            return buf;
        }
        }
    }
}

namespace Overlay
{
    bool visible = true;
    bool wantMouseCapture = false;
    bool speedometerVisible = true;
    bool debugEnabled = false;    // Settings -> Debug: unlocks the Debug tab
    bool unsafeEnabled = false;   // Settings -> Unsafe: unlocks experimental options (stub vehicle definitions)
    namespace { float g_speedometerScale = 2.0f; }
    namespace
    {
        int g_catPick = -1;              // selected catalog entry (saved in configs)
        bool g_catShowSpecial = false;   // list the train / helicopter / trailer rigs
        void ActSpawnSelected() { if (g_catPick >= 0) Spawner::RequestCatalog(g_catPick, false); }
    }

    void DrawSpeedometer()
    {
        if (!speedometerVisible) return;

        float speed = -1.f;
        if (g_pVehicle)
        {
            float vx = 0.f, vy = 0.f, vz = 0.f;
            if (SafeReadFloat(g_pVehicle, Offsets::Vehicle_VelX, vx) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_VelY, vy) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_VelZ, vz) &&
                std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz))
            {
                speed = sqrtf(vx * vx + vy * vy + vz * vz);
            }
        }

        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::Begin("Speed", nullptr, flags);
        if (ImGui::IsWindowHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f)
            {
                g_speedometerScale += wheel * 0.15f;
                if (g_speedometerScale < 0.5f) g_speedometerScale = 0.5f;
                if (g_speedometerScale > 6.0f) g_speedometerScale = 6.0f;
            }
        }
        ImGui::SetWindowFontScale(g_speedometerScale);
        if (speed >= 0.f)
        {
            // UE3's standard world scale is 1 Unreal Unit = 1 cm (used by
            // nearly every UE3 title), so UU/sec -> km/h is *0.01 (-> m/s)
            // *3.6 (-> km/h) = *0.036. Not independently verified against an
            // in-game speedometer for this exact build, but this is the
            // engine-wide default and gives plausible driving speeds.
            constexpr float kUUPerSecToKmh = 0.036f;
            ImGui::Text("%.0f km/h", speed * kUUPerSecToKmh);
        }
        else
            ImGui::TextDisabled("--");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::End();
    }

    // ---- Mission timers ---------------------------------------------------
    // Countdown objects seen by the game's timer tick within the last moment
    // (Patches.cpp Detour_Timer). +0xEC bit0 = running, +0xF4 = duration,
    // +0xFC = time remaining.
    float g_timerAddSeconds = 30.f;

    void DrawTimerSection()
    {
        ImGui::SeparatorText("Mission timer");
        Binds::Check("Freeze timers (countdowns stop)", &g_patches.freezeTimer);

        DWORD now = GetTickCount();
        int shown = 0;
        for (int i = 0; i < g_timerCount; ++i)
        {
            void* obj = g_timers[i].ptr;
            if (!obj || now - g_timers[i].lastSeenTick > 1500) continue;
            float remaining = 0.f, duration = 0.f;
            int32_t flags = 0;
            if (!SafeReadFloat(obj, 0xFC, remaining) || !SafeReadFloat(obj, 0xF4, duration) || !SafeReadInt(obj, 0xEC, flags)) continue;
            if (!std::isfinite(remaining) || remaining < 0.f || remaining > 1.0e6f) continue;
            if (!(flags & 1)) continue; // not running
            ++shown;
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputFloat("##rem", &remaining, 0.f, 0.f, "%.1f s"))
                SafeWriteFloat(obj, 0xFC, remaining < 0.f ? 0.f : remaining);
            ImGui::SameLine();
            if (ImGui::Button("+")) SafeWriteFloat(obj, 0xFC, remaining + g_timerAddSeconds);
            ImGui::SameLine();
            if (ImGui::Button("Refill")) SafeWriteFloat(obj, 0xFC, duration);
            ImGui::SameLine();
            ImGui::TextDisabled("of %.0f s  @%08X", duration, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj)));
            ImGui::PopID();
        }
        if (!shown) ImGui::TextDisabled("(no running countdown right now - start a timed mission)");
        ImGui::SetNextItemWidth(110);
        ImGui::InputFloat("seconds added by [+]", &g_timerAddSeconds, 5.f, 30.f, "%.0f");
    }

    // ---- Police / wanted level ---------------------------------------------
    extern "C" volatile long g_policeTickCalls;

    void DrawPoliceSection()
    {
        ImGui::SeparatorText("Police / wanted level");
        {
            static long lastCalls = 0; static ULONGLONG lastAt = 0; static float perSec = 0.f;
            ULONGLONG now = GetTickCount64();
            if (now - lastAt >= 1000)
            {
                long calls = g_policeTickCalls;
                perSec = (calls - lastCalls) * 1000.f / static_cast<float>(now - lastAt);
                lastCalls = calls; lastAt = now;
            }
            if (debugEnabled) ImGui::TextDisabled("Works on foot and in vehicles. Police subsystem tick: %.0f calls/s (level changes are also applied from the frame hook).", perSec);
        }
        Police::State st = Police::Read();
        if (!st.found)
        {
            if (ImGui::Button("Find police system")) Police::Refresh();
            ImGui::SameLine();
            ImGui::TextDisabled("not found (load into the world first)");
        }
        else
        {
            ImGui::Text(TR("Wanted level: %d / %d   (heat %.2f)   %s"), st.level, st.maxLevel, st.percent,
                        st.lostState == 0 ? "wanted" : "clean");
            if (!st.enabled) ImGui::TextDisabled("police system currently switched off");
        }

        Binds::Check("Police ignore me (never wanted)", &Police::neverWanted);
        Binds::Check("Police never shoot or arrest me", &Police::peaceful);

        static bool allowAbove5 = false;
        ImGui::BeginDisabled(Police::neverWanted);
        if (debugEnabled) Binds::Check("Allow levels above 5 (may misbehave / crash)", &allowAbove5);
        else allowAbove5 = false;
        if (!allowAbove5 && Police::lockValue > 5) Police::lockValue = 5;
        if (allowAbove5)
            ImGui::InputInt("Wanted level to set", &Police::lockValue, 1, 5);
        else
            ImGui::SliderInt("Wanted level to set", &Police::lockValue, 0, 5);
        if (Police::lockValue < 0) Police::lockValue = 0;
        if (Police::lockValue > 99) Police::lockValue = 99;
        Binds::Action("Set level", "police_set");
        ImGui::SameLine();
        Binds::Check("keep locked", &Police::lockLevel);
        ImGui::EndDisabled();

        Binds::Action("Lose the police now", "police_lose");
    }
    void DrawPerformanceSection()
    {
        ImGui::SeparatorText("Performance");
        Binds::Check("Limit FPS (the game ties logic to the frame rate)", &Perf::limitEnabled);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Target FPS", &Perf::targetFps, 20, 240);
        ImGui::Text(TR("Current FPS: %.0f"), Perf::measuredFps);
    }

    // Garages (class WheelmanGarage, derives from WheelmanUnlockable). State values seen in game: 0 = locked (not yet
    // reached in the story), 2 = available (the garage the player stood next to).
    void DrawGaragesSection()
    {
        ImGui::SeparatorText("Garages");
        ImGui::TextDisabled("Garages (class WheelmanGarage) repair the car, and unlock as the story goes. Use Vehicle -> General -> Repair for a remote repair.");
        if (ImGui::Button("Scan garages")) BuiltinCheats::ScanGarages();
        ImGui::SameLine();
        Binds::Action("Unlock all garages", "garages_unlock");
        Binds::Check("Garages always open (forces state 2, also when temporarily closed)", &BuiltinCheats::garagesAlwaysOpen);
        float px = 0, py = 0, pz = 0;
        const bool havePos = GetPlayerPosition(px, py, pz);
        if (BuiltinCheats::GarageCount() == 0) ImGui::TextDisabled("press Scan garages while in the world");
        else if (ImGui::BeginTable("garages", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Distance (m)");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("In use");
            ImGui::TableSetupColumn("Mission garage");
            ImGui::TableHeadersRow();
            for (int i = 0; i < BuiltinCheats::GarageCount(); ++i)
            {
                BuiltinCheats::GarageInfo g{};
                if (!BuiltinCheats::GarageAt(i, g)) continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", i + 1);
                ImGui::TableNextColumn();
                if (havePos) ImGui::Text("%.0f", std::sqrt((g.x - px) * (g.x - px) + (g.y - py) * (g.y - py)) / 100.f); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                {
                    const char* word = g.state == 0 ? "locked" : (g.state == 1 ? "unavailable" : (g.state == 2 ? "available" : (g.state == 3 ? "closed for now" : "state?")));
                    ImGui::Text("%d (%s)", g.state, TR(word));
                }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g.inUse ? "yes" : "no");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g.missionGarage ? "yes" : "no");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (debugEnabled)
        {
            static int state = 2;
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("garage state value", &state);
            ImGui::SameLine();
            if (ImGui::Button("Set on all garages")) BuiltinCheats::SetAllGarageStates(state);
        }
        if (BuiltinCheats::garageMessage[0]) ImGui::TextDisabled("%s", BuiltinCheats::garageMessage);

        ImGui::SeparatorText("Weapon and ammo caches");
        ImGui::TextDisabled("Caches are unlockables like the garages (state 2 = available; after use they cool down). They have no icon on the game's map by default.");
        if (ImGui::Button("Scan caches")) MapTools::ScanAsync();
        ImGui::SameLine();
        Binds::Action("Unlock all caches", "caches_unlock");
        Binds::Check("Caches always available (no cool-down)", &MapTools::cachesAlwaysOpen);
        ImGui::BeginDisabled(!unsafeEnabled);
        Binds::Check("Show the caches on the game's map (ammo stash icon)", &MapTools::showCachesOnMap);
        ImGui::EndDisabled();
        if (!unsafeEnabled) ImGui::TextDisabled("(changes the game's own map: needs the Unsafe switch in Settings)");
        {
            int total = 0, avail = 0;
            struct CRow { float dist; MapTools::Row r; };
            static std::vector<CRow> rows;
            rows.clear();
            float cpx = 0, cpy = 0, cpz = 0;
            const bool cHave = GetPlayerPosition(cpx, cpy, cpz);
            for (int i = 0; i < MapTools::Count(); ++i)
            {
                MapTools::Row r{};
                if (!MapTools::At(i, r) || !strstr(r.className, "Cache") || (r.x == 0.f && r.y == 0.f)) continue;
                ++total; if (r.state == 2) ++avail;
                rows.push_back({ cHave ? std::sqrt((r.x - cpx) * (r.x - cpx) + (r.y - cpy) * (r.y - cpy)) / 100.f : 0.f, r });
            }
            std::sort(rows.begin(), rows.end(), [](const CRow& a, const CRow& b) { return a.dist < b.dist; });
            ImGui::TextDisabled(TR("caches found: %d, available: %d"), total, avail);
            if (!rows.empty() && ImGui::BeginTable("caches", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 180)))
            {
                ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Distance (m)"); ImGui::TableSetupColumn("##go");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    const CRow& c = rows[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(c.r.className + 8);
                    ImGui::TableNextColumn(); ImGui::Text("%d", c.r.state);
                    ImGui::TableNextColumn(); if (cHave) ImGui::Text("%.0f", c.dist); else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); if (ImGui::SmallButton("GPS")) Nav::SetWaypoint(c.r.x, c.r.y, "Weapon cache");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    // World tab: things that belong to the game world rather than to the player or a vehicle.
    void DrawPoliceTab();
    void DrawMissionTab();
    void DrawKismetSection();
    void DrawWorldTab()
    {
        if (!ImGui::BeginTabBar("WorldTabs")) return;
        if (ImGui::BeginTabItem("Police")) { DrawPoliceTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Missions")) { DrawMissionTab(); DrawTimerSection(); if (debugEnabled) DrawKismetSection(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Garages")) { DrawGaragesSection(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Performance")) { DrawPerformanceSection(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    void DrawHudSection()
    {
        ImGui::SeparatorText("HUD");
        Binds::Check("Show speedometer overlay", &speedometerVisible);
        ImGui::TextDisabled("Drag its title bar to move it, scroll wheel over it to resize.");
        if (Binds::Check("Show the active binds window", &Binds::showWindow)) Binds::SaveSettings();
    }

    // ---- Vehicle tab -------------------------------------------------------
    bool BitCheckbox(void* base, int off, int bit, const char* label)
    {
        int32_t v = 0;
        if (!SafeReadInt(base, off, v)) return false;
        bool on = (static_cast<uint32_t>(v) >> bit) & 1u;
        if (Binds::Check(label, &on))
            SafeWriteInt(base, off, static_cast<int32_t>(on ? (static_cast<uint32_t>(v) | (1u << bit)) : (static_cast<uint32_t>(v) & ~(1u << bit))));
        return true;
    }

    bool FloatField(void* base, int off, const char* label, float step)
    {
        float v = 0.f;
        if (!SafeReadFloat(base, off, v) || !std::isfinite(v)) return false;
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputFloat(label, &v, step, step * 10.f, "%.3g"))
            SafeWriteFloat(base, off, v);
        return true;
    }

    void DrawVehicleGeneral()
    {
        const bool inCar = g_pVehicle && IsLiveVehicle(g_pVehicle, true);
        if (!inCar) ImGui::TextDisabled("Not in a vehicle: the options below are armed and apply as soon as you get into one.");

        ImGui::SeparatorText("Repair");
        if (inCar)
        {
            int32_t health = 0;
            if (SafeReadInt(g_pVehicle, Offsets::Vehicle_Health, health) &&
                ImGui::InputInt("Vehicle health (+0x2AC)", &health, 1, 100))
                SafeWriteInt(g_pVehicle, Offsets::Vehicle_Health, health);
        }
        Binds::Action("Repair vehicle (garage-style)", "repair");
        ImGui::SameLine();
        Binds::Action("Emergency exit", "exit_vehicle");
        ImGui::SameLine();
        Binds::Action("Delete my vehicle", "delete_vehicle");
        ImGui::TextDisabled(TR("%s   (Emergency exit gets you out of a vehicle you are stuck in - bind it to a key)"), VehicleMod::repairMessage);

        ImGui::SeparatorText("Durability");
        Binds::Check("Vehicle immortality (health forced to 40000 every frame)", &g_patches.vehicleGodMode);
        Binds::Check("No collision damage (health bar)", &VehicleMod::noCollisionDamage);
        Binds::Check("No body deformation", &VehicleMod::noDeformation);
        Binds::Check("Bullet proof (tyres, glass, panels, engine, fuel tank)", &VehicleMod::bulletproof);
        Binds::Check("Tyres can't be destroyed", &VehicleMod::invulnerableTyres);

        ImGui::SeparatorText("Driving");
        Binds::Check("Can't fall off a motorcycle", &VehicleMod::noBikeFall);
        Binds::Check("Infinite boost / nitro (Mana)", &g_patches.infiniteMana);
        Binds::Check("Ram Boom - ramming deals bonus damage", &g_patches.ramBoom);
        Binds::Check("Jack any vehicle (car-to-car and on-foot jacking on every tracked vehicle)", &VehicleMod::jackAny);
        Binds::Check("Jack: ignore the passenger rules (cars with 2+ people)", &VehicleMod::jackIgnoreOccupants);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", VehicleMod::jackGateStatus);
        Binds::Check("Jump between cars: relax the conditions (angle, speeds, longer jump arc)", &VehicleMod::jackAnywhere);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", VehicleMod::jackAnywhereStatus);
        if (inCar) ImGui::Text(TR("Speed: %.1f units/sec"), g_vehicleSpeed);

        ImGui::SeparatorText("Horn / siren");
        Binds::Check("Horn (bind it, mode Hold = honk while the key is held)", &VehicleMod::hornOn);
        Binds::Check("Siren (sound + lights, works on vehicles that have a siren)", &VehicleMod::sirenOn);
        Binds::Check("Siren lights only", &VehicleMod::sirenLightsOn);
    }

    // Vehicle -> Abilities: the raw ability bits and multipliers of the vehicle the player is in.
    void DrawVehicleAbilities()
    {
        if (!g_pVehicle || !IsLiveVehicle(g_pVehicle, true)) { ImGui::TextDisabled("(get in a car first)"); return; }

        ImGui::SeparatorText("Abilities");
        BitCheckbox(g_pVehicle, 0x700, 12, "Boost enabled");
        BitCheckbox(g_pVehicle, 0x700, 10, "Cyclone enabled");
        BitCheckbox(g_pVehicle, 0x700, 9, "Focus power enabled");
        BitCheckbox(g_pVehicle, 0x6C8, 9, "Car-to-car jacking allowed");
        BitCheckbox(g_pVehicle, 0x6C8, 10, "On-foot jacking allowed");

        ImGui::SeparatorText("Damage / power");
        FloatField(g_pVehicle, 0x738, "Ram (melee) mass multiplier", 0.5f);
        FloatField(g_pVehicle, 0x6F4, "Armour plating damage modifier", 0.1f);
        FloatField(g_pVehicle, 0x6D0, "Super power multiplier", 0.1f);
        FloatField(g_pVehicle, 0x6D4, "Focus mode damage multiplier", 0.1f);
        FloatField(g_pVehicle, 0x6CC, "Stability multiplier (power)", 0.1f);
        FloatField(g_pVehicle, 0xA1C, "Rampage damage scalar", 0.1f);
        FloatField(g_pVehicle, 0x68C, "Collision damage scaling", 0.1f);
        int32_t mh = 0;
        if (SafeReadInt(g_pVehicle, 0x628, mh) && ImGui::InputInt("Max health (+0x628)", &mh, 100, 1000))
            SafeWriteInt(g_pVehicle, 0x628, mh);
        if (SafeReadInt(g_pVehicle, 0x6E8, mh) && ImGui::InputInt("Min health (+0x6E8)", &mh, 10, 100))
            SafeWriteInt(g_pVehicle, 0x6E8, mh);

    }

    void DrawVehicleTuning()
    {
        ImGui::SeparatorText("Top speed lock");
        Binds::Check("Keep the top speed limit (written back every frame: mini cut-scenes reset it)", &VehicleMod::topSpeedLock);
        {
            float kmh = VehicleMod::topSpeedMs * 3.6f;
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputFloat("Locked top speed (km/h, 0 = take the current limit)", &kmh, 10.f, 50.f, "%.0f")) VehicleMod::topSpeedMs = kmh > 0.f ? kmh / 3.6f : 0.f;
            float cur = 0.f;
            if (VehicleMod::CurrentTopSpeed(cur))
            {
                ImGui::TextDisabled(TR("the vehicle's limit now: %.0f km/h"), cur * 3.6f);
                ImGui::SameLine();
                if (ImGui::SmallButton("Lock this value")) { VehicleMod::topSpeedMs = cur; VehicleMod::topSpeedLock = true; }
            }
            else ImGui::TextDisabled("(get in a car to see its limit)");
        }
        if (!g_pVehicle || !IsLiveVehicle(g_pVehicle, true)) { ImGui::TextDisabled("(get in a car first)"); return; }
        uintptr_t prof = VehicleMod::Profile();
        if (!prof)
        {
            ImGui::TextDisabled("tuning profile not resolved for this vehicle");
            return;
        }
        if (debugEnabled)
        {
            ImGui::Text(TR("Profile @ 0x%08X (shared by every car of this model)"), static_cast<uint32_t>(prof));
            ImGui::TextDisabled("Changes hit the running physics objects. Every car of the same model is affected.");
        }
        const char* group = nullptr;
        for (int i = 0; i < VehicleMod::kTuningCount; ++i)
        {
            const VehicleMod::TuningField& f = VehicleMod::kTuning[i];
            // gearbox, aerodynamics and the raw simulation fields are for developers
            if (!debugEnabled && (strcmp(f.group, "Transmission") == 0 || strcmp(f.group, "Aerodynamics") == 0 || strcmp(f.group, "Vehicle sim") == 0 || strcmp(f.group, "Profile") == 0)) continue;
            if (!group || strcmp(group, f.group) != 0) { ImGui::SeparatorText(f.group); group = f.group; }
            uintptr_t addr = VehicleMod::FieldAddress(prof, i);
            if (!addr) continue;
            ImGui::PushID(i);
            FloatField(reinterpret_cast<void*>(addr), 0, f.name, f.step);
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Button("Reset to original values")) VehicleMod::ResetTuning();
    }

    // Vehicle -> Spawn. Spawning through the catalog is a normal feature; only spawning unloaded definitions ("stubs")
    // stays behind the Unsafe switch in Settings.
    void DrawVehicleSpawn()
    {
        if (unsafeEnabled)
            ImGui::TextColored(ImVec4(1.f, 0.66f, 0.16f, 1.f), "Unsafe options are on: unloaded definitions (stubs) are listed and can crash the game.");
        ImGui::SeparatorText("Catalog: any vehicle (loads its package)");
        {
            static char filter[40] = {};
            int& pick = g_catPick;
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##catfilter", "filter (e.g. police, astra, boss)", filter, sizeof(filter));
            ImGui::SameLine();
            Binds::Check("show special (train/helicopter/trailers)", &g_catShowSpecial);
            auto lower = [](const char* s, char* out, size_t cap)
            {
                size_t i = 0;
                for (; s[i] && i + 1 < cap; ++i) out[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
                out[i] = 0;
            };
            char f[40]; lower(filter, f, sizeof(f));
            Spawner::CatalogInfo cur{};
            char curLbl[200] = "(pick a vehicle)";
            if (Spawner::CatalogAt(pick, cur))
            {
                if (debugEnabled) snprintf(curLbl, sizeof(curLbl), "%s   [%s]", cur.name, cur.path);
                else snprintf(curLbl, sizeof(curLbl), "%s", cur.name);
            }
            ImGui::SetNextItemWidth(420);
            if (ImGui::BeginCombo("Vehicle", curLbl))
            {
                for (int k = 0; k < Spawner::CatalogCount(); ++k)
                {
                    const int i = Spawner::CatalogSorted(k);
                    Spawner::CatalogInfo c{};
                    if (!Spawner::CatalogAt(i, c) || (c.special && !g_catShowSpecial)) continue;
                    if (f[0])
                    {
                        char l[200], p[200];
                        lower(c.name, l, sizeof(l)); lower(c.path, p, sizeof(p));
                        if (!strstr(l, f) && !strstr(p, f)) continue;
                    }
                    char row[240];
                    if (debugEnabled) snprintf(row, sizeof(row), "%s   [%s]", c.name, c.path);
                    else snprintf(row, sizeof(row), "%s", c.name);
                    ImGui::PushID(i);
                    if (ImGui::Selectable(row, i == pick)) pick = i;
                    if (i == pick) ImGui::SetItemDefaultFocus();   // the list opens scrolled to the current choice
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (Spawner::CatalogAt(pick, cur))
            {
                if (debugEnabled) ImGui::TextDisabled("%s  -  carried by %s.xxx%s", cur.path, cur.pkg, cur.special ? "   [special rig - may crash]" : "");
                else if (cur.special) ImGui::TextDisabled("special rig - may crash");
                Binds::Action("Spawn 9 m in front of me", "spawn_selected");
                if (debugEnabled)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Only load / find")) Spawner::RequestCatalog(pick, true);
                }
            }
            if (Spawner::catalogMessage[0]) ImGui::Text("%s", Spawner::catalogMessage);
            if (debugEnabled && Spawner::catalogDef) ImGui::TextDisabled("template object @ 0x%08X", static_cast<uint32_t>(Spawner::catalogDef));
            if (debugEnabled) ImGui::TextDisabled("Needs a natural game spawn first (captures the factory) and at least one known vehicle definition.");
        }
        ImGui::SeparatorText("Spawned vehicles (cleanup)");
        {
            ImGui::Text(TR("tracked: %d   removed so far: %d"), Spawner::SpawnedCount(), Spawner::removedTotal);
            if (ImGui::Button("Remove all spawned")) Spawner::RequestRemoveAll();
            ImGui::SameLine();
            if (ImGui::Button("Remove far ones")) Spawner::RequestRemoveFar();
            ImGui::TextDisabled("(the car you sit in is never removed; 'far' = beyond the keep-near distance)");
            if (unsafeEnabled) Binds::Check("auto-remove spawned vehicles", &Spawner::autoRemove);
            else ImGui::TextDisabled("Auto-remove is always on. Turning it off is an Unsafe option (Settings): leftover vehicles can crash the game.");
            ImGui::SetNextItemWidth(220);
            ImGui::SliderInt("max spawned vehicles", &Spawner::maxSpawned, 1, 40);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderFloat("remove when farther than (m)", &Spawner::autoRemoveMeters, 100.f, 2000.f, "%.0f");
            Binds::Check("never auto-remove vehicles near me", &Spawner::keepNear);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderFloat("near distance (m)", &Spawner::keepNearMeters, 20.f, 400.f, "%.0f");
        }
        ImGui::SeparatorText("Spawn vehicle");
        {
            // Models the game spawned by itself (remembered by the factory hook): available on foot too.
            static int seenPick = 0;
            const int sc = Spawner::SeenCount();
            if (sc == 0) ImGui::TextDisabled("no remembered models yet - they are collected as the game spawns traffic around you");
            else
            {
                if (seenPick >= sc) seenPick = 0;
                Spawner::SeenModel sm{};
                Spawner::SeenAt(seenPick, sm);
                char lbl[80];
                snprintf(lbl, sizeof(lbl), "model #%d  def 0x%08X  seen %u x", seenPick + 1, static_cast<uint32_t>(sm.def), sm.count);
                ImGui::SetNextItemWidth(320);
                if (ImGui::BeginCombo("Remembered model", lbl))
                {
                    for (int i = 0; i < sc; ++i)
                    {
                        Spawner::SeenModel m{};
                        if (!Spawner::SeenAt(i, m)) continue;
                        char l[80];
                        snprintf(l, sizeof(l), "model #%d  def 0x%08X  seen %u x", i + 1, static_cast<uint32_t>(m.def), m.count);
                        ImGui::PushID(i); if (ImGui::Selectable(l, i == seenPick)) seenPick = i; if (i == seenPick) ImGui::SetItemDefaultFocus(); ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Spawn remembered model 9 m in front of me")) Spawner::RequestPair(sm.cls, sm.def);
            }
        }
        {
            // Every vehicle definition in memory (the game has no readable names for them, so they are labelled by
            // their name index and the health of a live vehicle using them).
            static int defPick = 0;
            static bool includeStubs = false;
            if (ImGui::Button("Scan vehicle definitions")) { Spawner::ScanDefs(); defPick = 0; }
            ImGui::SameLine();
            if (unsafeEnabled) Binds::Check("show stubs (body 0x0, cannot be spawned)", &includeStubs);
            else includeStubs = false;
            int idx[128], n = 0;
            for (int i = 0; i < Spawner::DefCount() && n < 128; ++i)
            {
                Spawner::DefInfo d{};
                if (Spawner::DefAt(i, d) && (d.populated || includeStubs)) idx[n++] = i;
            }
            // Group by body so entries that look the same (same model, different variant) sit together.
            for (int a = 1; a < n; ++a)
            {
                const int cur = idx[a];
                Spawner::DefInfo dc{}; Spawner::DefAt(cur, dc);
                int b = a - 1;
                for (; b >= 0; --b)
                {
                    Spawner::DefInfo db{}; Spawner::DefAt(idx[b], db);
                    if (db.body < dc.body || (db.body == dc.body && db.nameIdx <= dc.nameIdx)) break;
                    idx[b + 1] = idx[b];
                }
                idx[b + 1] = cur;
            }
            auto label = [&](int di, char* out, size_t cap)
            {
                Spawner::DefInfo d{};
                Spawner::DefAt(di, d);
                int users = 0; int32_t hp = 0;
                for (int k = 0; k < g_vehicleListCount; ++k)
                {
                    void* v = g_vehicleList[k].ptr;
                    int32_t def = 0;
                    if (GetTickCount() - g_vehicleList[k].lastSeenTick < 5000 && IsLiveVehicle(v) && SafeReadInt(v, 0xA00, def) &&
                        static_cast<uintptr_t>(static_cast<uint32_t>(def)) == d.def)
                    { ++users; SafeReadInt(v, 0x2AC, hp); }
                }
                const char* user = Spawner::LabelFor(d.nameIdx, d.family);
                if (users) snprintf(out, cap, "%s%sbody 0x%X  type 0x%X/0x%X  hp %d (%d in view)%s", user, user[0] ? "  " : "", d.body, d.nameIdx, d.family, hp, users, d.populated ? "" : "  [stub]");
                else       snprintf(out, cap, "%s%sbody 0x%X  type 0x%X/0x%X%s", user, user[0] ? "  " : "", d.body, d.nameIdx, d.family, d.populated ? "" : "  [stub]");
            };
            static bool collapse = true;
            Binds::Check("one entry per model (same body = same look, variants in a second list)", &collapse);
            auto bodyOf = [&](int k) { Spawner::DefInfo x{}; Spawner::DefAt(idx[k], x); return x.body; };
            int grp[129], gn = 0;   // grp[g] = first position in idx[] of group g, grp[gn] = n
            for (int i = 0; i < n; ++i)
                if (!collapse || i == 0 || bodyOf(i) == 0 || bodyOf(i) != bodyOf(i - 1)) grp[gn++] = i;
            grp[gn] = n;
            if (n == 0) ImGui::TextDisabled("press Scan to list the vehicle definitions currently loaded");
            else
            {
                if (defPick >= gn) defPick = 0;
                auto groupLabel = [&](int g, char* out, size_t cap)
                {
                    const int a = grp[g], b = grp[g + 1];
                    if (b - a == 1) { label(idx[a], out, cap); return; }
                    Spawner::DefInfo f{}; Spawner::DefAt(idx[a], f);
                    const char* user = "";
                    for (int k = a; k < b && !user[0]; ++k) { Spawner::DefInfo x{}; Spawner::DefAt(idx[k], x); user = Spawner::LabelFor(x.nameIdx, x.family); }
                    snprintf(out, cap, "%s%sbody 0x%X  (%d variants)", user, user[0] ? "  " : "", f.body, b - a);
                };
                char cur[160]; groupLabel(defPick, cur, sizeof(cur));
                ImGui::SetNextItemWidth(420);
                if (ImGui::BeginCombo("Model", cur))
                {
                    for (int g = 0; g < gn; ++g)
                    {
                        char l[160]; groupLabel(g, l, sizeof(l));
                        ImGui::PushID(g); if (ImGui::Selectable(l, g == defPick)) defPick = g; if (g == defPick) ImGui::SetItemDefaultFocus(); ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                static int variantPick = 0, lastGroup = -1;
                if (lastGroup != defPick) { variantPick = 0; lastGroup = defPick; }
                const int gStart = grp[defPick], gCount = grp[defPick + 1] - grp[defPick];
                if (variantPick >= gCount) variantPick = 0;
                if (gCount > 1)
                {
                    // Variants of one model: same look, different tuning (the tougher mission versions have far more health).
                    char vcur[160]; label(idx[gStart + variantPick], vcur, sizeof(vcur));
                    ImGui::SetNextItemWidth(420);
                    if (ImGui::BeginCombo("Variant", vcur))
                    {
                        for (int v = 0; v < gCount; ++v)
                        {
                            char l[160]; label(idx[gStart + v], l, sizeof(l));
                            ImGui::PushID(v); if (ImGui::Selectable(l, v == variantPick)) variantPick = v; if (v == variantPick) ImGui::SetItemDefaultFocus(); ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                }
                Spawner::DefInfo d{};
                Spawner::DefAt(idx[gStart + variantPick], d);
                {
                    // Your own name for this type (type/family are stable between runs; saved to WheelmanMod_vehicle_labels.txt).
                    static char buf[48] = {};
                    static uint64_t bufKey = ~0ull;
                    const uint64_t key = (static_cast<uint64_t>(d.nameIdx) << 32) | d.family;
                    if (bufKey != key) { strncpy_s(buf, Spawner::LabelFor(d.nameIdx, d.family), _TRUNCATE); bufKey = key; }
                    ImGui::SetNextItemWidth(220);
                    const bool enter = ImGui::InputText("Label", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
                    if (ImGui::Button("Save label") || enter) Spawner::SetLabel(d.nameIdx, d.family, buf);
                }
                const uintptr_t vc = Spawner::VehicleClass();
                if (!vc) ImGui::TextDisabled("vehicle class unknown yet (needs one natural spawn / a tracked vehicle)");
                else if (ImGui::Button("Spawn this definition 9 m in front of me")) Spawner::RequestPair(vc, d.def);
            }
        }
        {
            // Everything ever labelled (saved in WheelmanMod_vehicle_labels.txt). A row can be spawned while its
            // definition is loaded in the current area (Scan first); the rest show "not loaded".
            const int total = Spawner::LabelCount();
            int shown = 0;
            for (int i = 0; i < total; ++i) { uint32_t t, f; const char* s; if (Spawner::LabelAt(i, t, f, s)) ++shown; }
            char head[64];
            snprintf(head, sizeof(head), "Recorded vehicles (%d)###recorded", shown);
            if (ImGui::TreeNode(head))
            {
                const uintptr_t vc = Spawner::VehicleClass();
                for (int i = 0; i < total; ++i)
                {
                    uint32_t type = 0, fam = 0; const char* text = nullptr;
                    if (!Spawner::LabelAt(i, type, fam, text)) continue;
                    int found = -1;
                    for (int k = 0; k < Spawner::DefCount(); ++k)
                    {
                        Spawner::DefInfo d{};
                        if (Spawner::DefAt(k, d) && d.populated && d.nameIdx == type && d.family == fam) { found = k; break; }
                    }
                    ImGui::PushID(i);
                    ImGui::Text("%-16s  0x%X/0x%X", text, type, fam);
                    ImGui::SameLine(300);
                    if (found >= 0 && vc)
                    {
                        Spawner::DefInfo d{}; Spawner::DefAt(found, d);
                        if (ImGui::SmallButton("Spawn##recorded")) Spawner::RequestPair(vc, d.def);
                    }
                    else
                    {
                        float lx = 0, ly = 0;
                        if (found < 0 && Spawner::LabelPos(type, fam, lx, ly)) ImGui::TextDisabled(TR("not loaded (named near %.0f, %.0f m)"), lx / 100.f, ly / 100.f);
                        else ImGui::TextDisabled(found >= 0 ? "loaded (class unknown)" : "not loaded");
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) Spawner::SetLabel(type, fam, "");
                    ImGui::PopID();
                }
                if (shown == 0) ImGui::TextDisabled("nothing recorded yet - name a model with Label / Save label");
                ImGui::TreePop();
            }
        }
        {
            static int pick = 0;
            uintptr_t models[32]; char labels[32][64]; int mc = 0;
            float ox = 0, oy = 0, oz = 0;
            bool haveOrigin = GetPlayerPosition(ox, oy, oz);
            auto addModel = [&](uintptr_t v, const char* tag)
            {
                if (mc >= 32 || !IsLiveVehicle(reinterpret_cast<void*>(v))) return;
                for (int i = 0; i < mc; ++i) if (models[i] == v) return;
                int32_t hp = 0; float vx = 0, vy = 0, vz = 0;
                SafeReadInt(reinterpret_cast<void*>(v), 0x2AC, hp);
                SafeReadFloat(reinterpret_cast<void*>(v), 0xD4, vx); SafeReadFloat(reinterpret_cast<void*>(v), 0xD8, vy); SafeReadFloat(reinterpret_cast<void*>(v), 0xDC, vz);
                float d = haveOrigin ? std::sqrt((vx - ox) * (vx - ox) + (vy - oy) * (vy - oy)) * 0.01f : 0.f;
                models[mc] = v;
                snprintf(labels[mc], sizeof(labels[0]), "%s 0x%08X  hp %d  %.0f m", tag, static_cast<uint32_t>(v), hp, d);
                ++mc;
            };
            if (g_pVehicle) addModel(reinterpret_cast<uintptr_t>(g_pVehicle), "[mine]");
            DWORD now = GetTickCount();
            for (int i = 0; i < g_vehicleListCount; ++i)
                if (now - g_vehicleList[i].lastSeenTick < 5000) addModel(reinterpret_cast<uintptr_t>(g_vehicleList[i].ptr), "[nearby]");
            if (mc == 0) ImGui::TextDisabled("no live vehicle to copy right now (use a remembered model above)");
            else
            {
                if (pick >= mc) pick = 0;
                ImGui::SetNextItemWidth(320);
                if (ImGui::BeginCombo("Model to copy", labels[pick]))
                {
                    for (int i = 0; i < mc; ++i) { ImGui::PushID(i); if (ImGui::Selectable(labels[i], i == pick)) pick = i; if (i == pick) ImGui::SetItemDefaultFocus(); ImGui::PopID(); }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Spawn a copy 9 m in front of me")) Spawner::Request(models[pick]);
            }
            static const char* names[] = { "idle", "queued", "spawned", "failed" };
            if (debugEnabled)
            {
                ImGui::TextDisabled(TR("status: %s - %s   last spawned: 0x%08X"), names[Spawner::status & 3], Spawner::lastMessage, static_cast<uint32_t>(Spawner::lastSpawned));
                if (Spawner::Factory()) ImGui::TextDisabled(TR("vehicle factory: 0x%08X (captured)"), static_cast<uint32_t>(Spawner::Factory()));
                else ImGui::TextDisabled("vehicle factory: not captured yet - it is picked up when the game spawns a car by itself (drive around)");
                ImGui::TextDisabled("Uses the game's own vehicle factory (0x498340) from the police subsystem's per-frame update.");
            }
            else if ((Spawner::status & 3) == 3 && Spawner::lastMessage[0]) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%s", Spawner::lastMessage);
        }

    }

    void DrawVehicleTab()
    {
        if (!ImGui::BeginTabBar("VehicleTabs")) return;
        if (ImGui::BeginTabItem("General")) { DrawVehicleGeneral(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Handling")) { DrawVehicleTuning(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Abilities")) { DrawVehicleAbilities(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Spawn")) { DrawVehicleSpawn(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    // ---- Debug tab: pointers, scanners and experiments --------------------
    void DrawDebugToolsTab()
    {
        ImGui::SeparatorText("Graphics");
        if (ImGui::Button("Capture the draw calls of the next frame")) RequestFrameCapture();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", FrameCaptureStatus());
        ImGui::TextDisabled("Writes states, shaders, textures and render targets of every draw call to %%TEMP%%\\WheelmanMod_draws.txt (used to fix the sky flicker and the shadows).");
        ImGui::Text("Sky flicker test:");
        ImGui::SameLine(); ImGui::RadioButton("As the game does##sky", &g_skyMode, 0);
        ImGui::SameLine(); ImGui::RadioButton("Only the first dome##sky", &g_skyMode, 1);
        ImGui::SameLine(); ImGui::RadioButton("Only the last dome##sky", &g_skyMode, 2);
        ImGui::TextDisabled("The game draws two sky domes on top of each other; if one of these settings stops the flicker, that dome is the culprit.");
        ImGui::SliderFloat("Shadow depth bias x", &g_shadowBiasScale, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Shadow slope bias x", &g_shadowSlopeScale, 0.0f, 8.0f, "%.2f");
        ImGui::TextDisabled("Scales the depth bias of the shadow map pass (1 = as the game does). Too low: striped / noisy shadows, too high: thin objects lose their shadow.");
        ImGui::Text("Vehicle pointer (auto-detected): 0x%p", g_pVehicle);
        ImGui::Text("Old player pawn pointer (Mana hook): 0x%p", g_pPlayerPawn);

        if (g_pVehicle)
        {
            ImGui::SeparatorText("Vehicle position / velocity (read-only mirrors)");
            float pos[3] = {}, vel[3] = {};
            if (SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosX, pos[0]) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosY, pos[1]) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosZ, pos[2]))
                ImGui::Text("Position: %.1f, %.1f, %.1f", pos[0], pos[1], pos[2]);
            if (SafeReadFloat(g_pVehicle, Offsets::Vehicle_VelX, vel[0]) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_VelY, vel[1]) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_VelZ, vel[2]) &&
                std::isfinite(vel[0]) && std::isfinite(vel[1]) && std::isfinite(vel[2]))
                ImGui::Text("Velocity (+0xEC/F0/F4): %.1f, %.1f, %.1f", vel[0], vel[1], vel[2]);
            ImGui::TextWrapped("Havok owns the real transform: writes to these mirrors don't stick "
                                "and moving the rigid body directly leaves the physics broken.");
            ImGui::TextDisabled(TR("Tracked nearby vehicles: %d"), g_vehicleListCount);

            ImGui::SeparatorText("Color hunt");
            ImGui::TextWrapped("Snapshot the car right before repainting, repaint, then compare - every "
                                "changed byte gets logged to WheelmanMod_log.txt.");
            if (ImGui::Button("1. Snapshot before repaint"))
                SnapshotVehicleForDiff();
            ImGui::SameLine();
            if (ImGui::Button("2. Compare after repaint"))
                CompareVehicleDiff();

            int32_t paintIdx = 0;
            if (ReadPaintIndex(g_pVehicle, paintIdx))
            {
                if (ImGui::InputInt("Paint color index (try writing live)", &paintIdx, 1, 5))
                    WritePaintIndex(g_pVehicle, paintIdx);
            }
            else
                ImGui::TextDisabled("Paint color index: n/a right now");
        }

        if (g_pPlayerPawn)
        {
            ImGui::SeparatorText("Old pawn fields (from the Mana hook - offsets unverified, likely wrong)");
            float boost = 0.f;
            if (SafeReadFloat(g_pPlayerPawn, Offsets::Pawn_Boost, boost) &&
                ImGui::InputFloat("Boost / nitro (+0x5C)", &boost, 1.f, 100.f))
                SafeWriteFloat(g_pPlayerPawn, Offsets::Pawn_Boost, boost);
            int32_t ammo = 0;
            if (SafeReadInt(g_pPlayerPawn, Offsets::Pawn_Ammo, ammo) &&
                ImGui::InputInt("Ammo (+0x2F0)", &ammo, 1, 10))
                SafeWriteInt(g_pPlayerPawn, Offsets::Pawn_Ammo, ammo);
            float health = 0.f, maxHealth = 0.f;
            if (SafeReadFloat(g_pPlayerPawn, Offsets::Pawn_Health, health) &&
                ImGui::InputFloat("Health (+0x3F4)", &health, 1.f, 10.f))
                SafeWriteFloat(g_pPlayerPawn, Offsets::Pawn_Health, health);
            if (SafeReadFloat(g_pPlayerPawn, Offsets::Pawn_MaxHealth, maxHealth))
                ImGui::Text("Max health (+0x3F8): %.1f", maxHealth);
        }

        ImGui::SeparatorText("Stat finder");
        ImGui::TextWrapped("Snapshot, take damage, narrow to decreased values, repeat until few candidates remain.");
        if (g_pPlayerPawn && ImGui::Button("1. Snapshot PlayerPawn now"))
            ScanSnapshot(g_pPlayerPawn);
        ImGui::SameLine();
        if (g_scanHasBaseline && ImGui::Button("2. Narrow: decreased (took damage)"))
            ScanNarrow(g_pPlayerPawn, true);
        ImGui::SameLine();
        if (g_scanHasBaseline && ImGui::Button("2b. Narrow: increased"))
            ScanNarrow(g_pPlayerPawn, false);
        if (g_scanHasBaseline)
        {
            ImGui::Text("Candidates: %d", g_scanCandidateCount);
            for (int i = 0; i < g_scanCandidateCount && i < 20; ++i)
                ImGui::BulletText("+0x%X", g_scanCandidateOffsets[i]);
        }
    }
    void DrawFieldsTab()
    {
        if (!g_pVehicle)
        {
            ImGui::TextDisabled("(get in your car first)");
            return;
        }
        ImGui::TextWrapped("Every dword from +0x100 to +0xA80 on YOUR vehicle right now. Tick any "
                            "you want to also see - live - on every vehicle in the ESP overlay, "
                            "so you can compare specific numbers between different real cars "
                            "(different colors of the same model, etc.) and spot what tracks with "
                            "what you're looking at.");
        if (ImGui::Button("Show none"))
            for (bool& b : g_fieldShown) b = false;
        ImGui::SameLine();
        if (ImGui::Button("Show all"))
            for (bool& b : g_fieldShown) b = true;

        ImGui::BeginChild("FieldsScroll", ImVec2(0, 400), true);
        ImGuiListClipper clipper;
        clipper.Begin(kFieldCount);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                int offset = kFieldRangeStart + i * 4;
                int32_t value = 0;
                bool ok = SafeReadInt(g_pVehicle, offset, value);
                ImGui::PushID(i);
                Binds::Check("##show", &g_fieldShown[i]);
                ImGui::SameLine();
                if (ok)
                {
                    float asFloat;
                    memcpy(&asFloat, &value, sizeof(float));
                    ImGui::Text("+0x%03X = %d  (0x%08X, %.4g)", offset, value,
                                static_cast<uint32_t>(value), asFloat);
                }
                else
                {
                    ImGui::TextDisabled("+0x%03X = n/a", offset);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void DrawPlayerFieldsTab()
    {
        if (!g_pPlayerPawn)
        {
            ImGui::TextDisabled("(triggers once the player pawn ticks)");
            return;
        }
        ImGui::TextWrapped("Every dword from +0x0 to +0x%X on the player pawn (same range the Stat "
                            "Finder scanner on the Main tab covers). Pin anything interesting so it "
                            "stays visible while you keep scrolling and testing. Do something in-game "
                            "(jump, take damage, fire, pick up ammo/boost) and watch which pinned "
                            "values move.", kPlayerFieldRangeEnd);
        if (ImGui::Button("Unpin all"))
            for (bool& b : g_playerFieldPinned) b = false;

        bool anyPinned = false;
        for (bool b : g_playerFieldPinned) if (b) { anyPinned = true; break; }
        if (anyPinned)
        {
            ImGui::Separator();
            ImGui::TextUnformatted("Pinned:");
            for (int i = 0; i < kPlayerFieldCount; ++i)
            {
                if (!g_playerFieldPinned[i]) continue;
                int offset = kPlayerFieldRangeStart + i * 4;
                int32_t value = 0;
                bool ok = SafeReadInt(g_pPlayerPawn, offset, value);
                ImGui::PushID(1000000 + i);
                Binds::Check("##pin", &g_playerFieldPinned[i]);
                ImGui::SameLine();
                if (ok)
                {
                    float asFloat;
                    memcpy(&asFloat, &value, sizeof(float));
                    ImGui::Text("+0x%03X = %d  (0x%08X, %.4g)", offset, value,
                                static_cast<uint32_t>(value), asFloat);
                }
                else
                {
                    ImGui::TextDisabled("+0x%03X = n/a", offset);
                }
                ImGui::PopID();
            }
        }

        ImGui::Separator();
        ImGui::BeginChild("PlayerFieldsScroll", ImVec2(0, 400), true);
        ImGuiListClipper clipper;
        clipper.Begin(kPlayerFieldCount);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                int offset = kPlayerFieldRangeStart + i * 4;
                int32_t value = 0;
                bool ok = SafeReadInt(g_pPlayerPawn, offset, value);
                ImGui::PushID(i);
                Binds::Check("##pin", &g_playerFieldPinned[i]);
                ImGui::SameLine();
                if (ok)
                {
                    float asFloat;
                    memcpy(&asFloat, &value, sizeof(float));
                    ImGui::Text("+0x%03X = %d  (0x%08X, %.4g)", offset, value,
                                static_cast<uint32_t>(value), asFloat);
                }
                else
                {
                    ImGui::TextDisabled("+0x%03X = n/a", offset);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    // Offsets DERIVED (not yet live-verified) from Default__ object dumps
    // in MwyVehicle.upk: every "SerializedGroup" tag is a raw memory image
    // with the real byte offset of its first field. Anchored on offsets we
    // had already confirmed live (0x54C steering = m_SimState.AnalogLStickX,
    // 0x560 = m_iNumPlayersOnBoard, 0x5AC = m_eVehicleType) and walked
    // backwards/forwards through the declaration order. Camera object layout
    // (MwyVehicleCamera / ...Default3rdPerson) comes straight from their
    // Default__ groups: +0x30 m_Name, +0x38 m_FOV, +0x3C m_bIsFirstPerson,
    // +0x40 m_Height, +0x44 m_Distance, +0x48 m_CurrentYaw.
    // ---- Player (WheelmanPlayerPawn) -------------------------------------
    // Offsets derived with Tools\decompiler\Layout.ps1 from Default__ objects
    // (Engine.Pawn -> MwyPawn -> WheelmanPawn -> WheelmanPlayerPawn); see the
    // layout_*.txt files there. Anchors: Pawn.Health 0x2AC (default 200 for
    // the player). NOTE: the old g_pPlayerPawn from the Mana hook is NOT this
    // object (its 0x3F4/0x2F0 "health/ammo" guesses fall inside Vector fields
    // of the real pawn), so the real pawn is located by a memory signature.
    uintptr_t g_realPawn = 0;
    bool g_godInvincible = false;   // keeps m_bInvincible/m_bKismetInvincible set
    bool g_godHealthLock = false;   // keeps Health >= g_lockHealth
    int  g_lockHealth = 200;

    // Experimental noclip: Actor.PhysicsComponent (pawn+0x170) is the pawn's
    // MwyCharacterPhysicsComponent; its bit field at +0xB4 has ApplyGravity
    // (bit 1) and DoPhysics (bit 2). With both off the Havok character proxy
    // stops driving the pawn, so we move Actor.Location (0xD4) ourselves.
    bool  g_noclip = false;
    float g_noclipSpeed = 1500.f;
    bool  g_noclipActive = false;
    bool  g_noclipRestoring = false;
    ULONGLONG g_noclipRestoreAt = 0;
    int32_t g_noclipSavedBits = 0;
    int g_noclipSavedState = 0;
    ULONGLONG g_noclipLastTick = 0;
    int g_noclipForceState = 2; // ECharacterPhysicsState guess: 0 Walking, 1 PhysicsInAir, 2 KeyframedInAir, 3 Knockdown; -1 = don't touch

    bool PawnSignatureOk(uintptr_t b)
    {
        void* p = reinterpret_cast<void*>(b);
        int32_t a = 0, c = 0, d = 0, e = 0, ctl = 0;
        return SafeReadInt(p, 0x15DC, a) && SafeReadInt(p, 0x15E4, c) && SafeReadInt(p, 0x63C, d) &&
               SafeReadInt(p, 0x16A8, e) && SafeReadInt(p, 0x1D0, ctl) &&
               a == 1000 && c == 10000 && d == 10000 && e == 50 && static_cast<uint32_t>(ctl) > 0x10000;
    }

    uintptr_t ScanRegionForPawn(uintptr_t start, uintptr_t end)
    {
        __try
        {
            for (uintptr_t p = start; p + 12 <= end; p += 4)
            {
                const int32_t* d = reinterpret_cast<const int32_t*>(p);
                if (d[0] != 1000 || d[2] != 10000) continue;
                uintptr_t base = p - 0x15DC;
                if (PawnSignatureOk(base)) return base;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    uintptr_t FindRealPawn()
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x1000)
            {
                uintptr_t found = ScanRegionForPawn(reinterpret_cast<uintptr_t>(mbi.BaseAddress), regionEnd);
                if (found) return found;
            }
            addr = regionEnd;
        }
        return 0;
    }

    // The player's MwyCharacterPhysicsComponent is NOT at pawn+0x170 (that
    // field is null for this pawn - found by probing the live process); it
    // lives in the Components array. Find it by vtable (0x142FA10, static in
    // the exe) + owner back-references (+0x1C and +0x40 == the pawn).
    uintptr_t g_pawnComp = 0;
    constexpr uint32_t kCharPhysVtbl = 0x142FA10;

    bool CompOk(uintptr_t comp, uintptr_t pawn)
    {
        void* p = reinterpret_cast<void*>(comp);
        int32_t v = 0, o1 = 0, o2 = 0;
        return comp && SafeReadInt(p, 0, v) && static_cast<uint32_t>(v) == kCharPhysVtbl &&
               SafeReadInt(p, 0x1C, o1) && SafeReadInt(p, 0x40, o2) &&
               static_cast<uintptr_t>(static_cast<uint32_t>(o1)) == pawn && static_cast<uintptr_t>(static_cast<uint32_t>(o2)) == pawn;
    }

    uintptr_t ScanRegionForComp(uintptr_t start, uintptr_t end, uintptr_t pawn)
    {
        __try
        {
            for (uintptr_t p = start; p + 4 <= end; p += 4)
                if (*reinterpret_cast<const uint32_t*>(p) == kCharPhysVtbl && CompOk(p, pawn)) return p;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    uintptr_t FindPawnComp(uintptr_t pawn)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x200)
            {
                uintptr_t f = ScanRegionForComp(reinterpret_cast<uintptr_t>(mbi.BaseAddress), regionEnd, pawn);
                if (f) return f;
            }
            addr = regionEnd;
        }
        return 0;
    }

    struct BitDef { int offset; const char* title; const char* const* names; int count; };
    const char* const kBits1E4[] = { "bUpAndOut", "bIsWalking", "bWantsToCrouch", "bIsCrouched", "bTryToUncrouch", "bCanCrouch",
        "bCrawler", "bReducedSpeed", "bJumpCapable", "bCanJump", "bCanWalk", "bCanSwim", "bCanFly", "bCanClimbLadders",
        "bCanStrafe", "bAvoidLedges", "bStopAtLedges", "bCountJumps", "bSimulateGravity", "bIgnoreForces",
        "bCanWalkOffLedges", "bCanBeBaseForPawns", "bClientCollision", "bSimGravityDisabled", "bDirectHitWall",
        "bForceFloorCheck", "bForceKeepAnchor", "bCanMantle", "bCanClimbCeilings", "bCanSwatTurn", "bIsFemale",
        "bCanPickupInventory" };
    const char* const kBits1E8[] = { "bAmbientCreature", "bLOSHearing", "bMuffledHearing", "bAroundCornerHearing",
        "bDontPossess", "bAutoFire", "bRollToDesired", "bStationary", "bCachedRelevant", "bSpecialHUD", "bNoWeaponFiring",
        "bCanUse", "bRunPhysicsWithNoController", "bForceMaxAccel", "m_bInvincible", "m_bKismetInvincible",
        "bForceRMVelocity", "bForceRegularVelocity", "bPlayedDeath" };
    const char* const kBits5FC[] = { "EnableShadowLimits", "ShowPlanDebug", "ShowAnimDebug", "ShowAIDebug",
        "EnableClearBloodOnPickup", "m_bCanGreet", "m_bIsSitting", "mMustHoldWeapon", "UseSpecifiedVoiceID", "bDiscarding",
        "m_bDriver", "m_bInPrecisionAim", "m_bRegenerateHealth", "m_bAutoPilotUseInfiniteMass", "m_bAutoPilotAutoReverse" };
    const char* const kBits648[] = { "bStopProximityQueries", "AllowWeaponSwap", "m_bAllowNonSystemicPOICamera",
        "m_bWeaponOut", "m_bIsTargetable", "m_bIsExitingVehicle", "m_bIsEnteringVehicle", "m_bIsVehicleBailOut",
        "m_bIsVehicleDuckDown", "m_bIsFallFromVehicleRoof", "m_bHitReaction", "m_bIsAirjacking", "m_bArrested",
        "m_bInWater", "m_bArrestable" };

    void DrawBitfield(void* pawn, int offset, const char* title, const char* const* names, int count)
    {
        int32_t bits = 0;
        if (!SafeReadInt(pawn, offset, bits)) return;
        if (!ImGui::TreeNode(title)) return;
        for (int i = 0; i < count; ++i)
        {
            bool on = (static_cast<uint32_t>(bits) >> i) & 1u;
            if (Binds::Check(names[i], &on))
            {
                bits = on ? static_cast<int32_t>(static_cast<uint32_t>(bits) | (1u << i))
                          : static_cast<int32_t>(static_cast<uint32_t>(bits) & ~(1u << i));
                SafeWriteInt(pawn, offset, bits);
            }
        }
        ImGui::TreePop();
    }

    // Finds / validates the player pawn; returns it or nullptr. Draws the "Find player pawn" helper when it is missing.
    void* PlayerPawnForTab()
    {
        const uintptr_t p = GetPlayerPawn();   // validated, and re-scanned at most every 3 s
        return p ? reinterpret_cast<void*>(p) : nullptr;
    }

    void DrawPlayerTab()
    {
        void* pawn = PlayerPawnForTab();
        if (!pawn)
        {
            ImGui::TextDisabled("Player pawn not found yet (load into the world). The switches below are armed anyway.");
            if (ImGui::Button("Find player pawn")) g_realPawn = FindRealPawn();
        }

        ImGui::SeparatorText("Health / god mode");
        Binds::Check("Invincible (m_bInvincible + m_bKismetInvincible)", &g_godInvincible);
        Binds::Check("Keep health at least:", &g_godHealthLock);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##lockhp", &g_lockHealth, 0, 0);
        if (pawn && debugEnabled)
        {
            int32_t hp = 0;
            if (SafeReadInt(pawn, 0x2AC, hp) && ImGui::InputInt("Health (+0x2AC, default 200)", &hp, 10, 100))
                SafeWriteInt(pawn, 0x2AC, hp);
            float dmg = 0.f;
            if (SafeReadFloat(pawn, 0x6A4, dmg) && ImGui::InputFloat("Damage taken (+0x6A4, 1 = normal, 0 = none)", &dmg, 0.1f, 1.f))
                SafeWriteFloat(pawn, 0x6A4, dmg);
            float dscale = 0.f;
            if (SafeReadFloat(pawn, 0x308, dscale) && ImGui::InputFloat("Damage scaling (+0x308)", &dscale, 0.1f, 1.f))
                SafeWriteFloat(pawn, 0x308, dscale);
            int32_t low = 0;
            if (SafeReadInt(pawn, 0x16A8, low) && ImGui::InputInt("Low health limit (+0x16A8)", &low))
                SafeWriteInt(pawn, 0x16A8, low);
        }

        ImGui::SeparatorText("Weapons");
        Binds::Check("No reload - ammo never drops below 16", &g_patches.noReload);
        Binds::Check("No weapon spread (accuracy is always perfect)", &WeaponMod::noSpread);
        Binds::Check("No recoil", &WeaponMod::noRecoil);
        if (debugEnabled)
        {
            float base = 0, maxOff = 0, perShot = 0, cur = 0, rec = 0;
            if (WeaponMod::ReadAim(base, maxOff, perShot, cur, rec))
                ImGui::TextDisabled("weapon @ 0x%08X  spread base/max/per-shot/current = %.1f / %.1f / %.1f / %.1f deg   recoil %.2f",
                                    static_cast<uint32_t>(WeaponMod::currentWeapon), base, maxOff, perShot, cur, rec);
            else
                ImGui::TextDisabled("(no weapon in hand)");
        }

        ImGui::SeparatorText("Give weapon (experimental)");
        {
            static int wpick = 0;
            if (ImGui::Button("Scan weapon classes")) { Weapons::Scan(); wpick = 0; }
            ImGui::SameLine();
            ImGui::TextDisabled("hold any weapon first; needs the game to have loaded the classes");
            const int wn = Weapons::Count();
            if (wn > 0)
            {
                if (wpick >= wn) wpick = 0;
                Weapons::Entry cur{};
                Weapons::At(wpick, cur);
                ImGui::SetNextItemWidth(300);
                if (ImGui::BeginCombo("Weapon", cur.label))
                {
                    for (int i = 0; i < wn; ++i)
                    {
                        Weapons::Entry e{};
                        if (Weapons::At(i, e) && (!e.unsafe || unsafeEnabled)) { ImGui::PushID(i); if (ImGui::Selectable(e.label, i == wpick)) wpick = i; if (i == wpick) ImGui::SetItemDefaultFocus(); ImGui::PopID(); }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Give this weapon")) Weapons::Give(wpick);
            }
            ImGui::TextDisabled(TR("status: %s"), Weapons::lastMessage);
        }

        ImGui::SeparatorText("Camera");
        Binds::Check("Direct mouse camera (experimental)", &MouseLook::enabled);
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Mouse sensitivity", &MouseLook::sensitivity, 0.01f, 0.30f, "%.3f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Sensitivity while aiming (right mouse button held)", &MouseLook::aimScale, 0.10f, 1.50f, "x%.2f");
        Binds::Check("Invert vertical axis", &MouseLook::invertY);
        ImGui::TextDisabled("The on-foot camera turns straight from the mouse instead of the game's emulated controller stick: no dead zone, acceleration or smoothing. In a vehicle and in cut-scenes the mouse works as in the game.");
        if (debugEnabled) ImGui::TextDisabled("%s", MouseLook::status);

        ImGui::SeparatorText("Noclip / fly");
        Binds::Check("Noclip: W/A/S/D move along the camera, Space up, Ctrl down, Shift x3", &g_noclip);
        ImGui::SliderFloat("Noclip speed", &g_noclipSpeed, 200.f, 20000.f, "%.0f");

        if (!pawn) return;

        ImGui::SeparatorText("Health regeneration");
        static const struct { int off; const char* n; } regen[] = {
            { 0x6E0, "InitialHealthRegenStartTime" }, { 0x6E4, "InitialHealthRegenRate" },
            { 0x6E8, "SecondaryHealthRegenStartTime" }, { 0x6EC, "SecondaryHealthRegenRate" } };
        for (const auto& r : regen)
        {
            float v = 0.f;
            if (SafeReadFloat(pawn, r.off, v) && ImGui::InputFloat(r.n, &v, 0.5f, 5.f))
                SafeWriteFloat(pawn, r.off, v);
        }

        if (!debugEnabled) return;   // the raw movement fields below are for developers
        ImGui::SeparatorText("Movement");
        static const struct { int off; const char* n; float step; } mv[] = {
            { 0x258, "GroundSpeed (+0x258)", 50.f }, { 0x25C, "WaterSpeed (+0x25C)", 50.f },
            { 0x260, "AirSpeed (+0x260)", 50.f }, { 0x264, "LadderSpeed (+0x264)", 10.f },
            { 0x268, "AccelRate (+0x268)", 50.f }, { 0x26C, "JumpZ (+0x26C)", 50.f },
            { 0x278, "AirControl (+0x278)", 0.05f }, { 0x27C, "WalkingPct (+0x27C)", 0.05f },
            { 0x280, "CrouchedPct (+0x280)", 0.05f }, { 0x284, "MaxFallSpeed (+0x284)", 100.f },
            { 0x790, "m_fRotationRate (+0x790)", 10.f } };
        for (const auto& m : mv)
        {
            float v = 0.f;
            if (SafeReadFloat(pawn, m.off, v) && ImGui::InputFloat(m.n, &v, m.step, m.step * 10.f))
                SafeWriteFloat(pawn, m.off, v);
        }
        float px = 0.f, py = 0.f, pz = 0.f;
        if (SafeReadFloat(pawn, 0xD4, px) && SafeReadFloat(pawn, 0xD8, py) && SafeReadFloat(pawn, 0xDC, pz))
            ImGui::Text("Location: %.0f, %.0f, %.0f", px, py, pz);
    }

    // Debug -> Player flags: the raw pawn fields that used to sit at the bottom of the Player tab.
    void DrawPlayerFlagsDebug()
    {
        void* pawn = PlayerPawnForTab();
        if (!pawn) { ImGui::TextDisabled("real WheelmanPlayerPawn not found"); return; }
        ImGui::Text("WheelmanPlayerPawn @ 0x%08X", static_cast<uint32_t>(g_realPawn));
        ImGui::TextDisabled("m_bRegenerateHealth is bit 12 of the +0x5FC flags below.");

        ImGui::SeparatorText("Misc");
        int32_t vq = 0;
        if (SafeReadInt(pawn, 0x63C, vq) && ImGui::InputInt("VehicleQueryDistance (+0x63C)", &vq, 500, 5000))
            SafeWriteInt(pawn, 0x63C, vq);
        float wsd = 0.f;
        if (SafeReadFloat(pawn, 0x650, wsd) && ImGui::InputFloat("WeaponSwapDelay (+0x650)", &wsd, 0.1f, 1.f))
            SafeWriteFloat(pawn, 0x650, wsd);
        float px = 0.f, py = 0.f, pz = 0.f;
        if (SafeReadFloat(pawn, 0xD4, px) && SafeReadFloat(pawn, 0xD8, py) && SafeReadFloat(pawn, 0xDC, pz))
            ImGui::Text("Location (+0xD4): %.0f, %.0f, %.0f", px, py, pz);

        ImGui::SeparatorText("Flags (checkbox = live bit)");
        DrawBitfield(pawn, 0x1E4, "Pawn flags +0x1E4", kBits1E4, 32);
        DrawBitfield(pawn, 0x1E8, "Pawn flags +0x1E8 (incl. m_bInvincible)", kBits1E8, 19);
        DrawBitfield(pawn, 0x5FC, "WheelmanPawn flags +0x5FC", kBits5FC, 15);
        DrawBitfield(pawn, 0x648, "WheelmanPawn flags +0x648", kBits648, 15);
    }

    // Debug: internals of the pawn's character physics component (used by noclip).
    void DrawPlayerPhysicsDebug()
    {
        if (!g_realPawn || !PawnSignatureOk(g_realPawn))
        {
            ImGui::TextDisabled("real player pawn not found (open the Player tab first)");
            return;
        }
        if (ImGui::Button("Find physics component") || (g_pawnComp == 0 && ImGui::IsWindowAppearing()))
            g_pawnComp = FindPawnComp(g_realPawn);
        if (!CompOk(g_pawnComp, g_realPawn)) g_pawnComp = 0;
        int32_t compPtr = static_cast<int32_t>(g_pawnComp), cbits = 0;
        if (!compPtr) ImGui::TextDisabled("physics component not found");
        if (compPtr && SafeReadInt(reinterpret_cast<void*>(compPtr), 0xB4, cbits))
            ImGui::Text("PhysicsComponent @ 0x%08X  flags(+0xB4)=0x%X  ApplyGravity=%d DoPhysics=%d",
                        static_cast<uint32_t>(compPtr), static_cast<uint32_t>(cbits), (cbits >> 1) & 1, (cbits >> 2) & 1);
        uint8_t cst = 0, shp = 0;
        if (compPtr && SafeReadByte(reinterpret_cast<void*>(compPtr), 0xB8, cst) && SafeReadByte(reinterpret_cast<void*>(compPtr), 0xB9, shp))
        {
            ImGui::Text("CurrentState (+0xB8) = %d   m_eActiveShape (+0xB9) = %d  [names in file: Walking, PhysicsInAir, KeyframedInAir, Knockdown]", cst, shp);
            int st = cst;
            if (ImGui::InputInt("set CurrentState now", &st) && st >= 0 && st < 8)
                SafeWriteByte(reinterpret_cast<void*>(compPtr), 0xB8, static_cast<uint8_t>(st));
        }
        ImGui::InputInt("noclip forces CurrentState (-1 = leave alone)", &g_noclipForceState);
        float gm = 0.f;
        if (compPtr && SafeReadFloat(reinterpret_cast<void*>(compPtr), 0x19C, gm) &&
            ImGui::InputFloat("GravityMultiplier (comp +0x19C)", &gm, 0.1f, 1.f))
            SafeWriteFloat(reinterpret_cast<void*>(compPtr), 0x19C, gm);
    }

    // Called every frame (also with the menu hidden) - enforces god mode.
    void NoclipTick(void* pawn)
    {
        if (!CompOk(g_pawnComp, g_realPawn))
            g_pawnComp = 0;
        if (g_noclip && !g_pawnComp)
            g_pawnComp = FindPawnComp(g_realPawn); // one scan when noclip is switched on / lost
        void* comp = reinterpret_cast<void*>(g_pawnComp);
        if (!g_noclip)
        {
            if (g_noclipActive && comp)
            {
                int32_t bits = 0;
                if (SafeReadInt(comp, 0xB4, bits))
                    SafeWriteInt(comp, 0xB4, static_cast<int32_t>((static_cast<uint32_t>(bits) & ~0x6u) | (static_cast<uint32_t>(g_noclipSavedBits) & 0x6u) | 0x10u)); // 0x10 = FirstSimulationStep: re-init the proxy at the new location
                // Writing the saved state straight back leaves the Havok character proxy
                // dead (no walls, no gravity). The state has to actually change for the
                // game to rebuild it, so go through PhysicsInAir (1) first (verified live).
                SafeWriteByte(comp, 0xB8, 1);
                g_noclipRestoreAt = GetTickCount64() + 400;
                g_noclipRestoring = true;
            }
            g_noclipActive = false;
            if (g_noclipRestoring && comp && GetTickCount64() >= g_noclipRestoreAt)
            {
                SafeWriteByte(comp, 0xB8, static_cast<uint8_t>(g_noclipSavedState));
                g_noclipRestoring = false;
            }
            return;
        }
        g_noclipRestoring = false;
        if (!comp) return;
        int32_t bits = 0;
        if (!SafeReadInt(comp, 0xB4, bits)) return;
        if (!g_noclipActive)
        {
            g_noclipSavedBits = bits; g_noclipActive = true; g_noclipLastTick = GetTickCount64();
            uint8_t st = 0; SafeReadByte(comp, 0xB8, st); g_noclipSavedState = st;
        }
        if (bits & 0x6) SafeWriteInt(comp, 0xB4, static_cast<int32_t>(static_cast<uint32_t>(bits) & ~0x6u));
        if (g_noclipForceState >= 0) SafeWriteByte(comp, 0xB8, static_cast<uint8_t>(g_noclipForceState));

        ULONGLONG now = GetTickCount64();
        float dt = static_cast<float>(now - g_noclipLastTick) / 1000.f;
        g_noclipLastTick = now;
        if (dt > 0.1f) dt = 0.1f;

        float m[16];
        if (!GetCandidateViewProjMatrix(m)) return;
        float fx = m[3], fy = m[7], fz = m[11];
        float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-4f) return;
        fx /= fl; fy /= fl; fz /= fl;
        float rx = -fy, ry = fx;
        float rl = std::sqrt(rx * rx + ry * ry);
        if (rl > 1e-4f) { rx /= rl; ry /= rl; }

        float mx = 0.f, my = 0.f, mz = 0.f;
        if (!ImGui::GetIO().WantCaptureKeyboard)
        {
            if (GetAsyncKeyState('W') & 0x8000) { mx += fx; my += fy; mz += fz; }
            if (GetAsyncKeyState('S') & 0x8000) { mx -= fx; my -= fy; mz -= fz; }
            if (GetAsyncKeyState('D') & 0x8000) { mx += rx; my += ry; }
            if (GetAsyncKeyState('A') & 0x8000) { mx -= rx; my -= ry; }
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) mz += 1.f;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mz -= 1.f;
        }
        float len = std::sqrt(mx * mx + my * my + mz * mz);
        float x = 0.f, y = 0.f, z = 0.f;
        if (!SafeReadFloat(pawn, 0xD4, x) || !SafeReadFloat(pawn, 0xD8, y) || !SafeReadFloat(pawn, 0xDC, z)) return;
        if (len > 1e-4f)
        {
            float speed = g_noclipSpeed * ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 3.f : 1.f) * dt / len;
            x += mx * speed; y += my * speed; z += mz * speed;
            SafeWriteFloat(pawn, 0xD4, x); SafeWriteFloat(pawn, 0xD8, y); SafeWriteFloat(pawn, 0xDC, z);
        }
        SafeWriteFloat(pawn, 0xEC, 0.f); SafeWriteFloat(pawn, 0xF0, 0.f); SafeWriteFloat(pawn, 0xF4, 0.f);
    }

    void PlayerTick()
    {
        if (!g_realPawn) return;
        if (!PawnSignatureOk(g_realPawn)) { g_realPawn = 0; g_noclipActive = false; return; }
        void* pawn = reinterpret_cast<void*>(g_realPawn);
        NoclipTick(pawn);
        if (!g_godInvincible && !g_godHealthLock) return;
        if (g_godInvincible)
        {
            int32_t f = 0;
            if (SafeReadInt(pawn, 0x1E8, f) && ((static_cast<uint32_t>(f) & 0xC000u) != 0xC000u))
                SafeWriteInt(pawn, 0x1E8, static_cast<int32_t>(static_cast<uint32_t>(f) | 0xC000u));
        }
        if (g_godHealthLock)
        {
            int32_t hp = 0;
            if (SafeReadInt(pawn, 0x2AC, hp) && hp < g_lockHealth)
                SafeWriteInt(pawn, 0x2AC, g_lockHealth);
        }
    }

    // WheelmanCamera (WheelmanCameras.upk) is the game's real camera manager
    // and already contains a built-in developer fly camera. Layout derived
    // from Default__WheelmanCamera groups (anchors 0x488, 0x4A8, 0x518):
    //   +0x47C m_pCurrentMode  +0x480 m_pPreviousMode  +0x484 m_pRayCast
    //   +0x488 m_iFlyCameraControllerNumber (default -1)
    //   +0x48C bool bitfield: bit0 FlyCameraStart, 1 Freeze, 2 HudStatus,
    //          3 AttachedToActor, 4 PPEToggle, 5 InvertY, 6 OnFootLookAtSpring
    //          (default on), 7 VehiclePOI (default on), 8 UIDisplayMessageActive
    //   +0x490 m_vFlyCameraOffset (3 floats)  +0x49C m_vFlyCameraRotation (3 int32 rotator)
    //   +0x4A8 m_fRotationSensitivityX/Y, +0x4B0 OrbitCamMouseSensitivityX/Y
    //   +0x4B8 m_XInput struct (24 floats, ends 0x518): fYawAxis 0x4B8, fPitchAxis 0x4BC,
    //          fFlyCameraPitch 0x4F0, fFlyCameraYaw 0x4F4, fFlyCameraMove 0x4F8, fFlyCameraStrafe 0x4FC
    uintptr_t g_wheelmanCam = 0;

    uintptr_t ScanRegionForCamera(uintptr_t start, uintptr_t end)
    {
        const uint32_t sig0 = 0x42480000, sig2 = 0x40A00000; // 50.0f, 5.0f at +0x4A8..+0x4B4
        __try
        {
            for (uintptr_t p = start; p + 16 <= end; p += 4)
            {
                const uint32_t* d = reinterpret_cast<const uint32_t*>(p);
                if (d[0] != sig0 || d[1] != sig0 || d[2] != sig2 || d[3] != sig2) continue;
                uintptr_t base = p - 0x4A8;
                const int32_t* ctrlNum = reinterpret_cast<const int32_t*>(base + 0x488);
                const uint32_t* curMode = reinterpret_cast<const uint32_t*>(base + 0x47C);
                if (*ctrlNum == -1 && *curMode > 0x10000) return base; // live instance has a current mode
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    uintptr_t FindWheelmanCamera()
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x600)
            {
                uintptr_t found = ScanRegionForCamera(reinterpret_cast<uintptr_t>(mbi.BaseAddress), regionEnd);
                if (found) return found;
            }
            addr = regionEnd;
        }
        return 0;
    }

    void DrawFlyCameraSection()
    {
        ImGui::TextUnformatted("WheelmanCamera - the game's own camera manager (has a built-in fly camera)");
        if (ImGui::Button("Find WheelmanCamera instance") || (g_wheelmanCam == 0 && ImGui::IsWindowAppearing()))
            g_wheelmanCam = FindWheelmanCamera();
        if (!g_wheelmanCam)
        {
            ImGui::TextDisabled("not found yet (press the button while playing)");
            return;
        }
        void* cam = reinterpret_cast<void*>(g_wheelmanCam);
        ImGui::Text("instance @ 0x%08X", static_cast<uint32_t>(g_wheelmanCam));

        {
            int32_t ctrl = 0, curMode = 0, prevMode = 0;
            if (SafeReadInt(cam, 0x488, ctrl) && ImGui::InputInt("m_iFlyCameraControllerNumber (+0x488, default -1)", &ctrl))
                SafeWriteInt(cam, 0x488, ctrl);
            SafeReadInt(cam, 0x47C, curMode);
            SafeReadInt(cam, 0x480, prevMode);
            ImGui::Text("m_pCurrentMode=0x%08X  m_pPreviousMode=0x%08X", static_cast<uint32_t>(curMode), static_cast<uint32_t>(prevMode));
        }
        int32_t flags = 0;
        if (SafeReadInt(cam, 0x48C, flags))
        {
            static const char* names[] = { "m_bFlyCameraStart", "m_bFlyCameraFreeze", "m_bFlyCameraHudStatus",
                                           "m_bFlyCameraAttachedToActor", "m_bFlyCameraPPEToggle", "m_bInvertY",
                                           "m_bOnFootLookAtSpring", "m_bVehiclePOI", "m_bUIDisplayMessageActive" };
            ImGui::Text("bool bitfield (+0x48C) = 0x%X", static_cast<uint32_t>(flags));
            for (int b = 0; b < 9; ++b)
            {
                bool on = (flags >> b) & 1;
                if (Binds::Check(names[b], &on))
                {
                    flags = on ? (flags | (1 << b)) : (flags & ~(1 << b));
                    SafeWriteInt(cam, 0x48C, flags);
                }
            }
        }
        float off[3] = {};
        bool haveOff = SafeReadFloat(cam, 0x490, off[0]) && SafeReadFloat(cam, 0x494, off[1]) && SafeReadFloat(cam, 0x498, off[2]);
        if (haveOff && ImGui::InputFloat3("m_vFlyCameraOffset (+0x490)", off))
            for (int k = 0; k < 3; ++k) SafeWriteFloat(cam, 0x490 + k * 4, off[k]);
        int32_t rot[3] = {};
        bool haveRot = SafeReadInt(cam, 0x49C, rot[0]) && SafeReadInt(cam, 0x4A0, rot[1]) && SafeReadInt(cam, 0x4A4, rot[2]);
        if (haveRot && ImGui::InputInt3("m_vFlyCameraRotation (+0x49C, 65536=360deg)", rot))
            for (int k = 0; k < 3; ++k) SafeWriteInt(cam, 0x49C + k * 4, rot[k]);

        ImGui::TextUnformatted("m_XInput (+0x4B8) - live inputs the game feeds the camera:");
        static const struct { int off; const char* n; } xin[] = {
            { 0x4B8, "fYawAxis" }, { 0x4BC, "fPitchAxis" }, { 0x4C0, "fVehicleLookBack" },
            { 0x4F0, "m_fFlyCameraPitch" }, { 0x4F4, "m_fFlyCameraYaw" },
            { 0x4F8, "m_fFlyCameraMove" }, { 0x4FC, "m_fFlyCameraStrafe" } };
        for (const auto& x : xin)
        {
            float v = 0.f;
            if (SafeReadFloat(cam, x.off, v) && ImGui::InputFloat(x.n, &v, 0.1f, 1.f))
                SafeWriteFloat(cam, x.off, v);
        }

        if (ImGui::CollapsingHeader("Raw camera memory (+0x000..+0x540, editable as int32)"))
        {
            ImGui::BeginChild("CamRaw", ImVec2(0, 300), true);
            ImGuiListClipper clipper;
            clipper.Begin(0x540 / 4);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    int32_t v = 0;
                    ImGui::PushID(i);
                    if (SafeReadInt(cam, i * 4, v))
                    {
                        float f;
                        memcpy(&f, &v, 4);
                        ImGui::Text("+0x%03X  0x%08X  %.4g", i * 4, static_cast<uint32_t>(v), f);
                        ImGui::SameLine(300);
                        ImGui::SetNextItemWidth(110);
                        int32_t edit = v;
                        if (ImGui::InputInt("##w", &edit, 0, 0))
                            SafeWriteInt(cam, i * 4, edit);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }
        ImGui::Separator();
    }

    void DrawCameraTab()
    {
        DrawFlyCameraSection();
        if (!g_pVehicle)
        {
            ImGui::TextDisabled("(get in your car for the per-vehicle camera objects below)");
            return;
        }
        struct ArrDef { int off; const char* name; };
        const ArrDef defs[] = { { 0x4D8, "m_Simulations" }, { 0x4E4, "m_SeatInstances" },
                                { 0x4F0, "m_DoorInstances" }, { 0x4FC, "m_EffectInstances" },
                                { 0x508, "m_Cameras" } };
        ImGui::TextUnformatted("TArray headers (derived offsets - data ptr / num / max):");
        for (const ArrDef& d : defs)
        {
            int32_t data = 0, num = 0, mx = 0;
            SafeReadInt(g_pVehicle, d.off, data);
            SafeReadInt(g_pVehicle, d.off + 4, num);
            SafeReadInt(g_pVehicle, d.off + 8, mx);
            ImGui::Text("+0x%X %-18s data=0x%08X num=%d max=%d", d.off, d.name, static_cast<uint32_t>(data), num, mx);
        }

        int32_t camData = 0, camNum = 0;
        SafeReadInt(g_pVehicle, 0x508, camData);
        SafeReadInt(g_pVehicle, 0x50C, camNum);
        ImGui::Separator();
        if (camData == 0 || camNum <= 0 || camNum > 16)
        {
            ImGui::TextDisabled("m_Cameras doesn't look like a valid array - derived offset may be off.");
            return;
        }
        for (int i = 0; i < camNum; ++i)
        {
            int32_t camPtr = 0;
            if (!SafeReadInt(reinterpret_cast<void*>(camData), i * 4, camPtr) || camPtr == 0)
                continue;
            void* cam = reinterpret_cast<void*>(camPtr);
            ImGui::PushID(i);
            ImGui::Text("Camera[%d] @ 0x%08X", i, static_cast<uint32_t>(camPtr));
            float fov = 0.f, height = 0.f, dist = 0.f, yaw = 0.f;
            int32_t firstPerson = 0;
            if (SafeReadFloat(cam, 0x38, fov) && ImGui::InputFloat("m_FOV (+0x38)", &fov, 1.f, 10.f))
                SafeWriteFloat(cam, 0x38, fov);
            if (SafeReadInt(cam, 0x3C, firstPerson))
                ImGui::Text("m_bIsFirstPerson bits (+0x3C): 0x%X", static_cast<uint32_t>(firstPerson));
            if (SafeReadFloat(cam, 0x40, height) && ImGui::InputFloat("m_Height (+0x40)", &height, 10.f, 100.f))
                SafeWriteFloat(cam, 0x40, height);
            if (SafeReadFloat(cam, 0x44, dist) && ImGui::InputFloat("m_Distance (+0x44)", &dist, 10.f, 100.f))
                SafeWriteFloat(cam, 0x44, dist);
            if (SafeReadFloat(cam, 0x48, yaw))
                ImGui::Text("m_CurrentYaw (+0x48): %.3f", yaw);
            ImGui::Separator();
            ImGui::PopID();
        }
    }

    // ---- ESP -----------------------------------------------------------
    bool espEnabled = false;
    bool espShowSelf = false;    // draw a marker on your own car too, for calibration
    bool espShowHealth = true;
    bool espShowSpeed = true;
    bool espShowDistance = true;
    bool espShowCategory = true;
    bool espShowBodyStyle = true;
    bool espShowExperimental = false;
    bool espShowTags = false;
    bool espShowPaintIndex = false;
    float espMaxDistanceMeters = 150.f;

    // Per-category visibility filters (Offsets::Vehicle_Category: 0=Car,
    // 1=Moto, 2=Truck) so e.g. only motorcycles can be shown at a time.
    bool espFilterCars = true;
    bool espFilterMotos = true;
    bool espFilterTrucks = true;

    // Still-unexplained fields that vary between vehicles but aren't
    // accounted for by any confirmed field. Labeled generically so the user
    // can correlate them visually in-game and report back ("TagN was X on
    // this car") - the exact offset is kept right here so whatever comes
    // back can be pinned to a real address immediately. Tag2 (+0x320) and
    // the old Tag4 (+0x54C, turned out to be steering angle - see
    // Offsets::Vehicle_SteeringAngle) have been resolved/dropped already.
    struct DiagTag { const char* name; int offset; bool isByte; };
    constexpr DiagTag kDiagTags[] = {
        // dword; varies even between different instances of the SAME model -
        // best lead so far for paint color, needs a value<->real-color match.
        { "Tag1", Offsets::Vehicle_ColorCandidate, false },
    };

    const char* BodyStyleHint(int32_t v)
    {
        switch (v)
        {
        case 56: return "Moto";
        case 67: return "Truck";
        case 70: return "Cabrio/Van";
        case 71: return "Car/SUV";
        case 72: return "Quad";
        default: return nullptr;
        }
    }

    // BodyStyle IDs get reused across unrelated shapes (e.g. 70 alone means
    // "cabrio, regular van, OR heavy van"), but combining it with Category
    // (which at least separates car/moto/truck) resolves some of that -
    // confirmed by live testing: Category=Truck narrows BodyStyle 70 down to
    // specifically the heavy van, and BodyStyle 67 down to specifically the
    // KamAZ-style truck. Returns null (caller falls back to BodyStyleHint
    // alone) when the combination doesn't add anything beyond that.
    const char* CombinedVehicleLabel(uint8_t category, int32_t bodyStyle)
    {
        if (category == 2 && bodyStyle == 70) return "Heavy Van";
        if (category == 2 && bodyStyle == 67) return "Truck";
        return nullptr;
    }

    void DrawESPTab()
    {
        Binds::Check("Enable ESP", &espEnabled);
        if (debugEnabled)
            ImGui::TextWrapped("Projects world positions to screen using the camera's "
                                "View*Projection matrix (vertex shader constant c240, found by "
                                "watching which shader register updates rarely per frame yet "
                                "rotates with the camera - see D3DHook.h). Range is limited to "
                                "whatever the game's own nearby-vehicle ram-detection already "
                                "tracks (Debug tab 'Tracked nearby vehicles' count).");
        Binds::Check("Show health", &espShowHealth);
        ImGui::SameLine();
        Binds::Check("Show speed", &espShowSpeed);
        ImGui::SameLine();
        Binds::Check("Show distance", &espShowDistance);
        Binds::Check("Show vehicle class (Car/Moto/Truck)", &espShowCategory);
        Binds::Check("Show body style (finer subclass)", &espShowBodyStyle);
        ImGui::TextUnformatted("Only show:");
        ImGui::SameLine();
        Binds::Check("Cars", &espFilterCars);
        ImGui::SameLine();
        Binds::Check("Motos", &espFilterMotos);
        ImGui::SameLine();
        Binds::Check("Trucks", &espFilterTrucks);
        Binds::Check("Also mark my own vehicle (calibration)", &espShowSelf);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Max distance (m)", &espMaxDistanceMeters, 10.f, 500.f, "%.0f");
    }

    void DrawESPDebugTab()
    {
        Binds::Check("Show experimental fields (pointer/State/Steer)", &espShowExperimental);
        ImGui::TextWrapped("Pointer: the vehicle's own address, shown so a specific car can be "
                            "called out precisely (e.g. in chat) instead of describing it. Steer: "
                            "confirmed steering wheel angle (0=full left, 255=full right) - just "
                            "live input state, not an identity trait. State: a byte that must be "
                            "0 for the game's own AI to treat the vehicle as a valid target - "
                            "guessing wrecked/hidden/despawning, but every vehicle sampled so far "
                            "reads 0, so not useful yet.");
        ImGui::TextWrapped("Make/model and color are not included: unlike these, they'd most "
                            "likely live in a shared per-vehicle-model data table (not on the "
                            "instance itself), which is a separate, bigger investigation to "
                            "locate - let me know if you want to go after that next.");

        ImGui::Separator();
        Binds::Check("Show diagnostic tags (Tag1)", &espShowTags);
        ImGui::TextWrapped("Tag1: varies even between two of the same model - an earlier lead for "
                            "paint color that turned out to not move when repainting your own car.");
        Binds::Check("Show paint color index (confirmed)", &espShowPaintIndex);
        ImGui::TextWrapped("(+0x170)+0x24 - CONFIRMED via live testing across multiple repaints "
                            "(clean small-integer jumps that only ever happen exactly when "
                            "repainting, e.g. 9->30->31). +0x170 itself is just a pointer that "
                            "gets reallocated each repaint, so only this nested field matters. "
                            "Shown on every vehicle so different real colors can be compared - "
                            "if you can map specific index numbers to specific colors, tell me.");
    }

    void DrawEspEntry(void* vehicle, bool isSelf)
    {
        float px = 0.f, py = 0.f, pz = 0.f;
        if (!SafeReadFloat(vehicle, Offsets::Vehicle_PosX, px) ||
            !SafeReadFloat(vehicle, Offsets::Vehicle_PosY, py) ||
            !SafeReadFloat(vehicle, Offsets::Vehicle_PosZ, pz))
            return;

        if (!isSelf)
        {
            uint8_t filterCategory = 0;
            if (SafeReadByte(vehicle, Offsets::Vehicle_Category, filterCategory))
            {
                bool allowed = (filterCategory == 0 && espFilterCars) ||
                                (filterCategory == 1 && espFilterMotos) ||
                                (filterCategory == 2 && espFilterTrucks);
                if (!allowed) return;
            }
        }

        float distMeters = -1.f;
        if (g_pVehicle && !isSelf)
        {
            float mx = 0.f, my = 0.f, mz = 0.f;
            if (SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosX, mx) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosY, my) &&
                SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosZ, mz))
            {
                float dx = px - mx, dy = py - my, dz = pz - mz;
                distMeters = sqrtf(dx * dx + dy * dy + dz * dz) * 0.01f; // UU (=cm) -> m
            }
            if (distMeters >= 0.f && distMeters > espMaxDistanceMeters) return;
        }

        float sx = 0.f, sy = 0.f;
        if (!WorldToScreen(px, py, pz + 100.f, sx, sy)) return; // +100uu: roughly roof height, not ground

        int32_t health = 0;
        bool haveHealth = SafeReadInt(vehicle, Offsets::Vehicle_Health, health);
        float vx = 0.f, vy = 0.f, vz = 0.f;
        bool haveSpeed = SafeReadFloat(vehicle, Offsets::Vehicle_VelX, vx) &&
                          SafeReadFloat(vehicle, Offsets::Vehicle_VelY, vy) &&
                          SafeReadFloat(vehicle, Offsets::Vehicle_VelZ, vz) &&
                          std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz);
        float speedKmh = haveSpeed ? sqrtf(vx * vx + vy * vy + vz * vz) * 0.036f : 0.f;

        ImDrawList* draw = ImGui::GetForegroundDrawList();
        ImU32 color = isSelf ? IM_COL32(80, 180, 255, 255) : IM_COL32(255, 60, 60, 255);
        draw->AddCircleFilled(ImVec2(sx, sy), 4.f, color);

        constexpr int kMaxEspLines = 64;
        char lines[kMaxEspLines][64];
        int lineCount = 0;
        if (isSelf) { snprintf(lines[lineCount++], sizeof(lines[0]), "(you)"); }
        if (espShowHealth && haveHealth) snprintf(lines[lineCount++], sizeof(lines[0]), "HP %d", health);
        if (espShowSpeed && haveSpeed) snprintf(lines[lineCount++], sizeof(lines[0]), "%.0f km/h", speedKmh);
        if (espShowDistance && distMeters >= 0.f) snprintf(lines[lineCount++], sizeof(lines[0]), "%.0fm", distMeters);
        uint8_t category = 0;
        bool haveCategory = SafeReadByte(vehicle, Offsets::Vehicle_Category, category);
        if (espShowCategory && haveCategory)
            snprintf(lines[lineCount++], sizeof(lines[0]), "%s", VehicleCategoryLabel(category));
        if (espShowBodyStyle)
        {
            int32_t bodyStyle = 0;
            if (SafeReadInt(vehicle, Offsets::Vehicle_BodyStyle, bodyStyle))
            {
                // Category+BodyStyle combined resolves some of BodyStyle's
                // id-reuse ambiguity (see CombinedVehicleLabel) - prefer
                // that when it has a sharper answer than BodyStyle alone.
                const char* combined = haveCategory ? CombinedVehicleLabel(category, bodyStyle) : nullptr;
                const char* hint = combined ? combined : BodyStyleHint(bodyStyle);
                if (hint) snprintf(lines[lineCount++], sizeof(lines[0]), "%s", hint);
                else snprintf(lines[lineCount++], sizeof(lines[0]), "Body %d", bodyStyle);
            }
        }
        if (espShowExperimental)
        {
            snprintf(lines[lineCount++], sizeof(lines[0]), "0x%p", vehicle);
            uint8_t steer = 0;
            if (SafeReadByte(vehicle, Offsets::Vehicle_SteeringAngle, steer))
                snprintf(lines[lineCount++], sizeof(lines[0]), "Steer %u/255", steer);
            uint8_t state = 0;
            if (SafeReadByte(vehicle, Offsets::Vehicle_StateFlag, state))
                snprintf(lines[lineCount++], sizeof(lines[0]), "State %u", state);
        }
        if (espShowTags)
        {
            for (const DiagTag& tag : kDiagTags)
            {
                if (tag.isByte)
                {
                    uint8_t v = 0;
                    if (SafeReadByte(vehicle, tag.offset, v))
                        snprintf(lines[lineCount++], sizeof(lines[0]), "%s %u", tag.name, v);
                }
                else
                {
                    int32_t v = 0;
                    if (SafeReadInt(vehicle, tag.offset, v))
                        snprintf(lines[lineCount++], sizeof(lines[0]), "%s %d", tag.name, v);
                }
            }
        }
        for (int i = 0; i < kFieldCount && lineCount < kMaxEspLines; ++i)
        {
            if (!g_fieldShown[i]) continue;
            int offset = kFieldRangeStart + i * 4;
            int32_t v = 0;
            if (SafeReadInt(vehicle, offset, v))
                snprintf(lines[lineCount++], sizeof(lines[0]), "+0x%X=%d", offset, v);
        }
        if (espShowPaintIndex && lineCount < kMaxEspLines)
        {
            int32_t paintIdx = 0;
            if (ReadPaintIndex(vehicle, paintIdx))
                snprintf(lines[lineCount++], sizeof(lines[0]), "Paint %d", paintIdx);
        }

        float ty = sy + 6.f;
        for (int i = 0; i < lineCount; ++i)
        {
            ImVec2 textSize = ImGui::CalcTextSize(lines[i]);
            ImVec2 pos(sx - textSize.x * 0.5f, ty);
            draw->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), lines[i]);
            draw->AddText(pos, IM_COL32(255, 255, 255, 255), lines[i]);
            ty += textSize.y;
        }
    }

    // ---- NPC ESP ------------------------------------------------------------
    bool npcEspEnabled = false;
    bool npcOnlyEnemies = false;
    bool npcShowEnemies = true;
    bool npcShowAllies = true;
    bool npcShowVip = true;
    bool npcShowPolice = true;
    bool npcShowCivilians = true;
    bool npcShowDead = false;
    bool npcShowHealth = true;
    bool npcShowDistance = true;
    bool npcShowBox = true;
    bool npcBoneBox = true;       // box fitted around the projected bones (falls back to the fixed box)
    bool npcShowSkeleton = true;
    bool npcShowHead = true;
    bool npcShowType = false;
    ULONGLONG g_npcTabTick = 0;
    float npcMaxDistanceMeters = 200.f;

    void DrawNpcESP()
    {
        Npc::scanning = npcEspEnabled || Aimbot::enabled || GetTickCount64() - g_npcTabTick < 2000 ||
                        NpcMod::keepDisarmed || NpcMod::enemiesIgnore || NpcMod::alliesInvincible || NpcMod::alliesKeepHealth ||
                        NpcMod::policeDisarmed || NpcMod::policeIgnore || NpcMod::blindAll || MiniMap::NeedsNpcScan();
        Npc::Tick();
        if (!npcEspEnabled) return;

        float ox = 0.f, oy = 0.f, oz = 0.f;
        bool haveOrigin = false;
        if (g_realPawn && SafeReadFloat(reinterpret_cast<void*>(g_realPawn), 0xD4, ox) &&
            SafeReadFloat(reinterpret_cast<void*>(g_realPawn), 0xD8, oy) && SafeReadFloat(reinterpret_cast<void*>(g_realPawn), 0xDC, oz))
            haveOrigin = true;
        else if (g_pVehicle && SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosX, ox) &&
                 SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosY, oy) && SafeReadFloat(g_pVehicle, Offsets::Vehicle_PosZ, oz))
            haveOrigin = true;

        static Npc::Info list[256];
        int n = Npc::Snapshot(list, 256, npcShowSkeleton || npcBoneBox || npcShowHead);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        for (int i = 0; i < n; ++i)
        {
            const Npc::Info& p = list[i];
            if (npcOnlyEnemies && !p.enemy) continue;
            if (!(p.enemy ? npcShowEnemies : p.police ? npcShowPolice : p.vip ? npcShowVip : p.ally ? npcShowAllies : npcShowCivilians)) continue;
            if (p.health <= 0 && !npcShowDead) continue;
            float dist = -1.f;
            if (haveOrigin)
            {
                float dx = p.x - ox, dy = p.y - oy, dz = p.z - oz;
                dist = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f;
                if (dist > npcMaxDistanceMeters) continue;
                if (dist < 0.5f) continue; // that's us
            }
            float sx = 0.f, sy = 0.f, hx = 0.f, hy = 0.f, fx = 0.f, fy = 0.f;
            if (!WorldToScreen(p.x, p.y, p.z, sx, sy)) continue;
            ImU32 color = p.health <= 0 ? IM_COL32(140, 140, 140, 255)
                                        : (p.enemy ? IM_COL32(255, 50, 50, 255)          // enemy: red
                                        : (p.police ? IM_COL32(120, 150, 255, 255)       // police: blue
                                        : (p.vip ? IM_COL32(70, 200, 255, 255)           // VIP / rescue target: cyan
                                        : (p.ally ? IM_COL32(70, 230, 90, 255)           // ally: green
                                                  : IM_COL32(255, 220, 60, 255)))));     // ordinary NPC: yellow
            float top = sy;
            bool drewBoneBox = false;
            if (p.hasBones)
            {
                float bx[Npc::kMaxBones], by[Npc::kMaxBones]; bool ok[Npc::kMaxBones];
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (int b = 0; b < p.boneCount; ++b)
                {
                    ok[b] = WorldToScreen(p.bone[b][0], p.bone[b][1], p.bone[b][2], bx[b], by[b]);
                    if (!ok[b]) continue;
                    if (bx[b] < minX) minX = bx[b];
                    if (bx[b] > maxX) maxX = bx[b];
                    if (by[b] < minY) minY = by[b];
                    if (by[b] > maxY) maxY = by[b];
                }
                if (npcShowSkeleton)
                    for (int b = 0; b < p.boneCount; ++b)
                    {
                        int par = p.parent[b];
                        if (par != b && ok[b] && ok[par])
                            draw->AddLine(ImVec2(bx[b], by[b]), ImVec2(bx[par], by[par]), color, 1.5f);
                    }
                if (npcBoneBox && maxX > minX && maxY > minY)
                {
                    const float pad = 6.f;
                    draw->AddRect(ImVec2(minX - pad, minY - pad), ImVec2(maxX + pad, maxY + pad), color, 0.f, 0, 1.5f);
                    top = maxY + pad;
                    drewBoneBox = true;
                }
                float hsx, hsy;
                if (npcShowHead && WorldToScreen(p.head[0], p.head[1], p.head[2], hsx, hsy))
                    draw->AddCircle(ImVec2(hsx, hsy), 5.f, IM_COL32(255, 255, 255, 255), 12, 2.f);
            }
            if (!drewBoneBox)
            {
                if (npcShowBox && WorldToScreen(p.x, p.y, p.z + 90.f, hx, hy) && WorldToScreen(p.x, p.y, p.z - 90.f, fx, fy))
                {
                    float h = fy - hy;
                    if (h > 2.f && h < 2000.f)
                    {
                        float w = h * 0.4f;
                        draw->AddRect(ImVec2(sx - w * 0.5f, hy), ImVec2(sx + w * 0.5f, fy), color, 0.f, 0, 1.5f);
                        top = fy;
                    }
                }
                else if (!p.hasBones) draw->AddCircleFilled(ImVec2(sx, sy), 3.f, color);
            }

            char lines[4][48]; int lc = 0;
            snprintf(lines[lc++], sizeof(lines[0]), "%s", p.enemy ? "ENEMY" : (p.police ? "POLICE" : (p.vip ? "VIP" : (p.ally ? "ALLY" : "NPC"))));
            if (npcShowHealth) snprintf(lines[lc++], sizeof(lines[0]), "HP %d", p.health);
            if (npcShowType) snprintf(lines[lc++], sizeof(lines[0]), "type %d", p.cityType);
            if (npcShowDistance && dist >= 0.f) snprintf(lines[lc++], sizeof(lines[0]), "%.0fm", dist);
            float ty = top + 2.f;
            for (int k = 0; k < lc; ++k)
            {
                ImVec2 ts = ImGui::CalcTextSize(lines[k]);
                draw->AddText(ImVec2(sx - ts.x * 0.5f + 1, ty + 1), IM_COL32(0, 0, 0, 200), lines[k]);
                draw->AddText(ImVec2(sx - ts.x * 0.5f, ty), k == 0 ? color : IM_COL32(255, 255, 255, 255), lines[k]);
                ty += ts.y;
            }
        }
    }

    void DrawNpcEspTab()
    {
        Binds::Check("Enable NPC ESP", &npcEspEnabled);
        ImGui::TextDisabled("Scans memory for AI pawns about every 1.5 s. Who is who comes from how each pawn treats the player.");
        ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Red - enemies");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.27f, 0.9f, 0.35f, 1.f), "Green - allies");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.27f, 0.78f, 1.f, 1.f), "Cyan - VIP (the NPC to protect)");
        ImGui::TextColored(ImVec4(0.47f, 0.59f, 1.f, 1.f), "Blue - police");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.86f, 0.24f, 1.f), "Yellow - city NPCs");
        Binds::Check("Only enemies", &npcOnlyEnemies);
        ImGui::BeginDisabled(npcOnlyEnemies);
        Binds::Check("Enemies", &npcShowEnemies);
        ImGui::SameLine();
        Binds::Check("Allies", &npcShowAllies);
        ImGui::SameLine();
        Binds::Check("VIP", &npcShowVip);
        ImGui::SameLine();
        Binds::Check("Police", &npcShowPolice);
        ImGui::SameLine();
        Binds::Check("City NPCs", &npcShowCivilians);
        ImGui::EndDisabled();
        Binds::Check("Dead", &npcShowDead);
        Binds::Check("Box", &npcShowBox);
        ImGui::SameLine();
        Binds::Check("Box from bones", &npcBoneBox);
        ImGui::SameLine();
        Binds::Check("Skeleton", &npcShowSkeleton);
        ImGui::SameLine();
        Binds::Check("Head marker", &npcShowHead);
        Binds::Check("Health", &npcShowHealth);
        ImGui::SameLine();
        Binds::Check("Distance", &npcShowDistance);
        ImGui::SameLine();
        Binds::Check("Type id", &npcShowType);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Max distance (m)##npc", &npcMaxDistanceMeters, 10.f, 1000.f, "%.0f");
        static Npc::Info dbg[256];
        ImGui::Text(TR("Tracked pawns: %d"), Npc::Snapshot(dbg, 256));
    }
    void DrawNpcTab()
    {
        g_npcTabTick = GetTickCount64();
        ImGui::Text(TR("Tracked: %d enemies, %d allies (scan runs about every 1.5 s)"), NpcMod::statEnemies, NpcMod::statAllies);
        if (debugEnabled) ImGui::TextDisabled("Enemy = m_bIsEnemy without Kismet protection; ally = Kismet-invincible / rescue NPCs (green in the ESP).");

        ImGui::SeparatorText("Stealth");
        Binds::Check("Nobody can see me (tailing / stealth missions: mission targets, pedestrians and police are blinded)", &NpcMod::blindAll);
        if (debugEnabled) ImGui::TextDisabled("Sets m_bBlind on the vision component of every AI pawn, including the person you are told to follow. It cannot stop a mission script that measures distance or speed on its own.");
        ImGui::SeparatorText("Enemies");
        Binds::Action("Disarm all enemies now", "npc_disarm");
        ImGui::SameLine();
        Binds::Check("keep disarmed", &NpcMod::keepDisarmed);
        if (debugEnabled) ImGui::TextDisabled("Sets bNoWeaponFiring on the pawn - enemies can't fire their weapons.");
        Binds::Check("Enemies ignore me (blind, no target, forget target)", &NpcMod::enemiesIgnore);

        ImGui::SeparatorText("Allies");
        Binds::Check("Allies invincible", &NpcMod::alliesInvincible);
        Binds::Check("Keep allies at full health", &NpcMod::alliesKeepHealth);
        Binds::Action("Heal allies now", "npc_heal");

        if (!debugEnabled) return;   // the protected-NPC list is a developer tool
        ImGui::SeparatorText("Invincibility");
        Binds::Check("Make invincible enemies vulnerable (bosses / scripted, kept up continuously)", &NpcMod::enemiesVulnerable);
        ImGui::TextDisabled("Nearest protected NPCs and enemies - tick to protect, untick to make vulnerable:");
        {
            static Npc::Info list[256];
            int n = Npc::Snapshot(list, 256, false);
            float ox = 0, oy = 0, oz = 0;
            bool haveOrigin = GetPlayerPosition(ox, oy, oz);
            struct Row { int idx; float dist; };
            Row rows[256]; int rc = 0;
            for (int i = 0; i < n; ++i)
            {
                const Npc::Info& p = list[i];
                if (p.health <= 0 || !(p.invincible || p.rawEnemy)) continue;
                float dx = p.x - ox, dy = p.y - oy, dz = p.z - oz;
                rows[rc++] = { i, haveOrigin ? std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f : 0.f };
            }
            for (int a = 1; a < rc; ++a) { Row r = rows[a]; int b = a - 1; while (b >= 0 && rows[b].dist > r.dist) { rows[b + 1] = rows[b]; --b; } rows[b + 1] = r; }
            if (rc == 0) ImGui::TextDisabled("(none tracked right now)");
            if (rc > 0 && ImGui::BeginTable("npcinv", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("HP"); ImGui::TableSetupColumn("Dist");
                ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Invincible");
                ImGui::TableHeadersRow();
                for (int r = 0; r < rc && r < 16; ++r)
                {
                    const Npc::Info& p = list[rows[r].idx];
                    ImGui::PushID(static_cast<int>(p.address));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(p.enemy ? "ENEMY" : (p.police ? "POLICE" : (p.vip ? "VIP" : (p.ally ? "ALLY" : "NPC"))));
                    ImGui::TableNextColumn(); ImGui::Text("%d", p.health);
                    ImGui::TableNextColumn(); ImGui::Text("%.0fm", rows[r].dist);
                    ImGui::TableNextColumn(); ImGui::Text("%d", p.cityType);
                    ImGui::TableNextColumn();
                    bool inv = p.invincible;
                    if (Binds::Check("##inv", &inv)) NpcMod::SetInvincible(p.address, inv);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("ENEMY* = enemy flag on a protected pawn (boss / scripted target).");
        }
    }
    bool GetCameraMode(uintptr_t& mode)
    {
        static ULONGLONG lastTry = 0;
        auto valid = [](uintptr_t cam)
        {
            uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0; int32_t ctrl = 0;
            void* c = reinterpret_cast<void*>(cam);
            return cam && SafeReadInt(c, 0x4A8, reinterpret_cast<int32_t&>(s0)) && SafeReadInt(c, 0x4AC, reinterpret_cast<int32_t&>(s1)) &&
                   SafeReadInt(c, 0x4B0, reinterpret_cast<int32_t&>(s2)) && SafeReadInt(c, 0x4B4, reinterpret_cast<int32_t&>(s3)) &&
                   s0 == 0x42480000 && s1 == 0x42480000 && s2 == 0x40A00000 && s3 == 0x40A00000 &&
                   SafeReadInt(c, 0x488, ctrl) && ctrl == -1;
        };
        if (!valid(g_wheelmanCam))
        {
            g_wheelmanCam = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastTry < 3000) return false;
            lastTry = now;
            g_wheelmanCam = FindWheelmanCamera();
            if (!g_wheelmanCam) return false;
        }
        int32_t m = 0;
        if (!SafeReadInt(reinterpret_cast<void*>(g_wheelmanCam), 0x47C, m) || static_cast<uint32_t>(m) < 0x10000) return false;
        mode = static_cast<uintptr_t>(static_cast<uint32_t>(m));
        return true;
    }
    uintptr_t GetPlayerPawn()
    {
        static ULONGLONG lastTry = 0;
        if (g_realPawn && !PawnSignatureOk(g_realPawn)) g_realPawn = 0;
        if (!g_realPawn)
        {
            ULONGLONG now = GetTickCount64();
            if (now - lastTry < 3000) return 0;
            lastTry = now;
            g_realPawn = FindRealPawn();
        }
        return g_realPawn;
    }

    bool GetPlayerPosition(float& x, float& y, float& z)
    {
        static ULONGLONG lastTry = 0;
        if (g_realPawn && !PawnSignatureOk(g_realPawn)) g_realPawn = 0;
        if (!g_realPawn)
        {
            ULONGLONG now = GetTickCount64();
            if (now - lastTry < 3000) return false;
            lastTry = now;
            g_realPawn = FindRealPawn();
            if (!g_realPawn) return false;
        }
        void* p = reinterpret_cast<void*>(g_realPawn);
        return SafeReadFloat(p, 0xD4, x) && SafeReadFloat(p, 0xD8, y) && SafeReadFloat(p, 0xDC, z);
    }

    void DrawAimbotTab()
    {
        Binds::Check("Aimbot enabled", &Aimbot::enabled);
        ImGui::TextDisabled("Moves the mouse towards the head of the closest target inside the FOV circle. No wall check.");
        static const char* keys[] = { "Right mouse (aim)", "Left mouse", "Alt", "Shift", "X", "Mouse 4", "Always on" };
        ImGui::SetNextItemWidth(200);
        ImGui::Combo("Active while holding", &Aimbot::aimKey, keys, IM_ARRAYSIZE(keys));
        Binds::Check("Experimental aiming: rotate the camera directly (no mouse input, no jitter; on-foot camera)", &Aimbot::experimental);
        if (Aimbot::experimental && debugEnabled)
        {
            if (Aimbot::statMode)
                ImGui::TextDisabled("  camera mode 0x%08X  desired boom pitch/yaw = %d / %d", static_cast<uint32_t>(Aimbot::statMode), Aimbot::statBoomPitch, Aimbot::statBoomYaw);
            else
                ImGui::TextDisabled("  waiting for a target / this camera mode isn't supported (vehicle or cinematic camera)");
        }
        Binds::Check("Only enemies (m_bIsEnemy)", &Aimbot::onlyEnemies);
        ImGui::SliderFloat("FOV radius (px)", &Aimbot::fovPixels, 30.f, 900.f, "%.0f");
        ImGui::SliderFloat("Smoothing (higher = faster)", &Aimbot::smoothing, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat("Max distance (m)", &Aimbot::maxDistance, 10.f, 500.f, "%.0f");
        Binds::Check("Aim at the head bone (skeleton)", &Aimbot::useBoneHead);
        ImGui::SliderFloat("Head height above pawn location (fallback)", &Aimbot::headHeight, 0.f, 150.f, "%.0f");
        ImGui::SliderFloat("Crosshair offset X (px)", &Aimbot::offsetX, -200.f, 200.f, "%.0f");
        ImGui::SliderFloat("Crosshair offset Y (px)", &Aimbot::offsetY, -200.f, 200.f, "%.0f");
        Binds::Check("Draw FOV circle / target", &Aimbot::drawFov);
        ImGui::SeparatorText("AutoShot");
        Binds::Check("AutoShot when the crosshair reaches the head", &Aimbot::autoShot);
        ImGui::SliderFloat("Head hit radius (px)", &Aimbot::shotRadius, 3.f, 60.f, "%.0f");
        ImGui::SliderInt("Shot interval (ms)", &Aimbot::shotIntervalMs, 40, 500);
        if (!debugEnabled) return;   // the counters below are for developers
        ImGui::SeparatorText("Status");
        ImGui::Text("Targets inside FOV: %d   error: %.0f, %.0f px", Aimbot::statTargets, Aimbot::statErrX, Aimbot::statErrY);
        ImGui::Text("Mouse reads by the game: GetDeviceState %ld / GetDeviceData %ld / GetCursorPos %ld   motion injected: %ld",
                    Aimbot::statStateCalls, Aimbot::statDataCalls, Aimbot::statCursorCalls, Aimbot::statInjected);
        ImGui::TextDisabled("Motion goes into DirectInput reads; if the game makes none it falls back to GetCursorPos.");
        ImGui::Text("AutoShot: crosshair-to-head %.0f px (radius %.0f) -> %s   fire seen through DirectInput: %ld times",
                    Aimbot::statDist, Aimbot::shotRadius, Aimbot::statAligned ? "ALIGNED" : "not aligned", Aimbot::statFireInjected);
    }
    void DrawESPOverlay()
    {
        DrawNpcESP();
        Aimbot::Draw();
        if (!espEnabled) return;
        if (espShowSelf && g_pVehicle)
            DrawEspEntry(g_pVehicle, true);
        for (int i = 0; i < g_vehicleListCount; ++i)
        {
            void* v = g_vehicleList[i].ptr;
            if (!v || v == g_pVehicle) continue;
            DrawEspEntry(v, false);
        }
    }

    void DrawCrashTab()
    {
        ImGui::TextWrapped("Crash reports and minidumps are written to:");
        ImGui::TextDisabled("%s", CrashHandler::Dir());
        if (ImGui::Button("Open crash folder"))
        {
            CreateDirectoryA(CrashHandler::Dir(), nullptr);
            ShellExecuteA(nullptr, "open", CrashHandler::Dir(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        const char* lines[40];
        int n = CrashHandler::PreviousReport(lines, 40);
        if (n == 0)
        {
            ImGui::TextDisabled("No crash report from an earlier run.");
            return;
        }
        ImGui::Text(TR("Latest report: %s"), CrashHandler::PreviousReportPath());
        ImGui::BeginChild("CrashText", ImVec2(0, 340), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (int i = 0; i < n; ++i)
        {
            bool head = i < 4;
            ImGui::TextColored(head ? ImVec4(1.f, 0.6f, 0.4f, 1.f) : ImVec4(0.85f, 0.85f, 0.85f, 1.f), "%s", lines[i]);
        }
        ImGui::EndChild();
    }
    void DrawDebugTab()
    {
        if (!ImGui::BeginTabBar("DebugTabs")) return;
        if (ImGui::BeginTabItem("Tools")) { DrawDebugToolsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Crash")) { DrawCrashTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Vehicle fields")) { DrawFieldsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Player fields")) { DrawPlayerFieldsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Player physics")) { DrawPlayerPhysicsDebug(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Player flags")) { DrawPlayerFlagsDebug(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Cameras")) { DrawCameraTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("ESP")) { DrawESPDebugTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    // ---- Combat / Visuals: tabs that group the existing sections ----------------
    void DrawCombatTab()
    {
        if (!ImGui::BeginTabBar("CombatTabs")) return;
        if (ImGui::BeginTabItem("Aimbot")) { DrawAimbotTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Enemies / allies")) { DrawNpcTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    // The engine's own console commands (hidden in the shipped game): shadows, ambient occlusion, show flags, ...
    void DrawGameGraphicsTab()
    {
        // The best values are already the defaults; the sliders are only unlocked on request, for experiments.
        static bool unlocked = false;
        ImGui::SeparatorText("Graphics");
        Binds::Check("Enable post-processing", &PostFx::enabled);   // the master switch stays visible
        ImGui::Checkbox("Unlock graphics settings (for experiments only)", &unlocked);
        ImGui::TextDisabled("The best picture settings are already applied: 16x anisotropic filtering, sharper textures, anti-aliasing, ambient occlusion, distance haze and depth of field.");
        if (!unlocked) return;
        ImGui::SeparatorText("Image quality");
        ImGui::SetNextItemWidth(240); ImGui::SliderInt("Anisotropic filtering (1 = as the game)", &GfxBoost::anisotropy, 1, 16);
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Texture sharpness (LOD bias)", &GfxBoost::lodBias, -2.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Nothing is written to the game files. Anisotropic filtering keeps roads and walls sharp at an angle; a lower texture sharpness value makes all textures crisper (too low: shimmering).");
        ImGui::SeparatorText("Post-processing");
        if (debugEnabled) ImGui::TextDisabled("%s", TR(PostFx::status));
        ImGui::SeparatorText("Picture");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Anti-aliasing (FXAA)", &PostFx::fxaa, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Sharpening", &PostFx::sharpen, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Saturation", &PostFx::saturation, 0.5f, 1.6f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Contrast", &PostFx::contrast, 0.7f, 1.4f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Gamma (brightness of mid-tones)", &PostFx::gamma, 0.6f, 1.6f, "%.2f");
        ImGui::SeparatorText("Ambient occlusion (SSAO)");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Strength##ao", &PostFx::aoStrength, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Radius (cm)##ao", &PostFx::aoRadius, 20.0f, 400.0f, "%.0f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Bias##ao", &PostFx::aoBias, 0.0f, 0.5f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Fades out at (cm)##ao", &PostFx::aoMaxDistance, 1000.0f, 30000.0f, "%.0f");
        ImGui::SeparatorText("Distance haze");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Strength##haze", &PostFx::fogStrength, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Starts at (m)##haze", &PostFx::fogStart, 0.0f, 500.0f, "%.0f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Distance (m)##haze", &PostFx::fogDistance, 50.0f, 3000.0f, "%.0f");
        { float col[3] = { PostFx::fogR, PostFx::fogG, PostFx::fogB }; ImGui::SetNextItemWidth(240); if (ImGui::ColorEdit3("Colour##haze", col)) { PostFx::fogR = col[0]; PostFx::fogG = col[1]; PostFx::fogB = col[2]; } }
        ImGui::SeparatorText("Depth of field");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Blur amount (pixels, 0 = off)", &PostFx::dofAmount, 0.0f, 20.0f, "%.1f");
        Binds::Check("Focus on the middle of the screen", &PostFx::dofAuto);
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Focus distance (m, manual)", &PostFx::dofFocus, 1.0f, 300.0f, "%.0f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Sharp zone (m)", &PostFx::dofZone, 0.0f, 100.0f, "%.0f");
        ImGui::SetNextItemWidth(240); ImGui::SliderFloat("Blur builds up over (m)", &PostFx::dofTransition, 2.0f, 200.0f, "%.0f");
        if (ImGui::Button("Reset post-processing"))
        {
            PostFx::fogStrength = 1.0f; PostFx::fogStart = 215.0f; PostFx::fogDistance = 483.0f; PostFx::fogR = 154.0f / 255.0f; PostFx::fogG = 198.0f / 255.0f; PostFx::fogB = 226.0f / 255.0f;
            PostFx::dofAmount = 2.0f; PostFx::dofFocus = 112.0f; PostFx::dofZone = 100.0f; PostFx::dofTransition = 200.0f; PostFx::dofAuto = false;
            PostFx::fxaa = 1.0f; PostFx::sharpen = 0.35f; PostFx::saturation = 1.0f; PostFx::contrast = 1.0f; PostFx::gamma = 1.0f;
            PostFx::aoStrength = 1.0f; PostFx::aoRadius = 77.0f; PostFx::aoBias = 0.0f; PostFx::aoMaxDistance = 30000.0f;
        }
        if (!debugEnabled) return;   // everything below is diagnostics / developer tools
        ImGui::Checkbox("Show the depth (debug)", &PostFx::debugDepth); ImGui::SameLine();
        ImGui::Checkbox("Show the occlusion only (debug)", &PostFx::debugAO);
        ImGui::TextDisabled("%s", PostFx::depthStatus);
        ImGui::TextDisabled("One set of shaders over the finished picture (including the HUD). The scene depth is read by turning the game's depth buffer into an INTZ texture.");
        ImGui::SeparatorText("Engine console commands");
        ImGui::TextDisabled("The game has the usual Unreal 3 console commands but no console. The commands run, but most have no visible effect in this build (the rendering ignores them).");
        ImGui::SeparatorText("Shadows and lighting");
        for (int q = 0; q <= 3; ++q)
        {
            char label[40]; snprintf(label, sizeof(label), "Shadow quality %d##sq%d", q, q);
            if (q) ImGui::SameLine();
            if (ImGui::Button(label)) { char cmd[32]; snprintf(cmd, sizeof(cmd), "SHADOWQUALITY %d", q); GameConsole::Run(cmd); }
        }
        if (ImGui::Button("Soft shadow filter (on/off)")) GameConsole::Run("TOGGLEBPCF");
        ImGui::SameLine(); if (ImGui::Button("Ambient occlusion (on/off)")) GameConsole::Run("TOGGLEAO");
        ImGui::SameLine(); if (ImGui::Button("Variance shadow maps (on/off)")) GameConsole::Run("TOGGLEVSM");
        ImGui::SeparatorText("Show / hide (each button flips the flag)");
        static const char* flags[] = { "FOG", "POSTPROCESS", "DECALS", "DYNAMICSHADOWS", "PARTICLES", "FOLIAGE", "STATICMESHES", "SKELMESHES", "TERRAIN", "BSP", "UNLITTRANSLUCENCY" };
        static const char* flagLabels[] = { "Fog", "Post-processing", "Decals", "Dynamic shadows", "Particles", "Foliage", "Static meshes", "Skeletal meshes", "Terrain", "BSP", "Unlit translucency" };
        for (int i = 0; i < 11; ++i)
        {
            char label[64]; snprintf(label, sizeof(label), "%s##sh%d", TR(flagLabels[i]), i);
            if (i % 4) ImGui::SameLine();
            if (ImGui::Button(label)) { char cmd[48]; snprintf(cmd, sizeof(cmd), "SHOW %s", flags[i]); GameConsole::Run(cmd); }
        }
        ImGui::SeparatorText("Any console command");
        static char input[200] = "";
        ImGui::SetNextItemWidth(360);
        const bool enter = ImGui::InputText("##gcmd", input, sizeof(input), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Run") || enter) && input[0]) GameConsole::Run(input);
        ImGui::SameLine(); ImGui::TextDisabled("%s", TR(GameConsole::status));
        ImGui::TextDisabled("Examples: DRAWDISTANCE, SHADOWDEPTHBIAS 0.5, SHADOWRADIUS 1.0, SETRES 1920x1080, FPS, VIEWMODE LIGHTINGONLY, VIEWMODE LIT, STREAMLEVELS");
        if (GameConsole::Output()[0]) { ImGui::BeginChild("gcout", ImVec2(0, 90), true); ImGui::TextUnformatted(GameConsole::Output()); ImGui::EndChild(); }
    }

    void DrawVisualsTab()
    {
        if (!ImGui::BeginTabBar("VisualsTabs")) return;
        if (ImGui::BeginTabItem("Vehicles")) { DrawESPTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("NPCs")) { DrawNpcEspTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("HUD")) { DrawHudSection(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Graphics")) { DrawGameGraphicsTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    // ---- Settings ---------------------------------------------------------------
    void DrawSettingsTab()
    {
        {
            // UI language: saved in WheelmanMod_settings.txt; the first start takes it from the launcher's -language= argument.
            ImGui::SeparatorText("Language / Язык");
            int lang = Lang::current;
            ImGui::SetNextItemWidth(200);
            static const char* names[] = { "English", "Русский" };
            if (ImGui::Combo("##uilang", &lang, names, 2)) { Lang::current = lang; Binds::SaveSettings(); }
        }
        ImGui::SeparatorText("Loading protection");
        Binds::Check("Stop writing into the game while it loads / plays a cut-scene (the interface stays)", &LoadGuard::enabled);
        ImGui::SetNextItemWidth(200); ImGui::SliderInt("Extra quiet time after loading (s)", &LoadGuard::settleSeconds, 1, 30);
        ImGui::SetNextItemWidth(200); ImGui::SliderInt("Slow motion counts as a cut-scene after (s)", &LoadGuard::slowGraceSeconds, 1, 30);
        if (LoadGuard::Quiet()) ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), TR("quiet: %s, %d s left"), TR(LoadGuard::Reason()), LoadGuard::SecondsLeft());
        else ImGui::TextDisabled("active (the world is stable)");
        ImGui::SeparatorText("Hotkeys");
        {
            const int cap = Binds::SystemKeyCapturing();
            char b1[96], b2[96];
            snprintf(b1, sizeof(b1), "%s: %s##menukey", TR("Menu key"), cap == 1 ? TR("press a key...") : Binds::KeyNameOf(Binds::menuKey));
            snprintf(b2, sizeof(b2), "%s: %s##cursorkey", TR("Mouse cursor key"), cap == 2 ? TR("press a key...") : Binds::KeyNameOf(Binds::cursorKey));
            if (ImGui::Button(b1, ImVec2(260, 0))) Binds::BeginSystemKeyCapture(1);
            if (ImGui::Button(b2, ImVec2(260, 0))) Binds::BeginSystemKeyCapture(2);
            ImGui::SameLine();
            if (ImGui::SmallButton("Defaults (Insert / End)")) { Binds::menuKey = VK_INSERT; Binds::cursorKey = VK_END; Binds::SaveSettings(); }
            ImGui::TextDisabled("The cursor key captures / releases the mouse so the overlay can be clicked. Esc cancels the key choice. Home is left free for the game.");
        }
        ImGui::SeparatorText("Configs");
        static char nameBuf[48] = "default";
        static int sel = -1;
        static std::vector<std::string> cfgs;
        static int refreshIn = 0;
        if (refreshIn-- <= 0) { cfgs = Binds::ListConfigs(); refreshIn = 60; }
        ImGui::TextDisabled("A config stores every toggle and slider value plus all binds.");
        if (ImGui::BeginListBox("##cfglist", ImVec2(260, 110)))
        {
            for (int i = 0; i < static_cast<int>(cfgs.size()); ++i)
                if (ImGui::Selectable(cfgs[i].c_str(), i == sel))
                {
                    sel = i;
                    strncpy_s(nameBuf, cfgs[i].c_str(), _TRUNCATE);
                }
            ImGui::EndListBox();
        }
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
        if (ImGui::Button("Save")) { Binds::SaveConfig(nameBuf); refreshIn = 0; }
        ImGui::SameLine();
        if (ImGui::Button("Load")) Binds::LoadConfig(nameBuf);
        ImGui::SameLine();
        if (ImGui::Button("Delete")) { Binds::DeleteConfig(nameBuf); refreshIn = 0; sel = -1; }
        ImGui::SameLine();
        if (ImGui::Button("Open folder")) { ShellExecuteA(nullptr, "open", Binds::ConfigDir(), nullptr, nullptr, SW_SHOWNORMAL); }
        ImGui::TextDisabled("%s", Binds::lastMessage);
        bool autoLoad = strcmp(Binds::autoloadConfig, nameBuf) == 0 && nameBuf[0];
        if (ImGui::Checkbox("Load this config when the game starts", &autoLoad))
        {
            strncpy_s(Binds::autoloadConfig, autoLoad ? nameBuf : "", _TRUNCATE);
            Binds::SaveSettings();
        }
        if (Binds::autoloadConfig[0]) ImGui::TextDisabled(TR("auto-load: %s"), Binds::autoloadConfig);

        ImGui::SeparatorText("Binds");
        if (ImGui::Checkbox("Show the active binds window", &Binds::showWindow)) Binds::SaveSettings();
        ImGui::SameLine();
        if (ImGui::Checkbox("Show all binds (inactive ones are marked false)", &Binds::showAll)) Binds::SaveSettings();
        Binds::DrawSettingsBinds();

        ImGui::SeparatorText("Advanced");
        ImGui::Checkbox("Debug - show technical information and the Debug tab (offsets, scanners, developer tools)", &debugEnabled);
        ImGui::Checkbox("Unsafe - unlock experimental options (unloaded vehicle definitions in Vehicle -> Spawn)", &unsafeEnabled);
        if (unsafeEnabled) ImGui::TextColored(ImVec4(1.f, 0.66f, 0.16f, 1.f), "Unsafe options are on: they can crash the game.");
        ImGui::TextDisabled("Both switches are off again every time the game starts.");

        ImGui::SeparatorText("About");
        {
            char kb1[48], kb2[48];
            snprintf(kb1, sizeof(kb1), "%s", Binds::KeyNameOf(Binds::menuKey)); snprintf(kb2, sizeof(kb2), "%s", Binds::KeyNameOf(Binds::cursorKey));
            ImGui::TextDisabled(TR("%s - show / hide this menu      %s - release / capture the mouse"), kb1, kb2);
        }
        ImGui::TextDisabled("Right-click any option (or button) to bind a key to it.");
    }

    // ---- Map tab: markers and the minimap ---------------------------------------------------------------------------
    void DrawMinimapSection()
    {
        ImGui::SeparatorText("External minimap");
        ImGui::TextDisabled("A movable overlay map (round, GTA-style) with the markers below. Mouse wheel over it zooms; drag it while the mouse is captured (cursor key, End by default).");
        Binds::Check("Show the external minimap", &MiniMap::enabled);
        Binds::Check("Map turns with me (heading up)", &MiniMap::rotate);
        ImGui::SameLine();
        Binds::Check("Square minimap", &MiniMap::square);
        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Map radius (m)", &MiniMap::rangeMeters, 60.f, 4000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(200); ImGui::SliderInt("Minimap size (px)", &MiniMap::sizePx, 120, 800);
        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Minimap opacity", &MiniMap::opacity, 0.1f, 1.f, "%.2f");
        ImGui::SetNextItemWidth(200); ImGui::SliderInt("Re-scan markers every (s, 0 = manual)", &MiniMap::autoScanSeconds, 0, 120);
        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Icon size", &MiniMap::iconScale, 0.5f, 3.f, "%.2f");
        Binds::Check("Only markers the game itself shows on its map", &MiniMap::onlyGameVisible);
        if (ImGui::Button("Scan markers##minimap")) MapTools::ScanAsync();

        ImGui::SeparatorText("Map picture");
        if (debugEnabled) ImGui::TextDisabled("The game's own city map (extracted from UI_PDA) drawn behind the markers. Needs MapData\\citymap.dxt1 next to the DLL.");
        Binds::Check("Draw the city map behind the markers", &MiniMap::background);
        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Map picture opacity", &MiniMap::bgOpacity, 0.1f, 1.f, "%.2f");
        ImGui::TextDisabled(TR("Map picture: %s"), MiniMap::BackgroundStatus());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##mapbg")) MiniMap::ReloadBackground();
        if (ImGui::CollapsingHeader("Map calibration (world -> picture)"))
        {
            ImGui::TextDisabled("px = (world Y - origin Y) / cm per px,  py = (origin X - world X) / cm per px. Change only if markers do not sit on their roads.");
            ImGui::SetNextItemWidth(200); ImGui::DragFloat("Origin X (cm, top edge)", &MiniMap::mapOriginX, 100.f, -2.0e6f, 2.0e6f, "%.0f");
            ImGui::SetNextItemWidth(200); ImGui::DragFloat("Origin Y (cm, left edge)", &MiniMap::mapOriginY, 100.f, -2.0e6f, 2.0e6f, "%.0f");
            ImGui::SetNextItemWidth(200); ImGui::DragFloat("Centimetres per map pixel", &MiniMap::mapCmPerPx, 0.1f, 50.f, 600.f, "%.2f");
            if (ImGui::SmallButton("Default calibration")) { MiniMap::mapOriginX = 384610.f; MiniMap::mapOriginY = -856220.f; MiniMap::mapCmPerPx = 225.f; }
        }

        ImGui::SeparatorText("Lay over the game's minimap");
        ImGui::TextDisabled("Zoom and rotation follow the game's own HUD minimap. Move this window (cursor key, drag) onto the game's minimap and set the same size.");
        Binds::Check("Match the game's minimap (zoom and rotation)", &MiniMap::matchGame);
        ImGui::BeginDisabled(!MiniMap::matchGame);
        Binds::Check("Synchronise the zoom", &MiniMap::matchScale);
        ImGui::SameLine();
        Binds::Check("Synchronise the rotation", &MiniMap::matchRotation);
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Match zoom fine-tune", &MiniMap::matchZoom, 0.3f, 3.f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Match rotation offset (deg)", &MiniMap::matchYawOffsetDeg, -180.f, 180.f, "%.0f");
        Binds::Check("Invert the matched rotation", &MiniMap::matchYawInvert);
        {
            float sc = 0, mw = 0, yw = 0;
            if (MiniMap::ReadGameMinimapValues(sc, mw, yw))
                ImGui::TextDisabled(TR("game minimap: scale %.4f, width %.0f, yaw %.1f deg"), sc, mw, yw * 360.f / 65536.f);
            else ImGui::TextDisabled("game minimap: no running HUD screen found yet");
        }
        ImGui::SeparatorText("Icons");
        ImGui::TextDisabled("Markers are sorted by their game icon. Hover an icon on the minimap to see its exact type.");
        {
            ImGui::TextDisabled("Every category has its own size (multiplied by 'Icon size'); the mission badge is drawn larger than the pins, so it starts smaller.");
            for (int c = 0; c < MiniMap::kCategoryCount; ++c)
            {
                const ImVec4 cc = ImGui::ColorConvertU32ToFloat4(MiniMap::CategoryColor(c));
                ImGui::PushID(c);
                ImGui::PushStyleColor(ImGuiCol_CheckMark, cc);
                Binds::Check(MiniMap::CategoryLabel(c), &MiniMap::catOn[c]);
                ImGui::PopStyleColor();
                ImGui::SameLine(300);
                ImGui::SetNextItemWidth(130);
                ImGui::SliderFloat("##catsize", &MiniMap::catScale[c], 0.3f, 3.f, "size %.2f");
                ImGui::PopID();
            }
            if (ImGui::SmallButton("Reset all sizes")) for (int c = 0; c < MiniMap::kCategoryCount; ++c) MiniMap::catScale[c] = (c == MiniMap::CatStory ? 0.75f : 1.f);
            Binds::Check("Police influence zone (circle, like on the game's minimap)", &MiniMap::showPoliceZone);
            ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Police zone size", &MiniMap::zoneScale, 0.25f, 4.f, "%.2f");
        }
        {
            // how many markers of each category the last scan found
            int cnt[MiniMap::kCategoryCount] = {};
            for (int i = 0; i < MapTools::Count(); ++i)
            {
                MapTools::Row r{};
                if (!MapTools::At(i, r) || (r.x == 0.f && r.y == 0.f && r.z == 0.f)) continue;
                const int c = MiniMap::CategoryOfRow(r);
                if (c >= 0) ++cnt[c];
            }
            if (MapTools::Count())
            {
                ImGui::TextDisabled("Found by the last scan:");
                for (int c = 0; c < MiniMap::kMarkerCategoryCount; ++c)
                    if (cnt[c]) { ImGui::SameLine(); ImGui::TextDisabled("%s %d;", TR(MiniMap::CategoryLabel(c)), cnt[c]); }
            }
        }
    }

    void DrawMarkersSection()
    {
        ImGui::TextWrapped("Everything the game puts on the map: garages, weapon / ammo caches, mission and event markers, safe houses, taxi drop-offs, potatoes and checkpoints. Scan while in the world.");
        ImGui::SeparatorText("Markers");
        if (ImGui::Button("Scan markers##map")) MapTools::ScanAsync();
        ImGui::TextDisabled("Only reading is done by default: the game's own minimap is left exactly as it is. The options below rewrite the map flags of the game's markers and need the Unsafe switch (Settings).");
        ImGui::BeginDisabled(!unsafeEnabled);
        if (ImGui::Button("Show all garages on the map now")) MapTools::ShowAllNow(true);
        ImGui::SameLine();
        if (ImGui::Button("Show every marker with an icon now")) MapTools::ShowAllNow(false);
        Binds::Check("Keep garages on the map", &MapTools::showGaragesOnMap);
        Binds::Check("Keep every marker with an icon on the map", &MapTools::showAllOnMap);
        ImGui::EndDisabled();
        if (MapTools::message[0]) ImGui::TextDisabled("%s", MapTools::message);

        static int classFilter = -1;   // -1 = all
        static bool onlyWithIcon = true;
        {
            char cur[64];
            snprintf(cur, sizeof(cur), "%s", classFilter < 0 ? TR("All types") : MapTools::ClassName(classFilter));
            ImGui::SetNextItemWidth(300);
            if (ImGui::BeginCombo("Marker type", cur))
            {
                if (ImGui::Selectable(TR("All types"), classFilter < 0)) classFilter = -1;
                for (int c = 0; c < MapTools::ClassCount(); ++c)
                {
                    int n = 0;
                    for (int i = 0; i < MapTools::Count(); ++i) { MapTools::Row r{}; if (MapTools::At(i, r) && r.cls == c) ++n; }
                    char l[96]; snprintf(l, sizeof(l), "%s  (%d)", MapTools::ClassName(c), n);
                    ImGui::PushID(c);
                    if (ImGui::Selectable(l, classFilter == c)) classFilter = c;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            Binds::Check("only markers that have an icon", &onlyWithIcon);
        }
        if (MapTools::Count() == 0) ImGui::TextDisabled("press Scan markers while in the world");
        else
        {
            float px = 0, py = 0, pz = 0;
            const bool havePos = GetPlayerPosition(px, py, pz);
            struct MRow { float dist; MapTools::Row r; };
            static std::vector<MRow> rows;
            rows.clear();
            for (int i = 0; i < MapTools::Count(); ++i)
            {
                MapTools::Row r{};
                if (!MapTools::At(i, r)) continue;
                if (classFilter >= 0 && r.cls != classFilter) continue;
                if (onlyWithIcon && r.icon == 0) continue;
                const float d = havePos ? std::sqrt((r.x - px) * (r.x - px) + (r.y - py) * (r.y - py)) / 100.f : 0.f;
                rows.push_back({ d, r });
            }
            std::sort(rows.begin(), rows.end(), [](const MRow& a, const MRow& b) { return a.dist < b.dist; });
            ImGui::TextDisabled(TR("shown: %d of %d markers"), static_cast<int>(rows.size()), MapTools::Count());
            const int cols = debugEnabled ? 7 : 5;
            if (ImGui::BeginTable("maprows", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 300)))
            {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 2.6f);
                ImGui::TableSetupColumn("Map icon", ImGuiTableColumnFlags_WidthStretch, 2.6f);
                ImGui::TableSetupColumn("On map", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableSetupColumn("Distance (m)", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                if (debugEnabled) { ImGui::TableSetupColumn("+0x1C4"); ImGui::TableSetupColumn("+0x1D4"); }
                ImGui::TableHeadersRow();
                for (const MRow& m : rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(m.r.className + (strncmp(m.r.className, "Wheelman", 8) == 0 ? 8 : 0));
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(MapTools::IconName(m.r.icon));
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(m.r.onMap && !m.r.disabled ? "yes" : "no");
                    ImGui::TableNextColumn(); if (m.r.hasState) ImGui::Text("%d", m.r.state); else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); if (havePos) ImGui::Text("%.0f", m.dist); else ImGui::TextDisabled("-");
                    if (debugEnabled)
                    {
                        ImGui::TableNextColumn(); ImGui::Text("%08X", m.r.rawA);
                        ImGui::TableNextColumn(); ImGui::Text("%08X", m.r.rawB);
                    }
                }
                ImGui::EndTable();
            }
        }

    }

    void DrawHudMapSection()
    {
        ImGui::SeparatorText("Minimap (HUD)");
        if (debugEnabled) ImGui::TextDisabled("The game's own HUD minimap. Only the running HUD screen is changed (the archetype / default objects are left alone).");
        if (ImGui::Button("Find HUD screens")) MapTools::ScanHudAsync();
        if (debugEnabled) ImGui::TextDisabled(TR("HUD screens found: %d"), MapTools::HudCount());
        MapTools::Hud first{};
        bool haveHud = false;
        for (int i = 0; i < MapTools::HudCount() && !haveHud; ++i) haveHud = MapTools::HudAt(i, first) && MapTools::HudLive(first.obj);
        if (!haveHud) ImGui::TextDisabled("no running HUD screen yet (press Find HUD screens in the world)");
        else
        {
            auto forAll = [](auto fn) { for (int i = 0; i < MapTools::HudCount(); ++i) { MapTools::Hud h{}; if (MapTools::HudAt(i, h) && MapTools::HudLive(h.obj)) fn(h.obj); } };
            bool showMap = MapTools::GetBit(first.obj, 0x270, 10), showGps = MapTools::GetBit(first.obj, 0x270, 11);
            if (Binds::Check("Show the minimap (m_bShowMiniMap)", &showMap)) forAll([&](uintptr_t h) { MapTools::SetBit(h, 0x270, 10, showMap); });
            if (Binds::Check("Show the GPS route (m_bShowGPS)", &showGps)) forAll([&](uintptr_t h) { MapTools::SetBit(h, 0x270, 11, showGps); });
            float mscale = MapTools::GetFloat(first.obj, 0x38C);
            int maxm = MapTools::GetInt(first.obj, 0x39C);
            ImGui::SetNextItemWidth(220);
            ImGui::SliderFloat("Minimap zoom multiplier (1 = the game's own)", &MapTools::hudZoom, 0.25f, 4.f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##hudzoom")) MapTools::hudZoom = 1.f;
            if (debugEnabled) ImGui::TextDisabled(TR("m_fMapScale now: %.4f (the game recomputes it itself)"), MapTools::GetFloat(first.obj, 0x36C));
            ImGui::SetNextItemWidth(220);
            if (ImGui::SliderFloat("Map marker scale (m_fMapMarkerScale, game default 7.5)", &mscale, 0.5f, 30.f, "%.2f")) forAll([&](uintptr_t h) { MapTools::SetFloat(h, 0x38C, mscale); });
            ImGui::SameLine();
            if (ImGui::SmallButton("7.5##mscale")) { mscale = 7.5f; forAll([&](uintptr_t h) { MapTools::SetFloat(h, 0x38C, 7.5f); }); }
            ImGui::SetNextItemWidth(220);
            if (ImGui::InputInt("Max markers on the minimap (m_iMaxMapMarkers)", &maxm, 16, 64)) forAll([&](uintptr_t h) { MapTools::SetInt(h, 0x39C, maxm); });
            if (debugEnabled) ImGui::TextDisabled(TR("HUD @ 0x%08X (%s)"), static_cast<uint32_t>(first.obj), first.className);
        }

        if (debugEnabled && ImGui::CollapsingHeader("Map icon types of the game (enum MissionMarkers)"))
        {
            ImGui::BeginChild("icons", ImVec2(0, 220), true);
            for (int i = 0; i < MapTools::IconCount(); ++i) ImGui::Text("%2d  %s", i, MapTools::IconName(i));
            ImGui::EndChild();
        }
    }

    void DrawMapTab()
    {
        if (!ImGui::BeginTabBar("MapTabs")) return;
        if (ImGui::BeginTabItem("Minimap")) { DrawMinimapSection(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("GPS")) { MiniMap::DrawGpsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Markers")) { DrawMarkersSection(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Game HUD")) { DrawHudMapSection(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    // ---- Police tab: the crime table --------------------------------------------------------------------------------
    void DrawPoliceTab()
    {
        DrawPoliceSection();

        ImGui::SeparatorText("Police officers");
        g_npcTabTick = GetTickCount64();
        if (debugEnabled) ImGui::TextDisabled("Officers are recognised by their city type (5). The same mechanisms as for the enemies in Combat.");
        Binds::Check("Police keep disarmed (can't fire their weapons)", &NpcMod::policeDisarmed);
        Binds::Check("Police ignore me (blind, no target, forget target)", &NpcMod::policeIgnore);
        Binds::Action("Disarm all enemies and police now", "npc_disarm");

        // the crime table is read from the game silently; the reload button, the table itself and the escape timers are developer tools
        static ULONGLONG lastAuto = 0;
        if (!CrimeTools::Loaded() && GetTickCount64() - lastAuto > 3000) { lastAuto = GetTickCount64(); CrimeTools::Load(); }
        if (debugEnabled)
        {
            ImGui::SeparatorText("Crime table");
            ImGui::TextWrapped("The game's police crime table (WheelmanPoliceCrimeTable): how much heat every crime adds.");
            if (ImGui::Button("Reload the crime table")) CrimeTools::Load();
            if (CrimeTools::message[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", CrimeTools::message); }
        }
        if (!CrimeTools::Loaded()) { if (debugEnabled) ImGui::TextDisabled("crime table not found (load into the world)"); return; }

        ImGui::SeparatorText("Crime values");
        Binds::Check("Use the values below (writes them into the game every frame)", &CrimeTools::overrideOn);
        ImGui::SetNextItemWidth(220);
        ImGui::SliderFloat("Heat multiplier (only crimes that add heat)", &CrimeTools::changeScale, 0.f, 5.f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("Restore the game's values")) CrimeTools::ResetToOriginal();
        if (!debugEnabled) return;
        ImGui::TextDisabled("Heat change > 0 raises the wanted level, < 0 lowers it over time. 'Max level' is the highest level this crime can push you to.");
        if (ImGui::BeginTable("crimes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 340)))
        {
            ImGui::TableSetupColumn("Crime", ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn("Serious", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Max level", ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("Heat change", ImGuiTableColumnFlags_WidthFixed, 130.f);
            ImGui::TableSetupColumn("Game's value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < CrimeTools::kCrimes; ++i)
            {
                CrimeTools::Crime& c = CrimeTools::edited[i];
                const CrimeTools::Crime& o = CrimeTools::Original(i);
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(TR(CrimeTools::Name(i)));
                ImGui::TableNextColumn(); bool ser = c.serious != 0; if (ImGui::Checkbox("##ser", &ser)) c.serious = ser ? 1 : 0;
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(100); ImGui::SliderInt("##max", &c.maxLevel, 0, 5);
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("##chg", &c.change, 0.005f, -2.f, 5.f, "%.3f");
                ImGui::TableNextColumn(); ImGui::TextDisabled("%d / %.3f%s", o.maxLevel, o.change, o.serious ? " *" : "");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Running around / losing the police");
        ImGui::SetNextItemWidth(180); ImGui::DragFloat("Run-around time (s)", &CrimeTools::runAroundTime, 0.5f, 0.f, 600.f, "%.1f");
        ImGui::SetNextItemWidth(180); ImGui::DragFloat("Run-around distance", &CrimeTools::runAroundDistance, 10.f, 0.f, 100000.f, "%.0f");
        ImGui::SetNextItemWidth(180); ImGui::DragFloat("Run-around reset time (s)", &CrimeTools::runAroundResetTime, 0.5f, 0.f, 600.f, "%.1f");
        ImGui::TextDisabled("Seconds you must stay out of sight to lose the police, per wanted level (-1 = never; applied straight to the game):");
        for (int i = 0; i < CrimeTools::TimeToLoseCount(); ++i)
        {
            float t = CrimeTools::TimeToLose(i);
            char l[48]; snprintf(l, sizeof(l), TR("Level %d##lose"), i);
            ImGui::SetNextItemWidth(160);
            if (ImGui::DragFloat(l, &t, 0.5f, -1.f, 600.f, "%.1f")) CrimeTools::SetTimeToLose(i, t);
        }
    }

    // ---- Mission tab: the game's own mission debug commands (WheelmanPlayerPawn) ---------------------------------
    void DrawMissionTab()
    {
        ImGui::SeparatorText("Missions");
        Binds::Check("Anti-fail: the mission cannot be failed", &BuiltinCheats::antiFailMission);
        if (!debugEnabled) return;   // the rest is a set of developer tools (raw mission commands, marker list, Kismet variables)
        ImGui::TextWrapped("The game's own mission commands (functions of WheelmanPlayerPawn). They act on the mission that is running now; use them in the world.");
        ImGui::SeparatorText("Mission control");
        Binds::Action("Enable missions (EnableMissions)", "mission_enable");
        Binds::Action("Fail the mission (FailMission)", "mission_fail");
        Binds::Action("Complete the mission (EndMission)", "mission_end");
        Binds::Action("Retry the mission (RetryMission)", "mission_retry");
        Binds::Action("Pause the mission (PauseMission)", "mission_pause");
        ImGui::SameLine();
        Binds::Action("Resume the mission (ResumeMission)", "mission_resume");
        ImGui::TextDisabled("Complete / Fail / Retry / Pause / Resume work only while a mission objective is active. 'Enable missions' just turns the mission system back on; it does not start a mission - use the markers below.");
        ImGui::TextDisabled("Anti-fail swallows the game's mission-failed screen (every fail condition of the mission scripts ends in it) and the pawn's FailMission function.");
        ImGui::SeparatorText("Mission and event markers");
        ImGui::TextDisabled("Every mission / event start marker of the loaded world. Teleport puts you on the marker, which starts it (the game's own Teleport).");
        if (ImGui::Button("Scan markers")) BuiltinCheats::ScanMissionMarkers();
        if (BuiltinCheats::MarkerCount() == 0) ImGui::TextDisabled("press Scan markers while in the world");
        else
        {
            float px = 0, py = 0, pz = 0;
            const bool havePos = GetPlayerPosition(px, py, pz);
            struct Row { int index; float dist; BuiltinCheats::MarkerInfo info; };
            static std::vector<Row> rows;
            rows.clear();
            for (int i = 0; i < BuiltinCheats::MarkerCount(); ++i)
            {
                BuiltinCheats::MarkerInfo mi{};
                if (!BuiltinCheats::MarkerAt(i, mi) || (mi.x == 0.f && mi.y == 0.f && mi.z == 0.f)) continue;
                const float d = havePos ? std::sqrt((mi.x - px) * (mi.x - px) + (mi.y - py) * (mi.y - py)) / 100.f : 0.f;
                rows.push_back({ i, d, mi });
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.dist < b.dist; });
            if (ImGui::BeginTable("markers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 260)))
            {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 2.f);
                ImGui::TableSetupColumn("Distance (m)", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("##tp", ImGuiTableColumnFlags_WidthFixed, 90.f);
                ImGui::TableHeadersRow();
                for (const Row& r : rows)
                {
                    ImGui::PushID(r.index);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.info.name);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(TR(r.info.kind));
                    ImGui::TableNextColumn(); if (havePos) ImGui::Text("%.0f", r.dist); else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); if (ImGui::SmallButton("Teleport##marker")) BuiltinCheats::RequestTeleportToMarker(r.index);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::SeparatorText("Debug");
        Binds::Check("Mission debug info (Debug_Mission)", &BuiltinCheats::flags[BuiltinCheats::FlagMissionDebug]);
        // "Freeze timers" lives in the mission timer section of this tab (the same option twice gives duplicate ImGui IDs)
        if (BuiltinCheats::message[0]) ImGui::TextDisabled("%s", BuiltinCheats::message);
    }

    // ---- Kismet variables: mission scripts keep strike counters / flags / timers here ---------------------------------
    void DrawKismetSection()
    {
        ImGui::SeparatorText("Kismet variables (mission scripts)");
        ImGui::TextWrapped("Mission logic lives in Kismet variables. Their names are not stored in memory, so find the one that matters by its behaviour: take a snapshot, let the game do something (a warning, a timer), then show only what changed and lock it.");
        if (ImGui::Button("Scan variables")) KismetVars::Scan();
        ImGui::SameLine();
        if (ImGui::Button("Take a snapshot")) KismetVars::TakeSnapshot();
        ImGui::SameLine();
        if (ImGui::Button("Unlock all")) KismetVars::UnlockAll();
        static bool onlyChanged = true, showBool = true, showFloat = false;
        Binds::Check("Only variables that changed since the snapshot", &onlyChanged);
        ImGui::SameLine(); ImGui::Checkbox("Bool##kv", &showBool);
        ImGui::SameLine(); ImGui::Checkbox("Float##kv", &showFloat);
        ImGui::TextDisabled(TR("%d variables, %d locked"), KismetVars::Count(), KismetVars::LockedCount());
        if (KismetVars::message[0]) ImGui::TextDisabled("%s", KismetVars::message);
        if (KismetVars::Count() > 0 && ImGui::BeginTable("kismet", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 260)))
        {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("Object");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Snapshot");
            ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 50.f);
            ImGui::TableHeadersRow();
            int shown = 0;
            for (int i = 0; i < KismetVars::Count() && shown < 400; ++i)
            {
                KismetVars::Var v{};
                if (!KismetVars::At(i, v)) continue;
                if (onlyChanged && !v.changed && !v.locked) continue;
                if (v.kind == KismetVars::KBool && !showBool) continue;
                if (v.kind == KismetVars::KFloat && !showFloat) continue;
                ++shown;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v.kind == KismetVars::KInt ? "int" : (v.kind == KismetVars::KFloat ? "float" : "bool"));
                ImGui::TableNextColumn(); ImGui::Text("%08X", static_cast<uint32_t>(v.obj));
                ImGui::TableNextColumn();
                {
                    ImGui::SetNextItemWidth(90);
                    if (v.kind == KismetVars::KInt) { int x = static_cast<int32_t>(v.value); if (ImGui::InputInt("##v", &x, 0, 0)) KismetVars::SetValue(i, static_cast<uint32_t>(x)); }
                    else if (v.kind == KismetVars::KFloat) { float x; memcpy(&x, &v.value, 4); if (ImGui::InputFloat("##v", &x, 0.f, 0.f, "%.3f")) { uint32_t r; memcpy(&r, &x, 4); KismetVars::SetValue(i, r); } }
                    else { bool x = v.value != 0; if (ImGui::Checkbox("##v", &x)) KismetVars::SetValue(i, x ? 1u : 0u); }
                }
                ImGui::TableNextColumn();
                if (v.kind == KismetVars::KFloat) { float x; memcpy(&x, &v.snapshot, 4); ImGui::TextDisabled("%.3f", x); }
                else ImGui::TextDisabled("%d", static_cast<int32_t>(v.snapshot));
                ImGui::TableNextColumn();
                bool lk = v.locked;
                if (ImGui::Checkbox("##lock", &lk)) KismetVars::SetLock(i, lk);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- Built-in cheats: the game's own developer cheats (WheelmanCheatManager) and the garages ------------------
    void DrawCheatsTab()
    {
        ImGui::TextWrapped("The game's own developer cheats (class WheelmanCheatManager). It is not created in the shipped game, so its functions are called directly. Load into the world first.");
        if (!ImGui::BeginTabBar("CheatTabs")) return;
        if (ImGui::BeginTabItem("Player cheats")) {
        Binds::Check("God mode (cheat God)", &g_godInvincible);
        Binds::Check("Infinite ammo (cheat InfiniteAmmo)", &BuiltinCheats::ammoGod);
        ImGui::SetNextItemWidth(220);
        ImGui::SliderFloat("Slow motion / game speed (cheat SlowMo)", &BuiltinCheats::timeScale, 0.1f, 2.f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Normal speed")) BuiltinCheats::timeScale = 1.f;
        Binds::Action("Fill the focus gauge (cheat FocusGauge)", "cheat_focus");
        Binds::Action("Kill me (cheat KillMe)", "cheat_killme");
        Binds::Action("Resurrect me here (Resurrect)", "cheat_resurrect");
        Binds::Check("Game's controller God flag (bInvincible)", &BuiltinCheats::flags[BuiltinCheats::FlagControllerGod]);
        Binds::Check("Fake god mode: keep health above the value below (FakeGodModeEnabled)", &BuiltinCheats::flags[BuiltinCheats::FlagFakeGod]);
        ImGui::SetNextItemWidth(160);
        ImGui::InputInt("Fake god mode minimum health", &BuiltinCheats::fakeGodMinHealth, 10, 100);
        ImGui::EndTabItem(); }

        if (ImGui::BeginTabItem("Vehicle cheats")) {
        Binds::Action("Destroy my vehicle (cheat KillVehicle)", "cheat_killveh");
        ImGui::TextDisabled("Upgrade levels of the vehicle you drive (0 = stock). Applied through the game's own upgrade functions.");
        static const char* kNames[3] = { "Performance level (cheat VehiclePerformanceUpgrade)", "Melee mass level (cheat VehicleMeleeMassUpgrade)", "Health level (cheat VehicleHealthUpgrade)" };
        int* levels[3] = { &BuiltinCheats::perfLevel, &BuiltinCheats::meleeLevel, &BuiltinCheats::healthLevel };
        for (int i = 0; i < 3; ++i)
        {
            ImGui::SetNextItemWidth(160);
            ImGui::SliderInt(kNames[i], levels[i], 0, 3);
            const int cur = BuiltinCheats::CurrentUpgrade(i);
            if (cur >= 0) { ImGui::SameLine(); ImGui::TextDisabled(TR("now %d"), cur); }
        }
        Binds::Action("Apply the upgrade levels", "cheat_upgrade");
        ImGui::EndTabItem(); }

        if (ImGui::BeginTabItem("Game's debug displays")) {
        ImGui::TextDisabled("Switches of the game's own debug commands (they draw debug text / shapes if the build supports it).");
        using BuiltinCheats::flags;
        Binds::Check("Carjack debug (ToggleCarjackDebug)", &flags[BuiltinCheats::FlagCarjackDebug]);
        Binds::Check("Damage debug (ToggleDamageDebug)", &flags[BuiltinCheats::FlagDamageDebug]);
        Binds::Check("Melee boost debug (ToggleMeleeBoostDebug)", &flags[BuiltinCheats::FlagMeleeBoostDebug]);
        Binds::Check("Locational damage debug (ToggleLocDamageDebug)", &flags[BuiltinCheats::FlagLocDamageDebug]);
        Binds::Check("Collision impulse debug (ToggleCollisionImpulseDebug)", &flags[BuiltinCheats::FlagImpulseDebug]);
        Binds::Check("Show my vehicle health (ShowPlayerVehicleHealth)", &flags[BuiltinCheats::FlagVehicleHealth]);
        Binds::Check("Show my vehicle damage (ShowPlayerVehicleDamage)", &flags[BuiltinCheats::FlagVehicleDamage]);
        Binds::Check("Detailed vehicle damage (DetailPlayerVehicleDamage)", &flags[BuiltinCheats::FlagVehicleDamageDetail]);
        Binds::Check("Force feedback debug (DebugForceFeedback)", &flags[BuiltinCheats::FlagForceFeedbackDebug]);
        Binds::Action("Traffic debug display (ToggleTrafficDebugDisplay)", "cheat_trafficdbg");
        ImGui::EndTabItem(); }

        if (ImGui::BeginTabItem("Radio and map")) {
        Binds::Action("Toggle radio (ToggleRadio)", "cheat_radio");
        ImGui::SameLine();
        Binds::Action("Previous station", "cheat_radio_prev");
        ImGui::SameLine();
        Binds::Action("Next station (ChangeRadioStation)", "cheat_radio_next");
        Binds::Action("Toggle map (ToggleMap)", "cheat_map");
        ImGui::SameLine();
        Binds::Action("Toggle big map (ToggleBigMap)", "cheat_bigmap");
        ImGui::EndTabItem(); }
        ImGui::EndTabBar();

        if (BuiltinCheats::message[0]) ImGui::TextDisabled("%s", BuiltinCheats::message);
    }

    // ---- everything that can be bound / saved -----------------------------------------
    namespace
    {
        void ActGaragesUnlock() { BuiltinCheats::UnlockAllGarages(); }
        void ActCachesUnlock() { if (MapTools::Count() == 0) MapTools::Scan(); MapTools::UnlockCaches(); }
        void ActGfxAO() { GameConsole::Run("TOGGLEAO"); }
        void ActGfxSoftShadows() { GameConsole::Run("TOGGLEBPCF"); }
        void ActGfxFog() { GameConsole::Run("SHOW FOG"); }
        void ActGfxPost() { GameConsole::Run("SHOW POSTPROCESS"); }
        void ActMissionEnable() { BuiltinCheats::Request(BuiltinCheats::OpEnableMissions); }
        void ActMissionFail() { BuiltinCheats::Request(BuiltinCheats::OpFailMission); }
        void ActMissionPause() { BuiltinCheats::Request(BuiltinCheats::OpPauseMission); }
        void ActMissionResume() { BuiltinCheats::Request(BuiltinCheats::OpResumeMission); }
        void ActMissionEnd() { BuiltinCheats::Request(BuiltinCheats::OpEndMission); }
        void ActMissionRetry() { BuiltinCheats::Request(BuiltinCheats::OpRetryMission); }
        void ActCheatResurrect() { BuiltinCheats::Request(BuiltinCheats::OpResurrect); }
        void ActRadio() { BuiltinCheats::Request(BuiltinCheats::OpToggleRadio); }
        void ActRadioNext() { BuiltinCheats::Request(BuiltinCheats::OpNextStation); }
        void ActRadioPrev() { BuiltinCheats::Request(BuiltinCheats::OpPrevStation); }
        void ActMap() { BuiltinCheats::Request(BuiltinCheats::OpToggleMap); }
        void ActBigMap() { BuiltinCheats::Request(BuiltinCheats::OpToggleBigMap); }
        void ActTrafficDbg() { BuiltinCheats::Request(BuiltinCheats::OpTrafficDebug); }
        void ActCheatKillMe() { BuiltinCheats::RequestKillMe(); }
        void ActCheatKillVeh() { BuiltinCheats::RequestKillVehicle(); }
        void ActCheatFocus() { BuiltinCheats::RequestFocusGauge(); }
        void ActCheatUpgrade() { BuiltinCheats::RequestUpgrades(); }
        void ActRepair() { VehicleMod::RequestRepair(); }
        void ActExit() { VehicleMod::RequestExit(); }
        void ActDeleteVehicle() { VehicleMod::RequestDelete(); }
        void ActPoliceSet() { Police::SetLevel(Police::lockValue); }
        void ActPoliceLose() { Police::LoseNow(); }
        void ActNpcDisarm() { NpcMod::DisarmNow(); }
        void ActNpcHeal() { NpcMod::HealAlliesNow(); }
    }

    void InitFeatures()
    {
        using namespace Binds;
        // ---- Player
        RegisterToggle("player.invincible", "Player: invincible", "Player", &g_godInvincible);
        RegisterToggle("player.healthlock", "Player: keep health at least", "Player", &g_godHealthLock);
        RegisterToggle("player.noreload", "Weapon: no reload", "Player", &g_patches.noReload);
        RegisterToggle("player.nospread", "Weapon: no spread", "Player", &WeaponMod::noSpread);
        RegisterToggle("player.norecoil", "Weapon: no recoil", "Player", &WeaponMod::noRecoil);
        RegisterToggle("player.noclip", "Player: noclip", "Player", &g_noclip);
        // ---- Vehicle
        RegisterToggle("veh.immortal", "Vehicle: immortality", "Vehicle", &g_patches.vehicleGodMode);
        RegisterToggle("veh.nocolldmg", "Vehicle: no collision damage", "Vehicle", &VehicleMod::noCollisionDamage);
        RegisterToggle("veh.nodeform", "Vehicle: no deformation", "Vehicle", &VehicleMod::noDeformation);
        RegisterToggle("veh.bulletproof", "Vehicle: bullet proof", "Vehicle", &VehicleMod::bulletproof);
        RegisterToggle("veh.tyres", "Vehicle: tyres can't be destroyed", "Vehicle", &VehicleMod::invulnerableTyres);
        RegisterToggle("veh.nobikefall", "Vehicle: can't fall off a motorcycle", "Vehicle", &VehicleMod::noBikeFall);
        RegisterToggle("veh.boost", "Vehicle: infinite boost", "Vehicle", &g_patches.infiniteMana);
        RegisterToggle("veh.ramboom", "Vehicle: Ram Boom", "Vehicle", &g_patches.ramBoom);
        RegisterToggle("veh.jackany", "Vehicle: jack any vehicle", "Vehicle", &VehicleMod::jackAny);
        RegisterToggle("veh.jackocc", "Vehicle: jack ignores passengers", "Vehicle", &VehicleMod::jackIgnoreOccupants);
        RegisterToggle("veh.jackfar", "Vehicle: jump between cars, relaxed conditions", "Vehicle", &VehicleMod::jackAnywhere);
        RegisterToggle("veh.horn", "Vehicle: horn", "Vehicle", &VehicleMod::hornOn);
        RegisterToggle("veh.siren", "Vehicle: siren", "Vehicle", &VehicleMod::sirenOn);
        RegisterToggle("veh.sirenlights", "Vehicle: siren lights", "Vehicle", &VehicleMod::sirenLightsOn);
        RegisterToggle("spawn.autoremove", "Spawn: auto-remove spawned vehicles", "Vehicle", &Spawner::autoRemove);
        RegisterToggle("spawn.keepnear", "Spawn: never auto-remove near vehicles", "Vehicle", &Spawner::keepNear);
        RegisterToggle("spawn.special", "Spawn: list special rigs", "Vehicle", &g_catShowSpecial);
        RegisterAction("spawn_selected", "Vehicle: spawn the selected catalog vehicle", "Vehicle", &ActSpawnSelected);
        RegisterToggle("cheat.ammo", "Cheat: infinite ammo", "Cheats", &BuiltinCheats::ammoGod);
        RegisterAction("garages_unlock", "World: unlock all garages", "World", &ActGaragesUnlock);
        RegisterAction("caches_unlock", "World: unlock all weapon caches", "World", &ActCachesUnlock);
        RegisterToggle("world.cachesopen", "World: caches always available", "World", &MapTools::cachesAlwaysOpen);
        RegisterToggle("world.cachesmap", "World: show caches on the game's map", "World", &MapTools::showCachesOnMap);
        RegisterToggle("gps.showroute", "GPS: show the route", "Map", &Nav::showRoute);
        RegisterFloat("gps.arrive", &Nav::arriveMeters);
        RegisterToggle("veh.topspeedlock", "Vehicle: lock the top speed", "Vehicle", &VehicleMod::topSpeedLock);
        RegisterFloat("veh.topspeed", &VehicleMod::topSpeedMs);
        RegisterToggle("mission.antifail", "Mission: anti-fail", "Mission", &BuiltinCheats::antiFailMission);
        RegisterToggle("guard.enabled", "Loading protection: stay quiet while loading", "Settings", &LoadGuard::enabled);
        RegisterToggle("cam.direct", "Camera: direct mouse camera", "Player", &MouseLook::enabled);
        RegisterFloat("cam.sens", &MouseLook::sensitivity);
        RegisterFloat("cam.aimscale", &MouseLook::aimScale);
        RegisterToggle("cam.inverty", "Camera: invert the vertical axis", "Player", &MouseLook::invertY);
        RegisterToggle("post.enabled","Graphics: post-processing (FXAA, sharpening, colour)", "Visuals", &PostFx::enabled);
        RegisterFloat("post.fxaa", &PostFx::fxaa);
        RegisterFloat("post.sharpen", &PostFx::sharpen);
        RegisterFloat("post.saturation", &PostFx::saturation);
        RegisterFloat("post.contrast", &PostFx::contrast);
        RegisterFloat("post.gamma", &PostFx::gamma);
        RegisterFloat("post.aostrength", &PostFx::aoStrength);
        RegisterFloat("post.aoradius", &PostFx::aoRadius);
        RegisterFloat("post.aobias", &PostFx::aoBias);
        RegisterFloat("post.aomax", &PostFx::aoMaxDistance);
        RegisterFloat("post.fogstrength", &PostFx::fogStrength);
        RegisterFloat("post.fogstart", &PostFx::fogStart);
        RegisterFloat("post.fogdistance", &PostFx::fogDistance);
        RegisterFloat("post.fogr", &PostFx::fogR);
        RegisterFloat("post.fogg", &PostFx::fogG);
        RegisterFloat("post.fogb", &PostFx::fogB);
        RegisterFloat("post.dofamount", &PostFx::dofAmount);
        RegisterFloat("post.doffocus", &PostFx::dofFocus);
        RegisterFloat("post.dofzone", &PostFx::dofZone);
        RegisterFloat("post.doftransition", &PostFx::dofTransition);
        RegisterToggle("post.dofauto", "Graphics: depth of field follows the screen centre", "Visuals", &PostFx::dofAuto);
        RegisterInt("graphics.aniso", &GfxBoost::anisotropy);
        RegisterFloat("graphics.lodbias", &GfxBoost::lodBias);
        RegisterInt("graphics.skymode", &g_skyMode);
        RegisterFloat("graphics.shadowbias", &g_shadowBiasScale);
        RegisterFloat("graphics.shadowslope", &g_shadowSlopeScale);
        RegisterInt("guard.slowgrace", &LoadGuard::slowGraceSeconds);
        RegisterInt("guard.settle2", &LoadGuard::settleSeconds);   // renamed: the old default (20 s) was a fixed delay
        RegisterAction("gfx_ao", "Graphics: ambient occlusion on/off", "Visuals", &ActGfxAO);
        RegisterAction("gfx_softshadows", "Graphics: soft shadow filter on/off", "Visuals", &ActGfxSoftShadows);
        RegisterAction("gfx_fog", "Graphics: fog on/off", "Visuals", &ActGfxFog);
        RegisterAction("gfx_post", "Graphics: post-processing on/off", "Visuals", &ActGfxPost);
        RegisterAction("mission_enable","Mission: enable missions", "Mission", &ActMissionEnable);
        RegisterAction("mission_fail", "Mission: fail", "Mission", &ActMissionFail);
        RegisterAction("mission_end", "Mission: complete", "Mission", &ActMissionEnd);
        RegisterAction("mission_retry", "Mission: retry", "Mission", &ActMissionRetry);
        RegisterAction("mission_pause", "Mission: pause", "Mission", &ActMissionPause);
        RegisterAction("mission_resume", "Mission: resume", "Mission", &ActMissionResume);
        RegisterToggle("minimap.enabled", "Minimap: show", "Map", &MiniMap::enabled);
        RegisterToggle("minimap.rotate", "Minimap: turns with me", "Map", &MiniMap::rotate);
        for (int c = 0; c < MiniMap::kCategoryCount; ++c)
        {
            // ids / labels must outlive the registry
            static char ids[MiniMap::kCategoryCount][48], labels[MiniMap::kCategoryCount][80], sids[MiniMap::kCategoryCount][48];
            snprintf(ids[c], sizeof(ids[c]), "minimap.cat.%s", MiniMap::CategoryKey(c));
            snprintf(labels[c], sizeof(labels[c]), "Minimap: %s", MiniMap::CategoryLabel(c));
            RegisterToggle(ids[c], labels[c], "Map", &MiniMap::catOn[c]);
            snprintf(sids[c], sizeof(sids[c]), "minimap.size.%s", MiniMap::CategoryKey(c));
            RegisterFloat(sids[c], &MiniMap::catScale[c]);
        }
        RegisterToggle("minimap.gamevisible", "Minimap: only markers the game shows", "Map", &MiniMap::onlyGameVisible);
        RegisterFloat("minimap.iconscale", &MiniMap::iconScale);
        RegisterToggle("minimap.policezone", "Minimap: police influence zone", "Map", &MiniMap::showPoliceZone);
        RegisterFloat("minimap.zonescale", &MiniMap::zoneScale);
        RegisterToggle("minimap.bg", "Minimap: city map picture", "Map", &MiniMap::background);
        RegisterFloat("minimap.bgopacity", &MiniMap::bgOpacity);
        RegisterFloat("minimap.originx", &MiniMap::mapOriginX);
        RegisterFloat("minimap.originy", &MiniMap::mapOriginY);
        RegisterFloat("minimap.cmpx", &MiniMap::mapCmPerPx);
        RegisterToggle("minimap.match", "Minimap: match the game's minimap", "Map", &MiniMap::matchGame);
        RegisterToggle("minimap.matchscale", "Minimap: synchronise the zoom", "Map", &MiniMap::matchScale);
        RegisterToggle("minimap.matchrot", "Minimap: synchronise the rotation", "Map", &MiniMap::matchRotation);
        RegisterToggle("minimap.square", "Minimap: square", "Map", &MiniMap::square);
        RegisterFloat("minimap.matchzoom", &MiniMap::matchZoom);
        RegisterFloat("minimap.matchyaw", &MiniMap::matchYawOffsetDeg);
        RegisterToggle("minimap.matchinvert", "Minimap: invert the matched rotation", "Map", &MiniMap::matchYawInvert);
        RegisterFloat("map.hudzoom", &MapTools::hudZoom);
        RegisterToggle("npc.blindall", "Stealth: nobody can see me", "Combat", &NpcMod::blindAll);
        RegisterToggle("police.disarm", "Police: keep disarmed", "Police", &NpcMod::policeDisarmed);
        RegisterToggle("police.ignore", "Police: ignore me (blind, no target)", "Police", &NpcMod::policeIgnore);
        RegisterToggle("map.garages", "Map: keep garages on the map", "Map", &MapTools::showGaragesOnMap);
        RegisterToggle("map.all", "Map: keep every icon marker on the map", "Map", &MapTools::showAllOnMap);
        RegisterToggle("police.crimes", "Police: use the edited crime values", "Police", &CrimeTools::overrideOn);
        RegisterToggle("mission.debug", "Mission: debug info", "Mission", &BuiltinCheats::flags[BuiltinCheats::FlagMissionDebug]);
        RegisterToggle("world.garagesopen", "World: garages always open", "World", &BuiltinCheats::garagesAlwaysOpen);
        RegisterAction("cheat_resurrect", "Cheat: resurrect me", "Cheats", &ActCheatResurrect);
        RegisterToggle("cheat.ctrlgod", "Cheat: controller God flag", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagControllerGod]);
        RegisterToggle("cheat.fakegod", "Cheat: fake god mode", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagFakeGod]);
        RegisterToggle("dbg.carjack", "Debug: carjack", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagCarjackDebug]);
        RegisterToggle("dbg.damage", "Debug: damage", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagDamageDebug]);
        RegisterToggle("dbg.melee", "Debug: melee boost", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagMeleeBoostDebug]);
        RegisterToggle("dbg.locdamage", "Debug: locational damage", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagLocDamageDebug]);
        RegisterToggle("dbg.impulse", "Debug: collision impulse", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagImpulseDebug]);
        RegisterToggle("dbg.vehhealth", "Debug: my vehicle health", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagVehicleHealth]);
        RegisterToggle("dbg.vehdamage", "Debug: my vehicle damage", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagVehicleDamage]);
        RegisterToggle("dbg.vehdetail", "Debug: detailed vehicle damage", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagVehicleDamageDetail]);
        RegisterToggle("dbg.ff", "Debug: force feedback", "Cheats", &BuiltinCheats::flags[BuiltinCheats::FlagForceFeedbackDebug]);
        RegisterAction("cheat_trafficdbg", "Debug: traffic display", "Cheats", &ActTrafficDbg);
        RegisterAction("cheat_radio", "Radio: toggle", "Cheats", &ActRadio);
        RegisterAction("cheat_radio_next", "Radio: next station", "Cheats", &ActRadioNext);
        RegisterAction("cheat_radio_prev", "Radio: previous station", "Cheats", &ActRadioPrev);
        RegisterAction("cheat_map", "Map: toggle", "Cheats", &ActMap);
        RegisterAction("cheat_bigmap", "Map: toggle big map", "Cheats", &ActBigMap);
        RegisterAction("cheat_focus", "Cheat: fill the focus gauge", "Cheats", &ActCheatFocus);
        RegisterAction("cheat_killme", "Cheat: kill me", "Cheats", &ActCheatKillMe);
        RegisterAction("cheat_killveh", "Cheat: destroy my vehicle", "Cheats", &ActCheatKillVeh);
        RegisterAction("cheat_upgrade", "Cheat: apply vehicle upgrade levels", "Cheats", &ActCheatUpgrade);
        RegisterAction("repair", "Vehicle: repair", "Vehicle", &ActRepair);
        RegisterAction("exit_vehicle", "Vehicle: emergency exit", "Vehicle", &ActExit);
        RegisterAction("delete_vehicle", "Vehicle: delete my vehicle", "Vehicle", &ActDeleteVehicle);
        // ---- World
        RegisterToggle("world.neverwanted", "Police: ignore me (never wanted)", "World", &Police::neverWanted);
        RegisterToggle("world.peaceful", "Police: never shoot or arrest me", "World", &Police::peaceful);
        RegisterToggle("world.keeplevel", "Police: keep the level locked", "World", &Police::lockLevel);
        RegisterAction("police_set", "Police: set the chosen level", "World", &ActPoliceSet);
        RegisterAction("police_lose", "Police: lose them now", "World", &ActPoliceLose);
        RegisterToggle("world.freezetimer", "Mission: freeze timers", "World", &g_patches.freezeTimer);
        RegisterToggle("world.fpslimit", "Performance: limit FPS", "World", &Perf::limitEnabled);
        // ---- Combat
        RegisterToggle("aim.enabled", "Aimbot", "Combat", &Aimbot::enabled);
        RegisterToggle("aim.experimental", "Aimbot: camera aiming (no cursor)", "Combat", &Aimbot::experimental);
        RegisterToggle("aim.onlyenemies", "Aimbot: only enemies", "Combat", &Aimbot::onlyEnemies);
        RegisterToggle("aim.head", "Aimbot: aim at the head bone", "Combat", &Aimbot::useBoneHead);
        RegisterToggle("aim.fov", "Aimbot: draw FOV circle", "Combat", &Aimbot::drawFov);
        RegisterToggle("aim.autoshot", "Aimbot: AutoShot", "Combat", &Aimbot::autoShot);
        RegisterToggle("npc.disarm", "Enemies: keep disarmed", "Combat", &NpcMod::keepDisarmed);
        RegisterToggle("npc.ignore", "Enemies: ignore me", "Combat", &NpcMod::enemiesIgnore);
        RegisterToggle("npc.vulnerable", "Enemies: make invincible ones vulnerable", "Combat", &NpcMod::enemiesVulnerable);
        RegisterToggle("npc.allyinv", "Allies: invincible", "Combat", &NpcMod::alliesInvincible);
        RegisterToggle("npc.allyheal", "Allies: keep at full health", "Combat", &NpcMod::alliesKeepHealth);
        RegisterAction("npc_disarm", "Enemies: disarm now", "Combat", &ActNpcDisarm);
        RegisterAction("npc_heal", "Allies: heal now", "Combat", &ActNpcHeal);
        // ---- Visuals
        RegisterToggle("esp.vehicles", "ESP: vehicles", "Visuals", &espEnabled);
        RegisterToggle("esp.health", "ESP: vehicle health", "Visuals", &espShowHealth);
        RegisterToggle("esp.speed", "ESP: vehicle speed", "Visuals", &espShowSpeed);
        RegisterToggle("esp.distance", "ESP: vehicle distance", "Visuals", &espShowDistance);
        RegisterToggle("esp.category", "ESP: vehicle class", "Visuals", &espShowCategory);
        RegisterToggle("esp.self", "ESP: mark my own vehicle", "Visuals", &espShowSelf);
        RegisterToggle("esp.cars", "ESP: cars", "Visuals", &espFilterCars);
        RegisterToggle("esp.motos", "ESP: motorcycles", "Visuals", &espFilterMotos);
        RegisterToggle("esp.trucks", "ESP: trucks", "Visuals", &espFilterTrucks);
        RegisterToggle("npcesp.enabled", "ESP: NPCs", "Visuals", &npcEspEnabled);
        RegisterToggle("npcesp.enemies", "ESP: only enemies", "Visuals", &npcOnlyEnemies);
        RegisterToggle("npcesp.showenemies", "ESP: show enemies", "Visuals", &npcShowEnemies);
        RegisterToggle("npcesp.allies", "ESP: show allies", "Visuals", &npcShowAllies);
        RegisterToggle("npcesp.vip", "ESP: show VIP", "Visuals", &npcShowVip);
        RegisterToggle("npcesp.police", "ESP: show police", "Visuals", &npcShowPolice);
        RegisterToggle("npcesp.civilians", "ESP: show city NPCs", "Visuals", &npcShowCivilians);
        RegisterToggle("npcesp.dead", "ESP: show dead", "Visuals", &npcShowDead);
        RegisterToggle("npcesp.box", "ESP: NPC box", "Visuals", &npcShowBox);
        RegisterToggle("npcesp.bonebox", "ESP: NPC box from bones", "Visuals", &npcBoneBox);
        RegisterToggle("npcesp.skeleton", "ESP: NPC skeleton", "Visuals", &npcShowSkeleton);
        RegisterToggle("npcesp.head", "ESP: NPC head marker", "Visuals", &npcShowHead);
        RegisterToggle("npcesp.health", "ESP: NPC health", "Visuals", &npcShowHealth);
        RegisterToggle("npcesp.distance", "ESP: NPC distance", "Visuals", &npcShowDistance);
        RegisterToggle("npcesp.type", "ESP: NPC type id", "Visuals", &npcShowType);
        RegisterToggle("hud.speedometer", "HUD: speedometer", "Visuals", &speedometerVisible);
        RegisterToggle("hud.binds", "HUD: active binds window", "Visuals", &Binds::showWindow);

        // ---- values saved in configs
        RegisterFloat("player.noclipspeed", &g_noclipSpeed);
        RegisterInt("player.lockhealth", &g_lockHealth);
        RegisterInt("world.wantedlevel", &Police::lockValue);
        RegisterInt("world.targetfps", &Perf::targetFps);
        RegisterInt("aim.key", &Aimbot::aimKey);
        RegisterFloat("aim.fovpx", &Aimbot::fovPixels);
        RegisterFloat("aim.smoothing", &Aimbot::smoothing);
        RegisterFloat("aim.maxdist", &Aimbot::maxDistance);
        RegisterFloat("aim.headheight", &Aimbot::headHeight);
        RegisterFloat("aim.offx", &Aimbot::offsetX);
        RegisterFloat("aim.offy", &Aimbot::offsetY);
        RegisterFloat("aim.shotradius", &Aimbot::shotRadius);
        RegisterInt("aim.shotinterval", &Aimbot::shotIntervalMs);
        RegisterFloat("cheat.timescale", &BuiltinCheats::timeScale);
        RegisterInt("cheat.fakegodmin", &BuiltinCheats::fakeGodMinHealth);
        RegisterFloat("minimap.range", &MiniMap::rangeMeters);
        RegisterFloat("minimap.opacity", &MiniMap::opacity);
        RegisterInt("minimap.size", &MiniMap::sizePx);
        RegisterInt("minimap.rescan", &MiniMap::autoScanSeconds);   // renamed: an old saved value of 60 s made new mission markers show up late
        RegisterFloat("police.heatscale", &CrimeTools::changeScale);
        RegisterFloat("police.runaround.time", &CrimeTools::runAroundTime);
        RegisterFloat("police.runaround.dist", &CrimeTools::runAroundDistance);
        RegisterFloat("police.runaround.reset", &CrimeTools::runAroundResetTime);
        for (int i = 0; i < CrimeTools::kCrimes; ++i)
        {
            char id[48];
            snprintf(id, sizeof(id), "police.crime.%d.max", i); RegisterInt(id, &CrimeTools::edited[i].maxLevel);
            snprintf(id, sizeof(id), "police.crime.%d.serious", i); RegisterInt(id, &CrimeTools::edited[i].serious);
            snprintf(id, sizeof(id), "police.crime.%d.heat", i); RegisterFloat(id, &CrimeTools::edited[i].change);
        }
        RegisterInt("cheat.perflevel", &BuiltinCheats::perfLevel);
        RegisterInt("cheat.meleelevel", &BuiltinCheats::meleeLevel);
        RegisterInt("cheat.healthlevel", &BuiltinCheats::healthLevel);
        RegisterInt("spawn.pick", &g_catPick);
        RegisterInt("spawn.maxvehicles", &Spawner::maxSpawned);
        RegisterFloat("spawn.removedist", &Spawner::autoRemoveMeters);
        RegisterFloat("spawn.neardist", &Spawner::keepNearMeters);
        RegisterFloat("esp.maxdist", &espMaxDistanceMeters);
        RegisterFloat("npcesp.maxdist", &npcMaxDistanceMeters);

        Binds::Init();
    }

    void Draw()
    {
        Lang::stripTechnical = !debugEnabled;   // technical remarks "(+0x2AC)", "(m_bIsEnemy)" ... are shown only with the Debug switch
        ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(720, 420), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::Begin("WHEELMAN MOD");
        {
            // The window remembers its size between runs (imgui.ini); the old narrow layout was saved small. Once per session the
            // window is widened to the size the new layout is designed for, after that the user's own resizing is respected.
            static bool firstFrame = true;
            if (firstFrame)
            {
                firstFrame = false;
                const ImVec2 s = ImGui::GetWindowSize();
                if (s.x < 900.0f || s.y < 600.0f) ImGui::SetWindowSize(ImVec2(s.x < 1000.0f ? 1000.0f : s.x, s.y < 700.0f ? 700.0f : s.y));
            }
        }

        // Layout: a category list on the left, the pages of the chosen category on the right (sub-pages as tabs). Every page is
        // built from labelled sections; long texts wrap at the window edge instead of being cut off.
        struct Category { const char* name; void (*draw)(); };
        static const Category cats[] = {
            { "PLAYER", &DrawPlayerTab }, { "VEHICLE", &DrawVehicleTab }, { "COMBAT", &DrawCombatTab }, { "WORLD", &DrawWorldTab },
            { "Map", &DrawMapTab }, { "VISUALS", &DrawVisualsTab }, { "Built-in cheats", &DrawCheatsTab },
            { "SETTINGS", &DrawSettingsTab }, { "DEBUG", &DrawDebugTab } };
        const int count = debugEnabled ? 9 : 8;
        static int selected = 0;
        if (selected >= count) selected = 0;

        const float navWidth = 190.0f;
        ImGui::BeginChild("##nav", ImVec2(navWidth, 0.0f), true);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
            for (int i = 0; i < count; ++i)
            {
                if (ImGui::Selectable(cats[i].name, selected == i, 0, ImVec2(0.0f, 30.0f))) selected = i;
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), false);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushItemWidth(-340.0f);
        cats[selected].draw();
        ImGui::PopItemWidth();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();

        ImGui::End();
    }
}

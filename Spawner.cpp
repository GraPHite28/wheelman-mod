#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include "Spawner.h"
#include "Lang.h"
#include "Patches.h"
#include "Overlay.h"
#include "Log.h"
#include "CrashHandler.h"

// Set by Detour_Factory (Patches.cpp) every time the game calls the factory spawn method.
extern "C" volatile uintptr_t g_spawnFactory = 0;

namespace Spawner
{
    int status = 0;
    bool allowStubs = false;
    uintptr_t lastSpawned = 0;
    const char* lastMessage = "";

    namespace
    {
        constexpr uintptr_t kFactorySpawn = 0x00498340;

        volatile long g_pending = 0;
        uintptr_t g_class = 0, g_def = 0;
        float g_loc[3] = {};
        int32_t g_rot[3] = {};
        ULONGLONG g_lastSpawn = 0;

        template <class T> bool Rd(uintptr_t a, T& out)
        {
            __try { out = *reinterpret_cast<const T*>(a); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        template <class T> bool Wr(uintptr_t a, T v)
        {
            __try { *reinterpret_cast<T*>(a) = v; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // Factory::Spawn(FVector* loc, FRotator* rot, void* other) - thiscall, callee pops 12 bytes.
        void* CallFactorySpawn(void* factory, const float* loc, const int32_t* rot)
        {
            void* result = nullptr;
            __asm
            {
                mov ecx, factory
                push 0
                push rot
                push loc
                mov edx, 0x00498340
                call edx
                mov result, eax
            }
            return result;
        }

        bool ValidObject(uintptr_t t)
        {
            uint32_t vt = 0;
            return t > 0x10000 && Rd(t, vt) && vt >= 0x1000000 && vt <= 0x1800000;   // UObject with a vtable in the exe
        }

        // Picks the next spawn spot in front of the player into g_loc / g_rot.
        bool ComputeSpawnSpot()
        {
            float px = 0, py = 0, pz = 0;
            if (!Overlay::GetPlayerPosition(px, py, pz)) return false;
            uintptr_t pawn = Overlay::GetPlayerPawn();
            int32_t yaw = 0;
            if (pawn) Rd(pawn + 0xE4, yaw);
            const float rad = static_cast<float>(yaw) * (6.2831853f / 65536.f);
            // Every spawn gets its own spot: two cars spawned one on top of the other (the crash reports show repeated
            // spawns 2-3 s apart at the same place, each followed by a crash within seconds) end up interpenetrating.
            static int spawnIndex = 0;
            const float fwd = 900.f + 700.f * static_cast<float>(spawnIndex % 5);
            const float side = 400.f * static_cast<float>((spawnIndex / 5) % 3 - 1);
            ++spawnIndex;
            g_loc[0] = px + std::cos(rad) * fwd + std::sin(rad) * side;
            g_loc[1] = py + std::sin(rad) * fwd - std::cos(rad) * side;
            g_loc[2] = pz + 150.f;
            g_rot[0] = 0; g_rot[1] = yaw; g_rot[2] = 0;
            return true;
        }
    }

    void DoSpawn();

    uintptr_t Factory() { return g_spawnFactory; }

    namespace
    {
        constexpr int kMaxSeen = 32;
        SeenModel g_seen[kMaxSeen];
        int g_seenCount = 0;
        bool g_inOwnSpawn = false;
    }

    int SeenCount() { return g_seenCount; }
    bool SeenAt(int i, SeenModel& out)
    {
        if (i < 0 || i >= g_seenCount) return false;
        out = g_seen[i];
        return true;
    }

    void OnFactorySpawn(uintptr_t factory)
    {
        if (g_inOwnSpawn) return;
        uint32_t cls = 0, def = 0;
        if (!Rd(factory + 0x54, cls) || !Rd(factory + 0x5C, def) || !ValidObject(cls) || !ValidObject(def)) return;
        for (int i = 0; i < g_seenCount; ++i)
            if (g_seen[i].cls == cls && g_seen[i].def == def) { ++g_seen[i].count; return; }
        if (g_seenCount < kMaxSeen) g_seen[g_seenCount++] = { cls, def, 1 };
    }

    namespace
    {
        constexpr uintptr_t kDefVtable = 0x0132D2B0;
        constexpr int kMaxDefs = 128;
        DefInfo g_defs[kMaxDefs];
        int g_defCount = 0;
        uintptr_t g_vehClass = 0;

        // A definition that was unloaded (mission-only cars go away with their mission) is freed memory: the allocator
        // overwrites the start of a freed block, so a live definition must still have the definition vtable, the UObject
        // marker and body/model objects that still have a vtable. Not bullet-proof, but it catches the usual case.
        bool DefLooksLive(uintptr_t def)
        {
            uint32_t vt = 0, body = 0, mid = 0;
            int32_t mark = 0;
            return def > 0x10000 && Rd(def, vt) && vt == kDefVtable && Rd(def + 0x18, mark) && mark == -1 &&
                   Rd(def + 0x34, body) && Rd(def + 0x38, mid) && ValidObject(body) && ValidObject(mid);
        }
    }

    namespace
    {
        struct Label { uint32_t type, family; char text[48]; float px, py; bool hasPos; };
        constexpr int kMaxLabels = 256;
        Label g_labels[kMaxLabels];
        int g_labelCount = 0;
        bool g_labelsLoaded = false;

        const char* LabelPath()
        {
            static char path[MAX_PATH] = {};
            if (!path[0])
            {
                HMODULE hm = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&LabelPath), &hm);
                GetModuleFileNameA(hm, path, MAX_PATH);
                char* slash = strrchr(path, '\\');
                if (slash) slash[1] = 0; else path[0] = 0;
                strcat_s(path, "WheelmanMod_vehicle_labels.txt");
            }
            return path;
        }

        void LoadLabels()
        {
            if (g_labelsLoaded) return;
            g_labelsLoaded = true;
            FILE* f = nullptr;
            fopen_s(&f, LabelPath(), "r");
            if (!f) return;
            char line[128];
            while (fgets(line, sizeof(line), f) && g_labelCount < kMaxLabels)
            {
                unsigned t = 0, fam = 0; int n = 0;
                if (sscanf_s(line, "%x %x %n", &t, &fam, &n) < 2 || n <= 0) continue;
                Label& l = g_labels[g_labelCount++];
                l.type = t; l.family = fam;
                strncpy_s(l.text, line + n, _TRUNCATE);
                for (char* c = l.text; *c; ++c) if (*c == '\n' || *c == '\r') *c = 0;
                // optional "|x,y": where the player stood when the model was named (a hint where to find it again)
                l.hasPos = false;
                if (char* bar = strrchr(l.text, '|'))
                {
                    float x = 0, y = 0;
                    if (sscanf_s(bar + 1, "%f,%f", &x, &y) == 2) { l.px = x; l.py = y; l.hasPos = true; }
                    *bar = 0;
                }
            }
            fclose(f);
        }
    }

    const char* LabelFor(uint32_t nameIdx, uint32_t family)
    {
        LoadLabels();
        for (int i = 0; i < g_labelCount; ++i)
            if (g_labels[i].type == nameIdx && g_labels[i].family == family) return g_labels[i].text;
        return "";
    }

    int LabelCount() { LoadLabels(); return g_labelCount; }   // slots, deleted ones are skipped by LabelAt

    bool LabelAt(int i, uint32_t& nameIdx, uint32_t& family, const char*& text)
    {
        LoadLabels();
        if (i < 0 || i >= g_labelCount || !g_labels[i].text[0]) return false;
        nameIdx = g_labels[i].type; family = g_labels[i].family; text = g_labels[i].text;
        return true;
    }

    void SetLabel(uint32_t nameIdx, uint32_t family, const char* text)
    {
        LoadLabels();
        int at = -1;
        for (int i = 0; i < g_labelCount; ++i)
            if (g_labels[i].type == nameIdx && g_labels[i].family == family) { at = i; break; }
        if (at < 0)
        {
            if (g_labelCount >= kMaxLabels) return;
            at = g_labelCount++;
            g_labels[at].type = nameIdx; g_labels[at].family = family;
        }
        strncpy_s(g_labels[at].text, text, _TRUNCATE);
        for (char* c = g_labels[at].text; *c; ++c) if (*c == '|') *c = '/';
        float px = 0, py = 0, pz = 0;
        if (Overlay::GetPlayerPosition(px, py, pz)) { g_labels[at].px = px; g_labels[at].py = py; g_labels[at].hasPos = true; }
        FILE* f = nullptr;
        fopen_s(&f, LabelPath(), "w");
        if (!f) return;
        for (int i = 0; i < g_labelCount; ++i)
        {
            const Label& l = g_labels[i];
            if (!l.text[0]) continue;
            if (l.hasPos) fprintf(f, "%X %X %s|%.0f,%.0f\n", l.type, l.family, l.text, l.px, l.py);
            else          fprintf(f, "%X %X %s\n", l.type, l.family, l.text);
        }
        fclose(f);
    }

    bool LabelPos(uint32_t nameIdx, uint32_t family, float& x, float& y)
    {
        LoadLabels();
        for (int i = 0; i < g_labelCount; ++i)
            if (g_labels[i].type == nameIdx && g_labels[i].family == family && g_labels[i].hasPos) { x = g_labels[i].px; y = g_labels[i].py; return true; }
        return false;
    }

    int DefCount() { return g_defCount; }
    bool DefAt(int i, DefInfo& out)
    {
        if (i < 0 || i >= g_defCount) return false;
        out = g_defs[i];
        return true;
    }

    uintptr_t VehicleClass()
    {
        if (g_vehClass) return g_vehClass;
        if (g_seenCount) return g_seen[0].cls;
        for (int i = 0; i < g_vehicleListCount; ++i)
        {
            uint32_t c = 0;
            if (IsLiveVehicle(g_vehicleList[i].ptr) && Rd(reinterpret_cast<uintptr_t>(g_vehicleList[i].ptr) + 0x28, c) && ValidObject(c))
                return g_vehClass = c;
        }
        return 0;
    }

    int ScanDefs()
    {
        g_defCount = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        while (addr < maxAddr && g_defCount < kMaxDefs)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x100)
            {
                __try
                {
                    for (uintptr_t p = start; p + 0x60 <= end && g_defCount < kMaxDefs; p += 4)
                    {
                        if (*reinterpret_cast<const uint32_t*>(p) != kDefVtable) continue;
                        if (*reinterpret_cast<const int32_t*>(p + 0x18) != -1) continue;
                        const uint32_t cls = *reinterpret_cast<const uint32_t*>(p + 0x28);
                        const uint32_t arch = *reinterpret_cast<const uint32_t*>(p + 0x2C);
                        if (cls < 0x10000 || arch < 0x10000) continue;   // stale copies of the vtable value have no class/archetype
                        DefInfo d{};
                        d.def = p;
                        d.nameIdx = *reinterpret_cast<const uint32_t*>(p + 0x20);
                        d.outer = *reinterpret_cast<const uint32_t*>(p + 0x1C);
                        d.family = 0;
                        if (d.outer > 0x10000 && d.outer < 0x7FFF0000) Rd(d.outer + 0x20, d.family);
                        const uint32_t bodyObj = *reinterpret_cast<const uint32_t*>(p + 0x34);
                        d.populated = bodyObj != 0;
                        d.body = 0;
                        if (bodyObj > 0x10000 && bodyObj < 0x7FFF0000) Rd(bodyObj + 0x20, d.body);
                        g_defs[g_defCount++] = d;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            addr = end;
        }
        LogF("Spawner: found %d vehicle definitions", g_defCount);
        return g_defCount;
    }

    bool Request(uintptr_t model)
    {
        if (!IsLiveVehicle(reinterpret_cast<void*>(model))) { status = 3; lastMessage = "model vehicle is no longer alive"; return false; }
        uint32_t cls = 0, def = 0;
        if (!Rd(model + 0x28, cls) || !Rd(model + 0xA00, def)) { status = 3; lastMessage = "can't read the model's class/definition"; return false; }
        return RequestPair(cls, def);
    }

    bool RequestPair(uintptr_t cls, uintptr_t def)
    {
        if (g_pending) { lastMessage = "a spawn is already queued"; return false; }
        if (!g_spawnFactory) { status = 3; lastMessage = "factory not captured yet - wait until the game spawns a car by itself"; return false; }
        if (!ValidObject(cls) || !ValidObject(def)) { status = 3; lastMessage = "model has no valid class/definition"; return false; }
        // A definition whose data is not loaded (+0x34 == 0, "body 0x0") builds a half-initialised car (fault inside
        // SpawnActor, crash a few seconds later in the destructor), so it is refused.
        uint32_t loaded = 0;
        Rd(def + 0x34, loaded);
        if (!DefLooksLive(def) && loaded != 0) { status = 3; lastMessage = "definition is no longer loaded (its area/mission was unloaded)"; return false; }
        if (loaded == 0) { status = 3; lastMessage = "definition has no body (body 0x0) - spawning it crashes the game"; return false; }
        if (!ComputeSpawnSpot()) { status = 3; lastMessage = "player position unknown"; return false; }
        g_class = cls;
        g_def = def;
        status = 1; lastMessage = "queued";
        InterlockedExchange(&g_pending, 1);
        return true;
    }

    // ---- catalog: load a vehicle's package on demand, then spawn any template it carries ---------------------------
    // A Transitional_<Model>.xxx package carries the whole model: the WheelmanVehicleTemplate objects (all variants:
    // civilian, police, gang, mission, event ...), mesh tree, physics, effects. Only the packages the current area
    // streams are resident, which is why the spawner used to know just the cars around the player. The game's package
    // loader is called directly: UObject::LoadPackage (0x70DC40 via the wrapper 0x70F3B0, ecx = name) and
    // StaticLoadObject (0x70EF00, cdecl: class, outer, name, filename, loadFlags, sandbox, reconcile) to find the
    // template by its "Group.Name" path.
    namespace
    {
        struct CatEntry { const char* pkg; const char* path; bool special; };
        const CatEntry kCatalog[] = {
#include "VehicleCatalog.inc"
        };
        constexpr int kCatalogCount = sizeof(kCatalog) / sizeof(kCatalog[0]);

        volatile long g_catPending = 0;
        int g_catIndex = -1;
        bool g_catLoadOnly = false;

        uintptr_t CallLoadPackage(const wchar_t* name, uint32_t flags)
        {
            uintptr_t result = 0;
            __asm
            {
                mov ecx, name
                push flags
                mov edx, 0x0070F3B0
                call edx
                add esp, 4
                mov result, eax
            }
            return result;
        }

        uintptr_t CallStaticLoadObject(uintptr_t cls, const wchar_t* path, uint32_t flags)
        {
            uintptr_t result = 0;
            __asm
            {
                push 1              // bAllowObjectReconciliation
                push 0              // Sandbox
                push flags          // LoadFlags
                push 0              // Filename
                push path           // Name ("Group.Object")
                push 0              // Outer
                push cls            // Class
                mov edx, 0x0070EF00
                call edx
                add esp, 0x1C
                mov result, eax
            }
            return result;
        }

        // Class of the WheelmanVehicleTemplate objects: taken from any definition we have already seen.
        uintptr_t TemplateClass()
        {
            for (int i = 0; i < g_seenCount; ++i)
            {
                uint32_t c = 0;
                if (DefLooksLive(g_seen[i].def) && Rd(g_seen[i].def + 0x28, c) && ValidObject(c)) return c;
            }
            for (int i = 0; i < g_vehicleListCount; ++i)
            {
                uint32_t def = 0, c = 0;
                if (IsLiveVehicle(g_vehicleList[i].ptr) && Rd(reinterpret_cast<uintptr_t>(g_vehicleList[i].ptr) + 0xA00, def) &&
                    DefLooksLive(def) && Rd(def + 0x28, c) && ValidObject(c)) return c;
            }
            return 0;
        }

        uintptr_t FindTemplate(uintptr_t cls, const wchar_t* path)
        {
            uintptr_t r = 0;
            __try { r = CallStaticLoadObject(cls, path, 0x2002); }   // LOAD_NoWarn | LOAD_Quiet
            __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
            return r;
        }

        uintptr_t LoadPkg(const wchar_t* name)
        {
            uintptr_t r = 0;
            __try { r = CallLoadPackage(name, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
            return r;
        }
    }

    // Variant overlay: the values a variant template stores (paint, effect / audio libraries, health, flags ...) are
    // written over its base template's fields while the factory spawns, and the base's own values are restored right
    // after (the spawn is synchronous). Words are compared as raw 32-bit values: a zero in the variant means "not set,
    // inherit". Array fields are three words (data, num, max) that are all set together, so they swap as a unit and no
    // buffer is ever shared or freed twice. Bit-field words are OR-ed instead of replaced.
    namespace
    {
        constexpr int kDefFirst = 0x34, kDefEnd = 0x3F0;
        struct Patch { uintptr_t addr; uint32_t orig; };
        Patch g_patch[(kDefEnd - kDefFirst) / 4];
        int g_patchCount = 0;

        // chain[0] = the variant ... chain[n-1] = the stub right above the base.
        void BuildOverlay(uintptr_t base, const uintptr_t* chain, int n)
        {
            uint32_t merged[(kDefEnd - kDefFirst) / 4] = {}, baseVal[(kDefEnd - kDefFirst) / 4] = {};
            for (int i = 0; i < (kDefEnd - kDefFirst) / 4; ++i) { Rd(base + kDefFirst + i * 4, baseVal[i]); merged[i] = baseVal[i]; }
            for (int level = n - 1; level >= 0; --level)          // nearest to the base first, the variant last
            {
                for (int i = 0; i < (kDefEnd - kDefFirst) / 4; ++i)
                {
                    const int off = kDefFirst + i * 4;
                    uint32_t w = 0;
                    if (!Rd(chain[level] + off, w) || w == 0) continue;
                    merged[i] = (off == 0xD8 || off == 0x3D4) ? (merged[i] | w) : w;
                }
            }
            for (int i = 0; i < (kDefEnd - kDefFirst) / 4; ++i)
            {
                if (merged[i] == baseVal[i]) continue;
                const uintptr_t at = base + kDefFirst + i * 4;
                g_patch[g_patchCount++] = { at, baseVal[i] };
                Wr<uint32_t>(at, merged[i]);
            }
            LogF("catalog: overlay changed %d word(s) of the base template", g_patchCount);
        }

        void RestoreOverlay()
        {
            for (int i = 0; i < g_patchCount; ++i) Wr<uint32_t>(g_patch[i].addr, g_patch[i].orig);
            g_patchCount = 0;
        }
    }

    const char* catalogMessage = "";
    uintptr_t catalogDef = 0;

    // ---- short display names ------------------------------------------------------------------------------------
    // "Opel Astra", "Opel Astra (police)", "Limousine (mission 1, broker's)": model + modification, in English and in
    // Russian (the menu language decides which one is shown). Base templates (groups Car / BigCar / Bike / Truck) are
    // the standard models and get no suffix. The model of a variant comes from the package that carries it, its
    // modification from the template group and keywords of its name.
    namespace
    {
        struct Tr3 { const char* key; const char* en; const char* ru; };

        const Tr3 kPkgModel[] = {
            { "BritConv", "British convertible", "Британский кабриолет" }, { "DeliveryVan", "Delivery van", "Фургон доставки" },
            { "Flatbed", "Flatbed truck", "Бортовой грузовик" }, { "FlatCargo", "Flatbed with cargo", "Бортовой грузовик с грузом" },
            { "FraConv", "French convertible", "Французский кабриолет" }, { "FrenchHatch", "French hatchback", "Французский хэтчбек" },
            { "GerConv", "German convertible", "Немецкий кабриолет" }, { "GermanCompact", "German compact", "Немецкий компакт" },
            { "GermanSedan", "German sedan", "Немецкий седан" }, { "Helicopter", "Helicopter", "Вертолёт" },
            { "ItalianCompact", "Italian compact", "Итальянский компакт" }, { "Limo", "Limousine", "Лимузин" },
            { "ModifiedSUV", "Modified SUV", "Модифицированный внедорожник" }, { "Moped", "Moped", "Мопед" },
            { "MovingVan", "Moving van", "Фургон для переезда" }, { "OpelAstra", "Opel Astra", "Opel Astra" },
            { "Pontiac", "Pontiac G8", "Pontiac G8" }, { "Scooter", "Scooter", "Скутер" }, { "Semi", "Semi truck", "Седельный тягач" },
            { "SuperBike", "Superbike", "Спортбайк" }, { "SUV", "SUV", "Внедорожник" }, { "Tanker", "Tanker", "Цистерна" },
            { "Train", "Train", "Поезд" }, { "Transporter", "Transporter", "Транспортёр" },
            { "TransporterCargo", "Transporter with cargo", "Транспортёр с грузом" }, { "TruckCab", "Truck cab", "Кабина грузовика" },
        };

        const Tr3 kBaseName[] = {
            { "britishconvertible", "British convertible", "Британский кабриолет" }, { "deliveryvan", "Delivery van", "Фургон доставки" },
            { "fireworksvan", "Fireworks van", "Фургон с фейерверками" }, { "frenchconvertible", "French convertible", "Французский кабриолет" },
            { "frenchhatch", "French hatchback", "Французский хэтчбек" }, { "germanconvertible", "German convertible", "Немецкий кабриолет" },
            { "germancompact", "German compact", "Немецкий компакт" }, { "germansedan", "German sedan", "Немецкий седан" },
            { "germansedan_weak", "German sedan (weak)", "Немецкий седан (слабый)" }, { "minicab", "Minicab", "Мини-такси" },
            { "italiancompact", "Italian compact", "Итальянский компакт" }, { "limo", "Limousine", "Лимузин" },
            { "modifiedsuv", "Modified SUV", "Модифицированный внедорожник" }, { "moped", "Moped", "Мопед" }, { "scooter", "Scooter", "Скутер" },
            { "superbike", "Superbike", "Спортбайк" }, { "suv", "SUV", "Внедорожник" }, { "opelastra", "Opel Astra", "Opel Astra" },
            { "pontiacg8", "Pontiac G8", "Pontiac G8" }, { "ambulance", "Ambulance", "Скорая помощь" }, { "movingvan", "Moving van", "Фургон для переезда" },
            { "securityvan", "Security van", "Фургон охраны" }, { "semi", "Semi truck", "Седельный тягач" }, { "tanker", "Tanker", "Цистерна" },
            { "transporter", "Transporter", "Транспортёр" }, { "flatbed", "Flatbed truck", "Бортовой грузовик" }, { "truckcab", "Truck cab", "Кабина грузовика" },
            { "truckandtrailer", "Truck with trailer", "Грузовик с прицепом" }, { "truckandmissiontrailer", "Truck with mission trailer", "Грузовик с миссионным прицепом" },
            { "trucktrailerandcargo", "Truck, trailer and cargo", "Грузовик, прицеп и груз" }, { "helicopter", "Helicopter", "Вертолёт" },
            { "veh_train_vehicle_template", "Train", "Поезд" }, { "flatbedwithcargo", "Flatbed with cargo", "Бортовой грузовик с грузом" },
            { "transporterwithcargo", "Transporter with cargo", "Транспортёр с грузом" },
        };

        // keywords looked for in the lower-case template name
        const Tr3 kKeyword[] = {
            { "boss", "boss", "босс" }, { "hard", "hard", "усиленная" }, { "easy", "easy", "лёгкая" }, { "wingmen", "wingmen", "прикрытие" },
            { "explody", "explosive", "взрывная" }, { "battered", "battered", "потрёпанная" }, { "prisoner", "prisoner transport", "перевозка заключённых" },
            { "security", "security", "охрана" }, { "riot", "riot", "ОМОН" }, { "player", "player's", "игрока" }, { "felipe", "Felipe's", "Фелипе" },
            { "sorin", "Sorin's", "Сорина" }, { "micca", "Micca's", "Микки" }, { "ches", "Che's", "Че" }, { "gallo", "Gallo's", "Галло" },
            { "broker", "Broker's", "Брокера" }, { "buyer", "buyer's", "покупателя" }, { "fugitive", "fugitive", "беглец" }, { "panda", "Panda", "Панда" },
            { "mto", "Made to Order", "на заказ" },
        };

        bool EqI(const char* a, const char* b) { return _stricmp(a, b) == 0; }
        const char* Pick(const Tr3& t, int lang) { return lang ? t.ru : t.en; }

        // "FrenchHatchback" -> "French Hatchback" (fallback for names that are not in the tables)
        void CamelSplit(const char* in, char* out, size_t cap)
        {
            size_t o = 0;
            for (size_t i = 0; in[i] && o + 2 < cap; ++i)
            {
                if (i > 0 && in[i] >= 'A' && in[i] <= 'Z' && in[i - 1] >= 'a' && in[i - 1] <= 'z') out[o++] = ' ';
                out[o++] = in[i] == '_' ? ' ' : in[i];
            }
            out[o] = 0;
        }

        // lang 0 = English, 1 = Russian
        void BuildShortName(const CatEntry& e, int lang, char* out, size_t cap)
        {
            char comp[6][64] = {};
            int nc = 0;
            {
                const char* p = e.path;
                while (*p && nc < 6)
                {
                    const char* dot = strchr(p, '.');
                    size_t len = dot ? static_cast<size_t>(dot - p) : strlen(p);
                    if (len > 63) len = 63;
                    memcpy(comp[nc], p, len); comp[nc][len] = 0; ++nc;
                    if (!dot) break;
                    p = dot + 1;
                }
            }
            const char* raw = nc ? comp[nc - 1] : e.path;
            const char* name = (_strnicmp(raw, "VT_", 3) == 0) ? raw + 3 : raw;
            char lower[64];
            for (size_t i = 0; ; ++i) { lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i]))); if (!name[i]) break; }

            const char* group = nc >= 2 ? comp[0] : "";
            if (_strnicmp(group, "Veh_Templates_", 14) == 0) group += 14;
            const bool base = EqI(group, "Car") || EqI(group, "BigCar") || EqI(group, "Bike") || EqI(group, "Truck") ||
                              EqI(group, "Helicopter") || EqI(group, "train");
            if (base)
            {
                for (const Tr3& b : kBaseName) if (EqI(b.key, lower)) { snprintf(out, cap, "%s", Pick(b, lang)); return; }
                CamelSplit(name, out, cap);
                return;
            }

            const char* model = nullptr;
            const char* pk = _strnicmp(e.pkg, "Transitional_", 13) == 0 ? e.pkg + 13 : e.pkg;
            for (const Tr3& m : kPkgModel) if (EqI(m.key, pk)) { model = Pick(m, lang); break; }
            char fallback[64];
            if (!model) { CamelSplit(name, fallback, sizeof(fallback)); model = fallback; }

            char mods[160] = {};
            auto add = [&](const char* s) { if (mods[0]) strcat_s(mods, ", "); strcat_s(mods, s); };
            bool mission = false;
            if (EqI(group, "Police")) add(lang ? "полиция" : "police");
            else if (EqI(group, "Chullos")) add(lang ? "Чулос Канальяс" : "Chulos gang");
            else if (EqI(group, "Lantos")) add(lang ? "Лос-Лантос" : "Los Lantos gang");
            else if (EqI(group, "Romanian")) add(lang ? "Румыны" : "Romanian gang");
            else if (EqI(group, "StreetRacers")) add(lang ? "уличный гонщик" : "street racer");
            else if (EqI(group, "Events")) add(nc >= 3 && EqI(comp[1], "streetRace") ? (lang ? "уличная гонка" : "street race") : (lang ? "событие" : "event"));
            else if (EqI(group, "Mission")) mission = true;
            else if (EqI(group, "streetRace")) add(lang ? "уличная гонка" : "street race");
            if (mission)
            {
                char m[40] = {};
                snprintf(m, sizeof(m), "%s", lang ? "миссия" : "mission");
                if (const char* p = strstr(lower, "mis")) { int num = atoi(p + 3); if (num > 0) snprintf(m, sizeof(m), "%s %d", lang ? "миссия" : "mission", num); }
                add(m);
            }
            for (const Tr3& k : kKeyword)
                if (strstr(lower, k.key)) add(Pick(k, lang));
            if (mods[0]) snprintf(out, cap, "%s (%s)", model, mods); else snprintf(out, cap, "%s", model);
        }

        char g_short[2][kCatalogCount][96];
        int g_order[2][kCatalogCount];
        bool g_namesBuilt = false;

        void BuildNames()
        {
            if (g_namesBuilt) return;
            g_namesBuilt = true;
            for (int lang = 0; lang < 2; ++lang)
            {
                for (int i = 0; i < kCatalogCount; ++i) { BuildShortName(kCatalog[i], lang, g_short[lang][i], sizeof(g_short[lang][i])); g_order[lang][i] = i; }
                // Different templates can map to the same short name (e.g. two Romanian delivery vans): number the later ones.
                for (int i = 1; i < kCatalogCount; ++i)
                {
                    int seen = 0;
                    const size_t n = strlen(g_short[lang][i]);
                    for (int j = 0; j < i; ++j)
                    {
                        const char* a = g_short[lang][j];
                        if (_strnicmp(a, g_short[lang][i], n) == 0 && (a[n] == 0 || (a[n] == ' ' && a[n + 1] == '#'))) ++seen;
                    }
                    if (seen) { char tmp[96]; snprintf(tmp, sizeof(tmp), "%s #%d", g_short[lang][i], seen + 1); strcpy_s(g_short[lang][i], tmp); }
                }
                // insertion sort by short name (case-insensitive), stable
                for (int a = 1; a < kCatalogCount; ++a)
                {
                    const int cur = g_order[lang][a];
                    int b = a - 1;
                    while (b >= 0 && _stricmp(g_short[lang][g_order[lang][b]], g_short[lang][cur]) > 0) { g_order[lang][b + 1] = g_order[lang][b]; --b; }
                    g_order[lang][b + 1] = cur;
                }
            }
        }
    }

    int CatalogCount() { return kCatalogCount; }
    bool CatalogAt(int i, CatalogInfo& out)
    {
        if (i < 0 || i >= kCatalogCount) return false;
        BuildNames();
        out = { kCatalog[i].pkg, kCatalog[i].path, kCatalog[i].special, g_short[Lang::current == Lang::RU ? 1 : 0][i] };
        return true;
    }
    int CatalogSorted(int k) { BuildNames(); return k >= 0 && k < kCatalogCount ? g_order[Lang::current == Lang::RU ? 1 : 0][k] : -1; }


    bool RequestCatalog(int index, bool loadOnly)
    {
        if (index < 0 || index >= kCatalogCount) return false;
        if (g_catPending || g_pending) { catalogMessage = "a request is already queued"; return false; }
        if (!loadOnly && !g_spawnFactory) { catalogMessage = "factory not captured yet - wait until the game spawns a car by itself"; return false; }
        if (!TemplateClass()) { catalogMessage = "no vehicle definition known yet (drive around until traffic appears)"; return false; }
        g_catIndex = index;
        g_catLoadOnly = loadOnly;
        catalogMessage = "queued";
        InterlockedExchange(&g_catPending, 1);
        return true;
    }

    namespace
    {
        void RunCatalog()
        {
            const CatEntry& e = kCatalog[g_catIndex];
            wchar_t pkgW[96] = {}, pathW[160] = {};
            MultiByteToWideChar(CP_ACP, 0, e.pkg, -1, pkgW, 96);
            MultiByteToWideChar(CP_ACP, 0, e.path, -1, pathW, 160);
            const uintptr_t tcls = TemplateClass();
            if (!tcls) { catalogMessage = "no vehicle definition known"; return; }

            char note[200];
            uintptr_t def = FindTemplate(tcls, pathW);
            const bool wasLoaded = def != 0;
            if (!def)
            {
                sprintf_s(note, "catalog: loading package %s for %s", e.pkg, e.path);
                CrashHandler::Note(note);
                LogF("%s", note);
                const uintptr_t pk = LoadPkg(pkgW);
                LogF("catalog: LoadPackage(%s) returned 0x%08X", e.pkg, static_cast<unsigned>(pk));
                def = FindTemplate(tcls, pathW);
            }
            catalogDef = def;
            if (!def) { catalogMessage = "template not found even after loading its package"; LogF("catalog: %s not found", e.path); return; }
            uint32_t vt = 0, body = 0, mid = 0;
            Rd(def, vt); Rd(def + 0x34, body); Rd(def + 0x38, mid);
            LogF("catalog: %s -> 0x%08X vtable 0x%08X body 0x%08X model 0x%08X (%s)", e.path, static_cast<unsigned>(def),
                 vt, body, mid, wasLoaded ? "was resident" : "just loaded");
            if (g_catLoadOnly) { catalogMessage = wasLoaded ? "template already resident" : "package loaded, template found"; return; }
            bool usedParent = false;
            g_patchCount = 0;
            uintptr_t chain[8]; int chainLen = 0;
            if (!DefLooksLive(def))
            {
                // Derived variants (police, gang, mission, event ...) only store what differs from their parent
                // template (_ParentTemplate, def+0x30); the game merges the parent's mesh / physics / sim data in
                // when it needs them, so on their own they have no body (+0x34). Take the nearest ancestor that has
                // its data as the base, and lay the variants' own values over it for the duration of the spawn.
                uintptr_t base = 0, cur = def;
                for (int depth = 0; depth < 8; ++depth)
                {
                    chain[chainLen++] = cur;
                    uint32_t parent = 0;
                    if (!Rd(cur + 0x30, parent) || !ValidObject(parent)) break;
                    cur = parent;
                    if (DefLooksLive(cur)) { base = cur; break; }
                }
                if (!base) { catalogMessage = "variant has no loaded base template (see log)"; LogF("catalog: no populated ancestor for 0x%08X", static_cast<unsigned>(def)); return; }
                LogF("catalog: variant 0x%08X is not resolved, base template 0x%08X, %d level(s)", static_cast<unsigned>(def), static_cast<unsigned>(base), chainLen);
                def = base;
                usedParent = true;
            }
            const uintptr_t vcls = VehicleClass();
            if (!vcls) { catalogMessage = "vehicle class unknown"; return; }
            if (!ComputeSpawnSpot()) { catalogMessage = "player position unknown"; return; }
            g_class = vcls;
            g_def = def;
            catalogMessage = "spawning";
            if (usedParent) BuildOverlay(def, chain, chainLen);
            DoSpawn();
            RestoreOverlay();
            catalogMessage = status == 2 ? (usedParent ? "spawned (variant laid over its base template)" : "spawned") : lastMessage;
        }
    }

    // ---- cleanup of the vehicles the mod spawned -------------------------------------------------------------------
    bool autoRemove = true;
    bool keepNear = true;
    int maxSpawned = 12;
    float autoRemoveMeters = 400.f;
    float keepNearMeters = 100.f;
    int removedTotal = 0;

    // Switching the cleanup off is an Unsafe option (leftover vehicles can crash the game): with Unsafe off the
    // cleanup runs regardless of the saved setting.
    bool AutoRemoveActive() { return autoRemove || !Overlay::unsafeEnabled; }

    namespace
    {
        constexpr int kMaxTracked = 64;
        struct Tracked { uintptr_t ptr; ULONGLONG when; };
        Tracked g_tracked[kMaxTracked];
        int g_trackedCount = 0;
        volatile long g_removeMode = 0;   // 0 none, 1 all, 2 far only
        ULONGLONG g_lastCleanup = 0;

        void Track(uintptr_t v)
        {
            if (g_trackedCount == kMaxTracked)   // drop the oldest entry (it just stops being managed)
            {
                memmove(&g_tracked[0], &g_tracked[1], sizeof(Tracked) * (kMaxTracked - 1));
                --g_trackedCount;
            }
            g_tracked[g_trackedCount++] = { v, GetTickCount64() };
        }

        // UWorld::DestroyActor(actor, bNetForce): usercall, edi = actor, world and flag on the stack, callee pops 8.
        bool CallDestroyActor(uintptr_t world, uintptr_t actor)
        {
            uint32_t r = 0;
            __asm
            {
                mov edi, actor
                push 0
                push world
                mov edx, 0x0082DA40
                call edx
                mov r, eax
            }
            return r != 0;
        }

        bool DestroyVehicle(uintptr_t v)
        {
            uint32_t world = 0;
            if (!Rd(0x01760A1C, world) || !ValidObject(world)) return false;
            bool ok = false;
            __try { ok = CallDestroyActor(world, v); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogF("Spawner: DestroyActor(0x%08X) raised an exception", static_cast<unsigned>(v)); }
            return ok;
        }

        // Distance to the player in metres (1 unit = 1 cm); -1 if unknown.
        float DistanceMeters(uintptr_t v)
        {
            float px = 0, py = 0, pz = 0, x = 0, y = 0, z = 0;
            if (!Overlay::GetPlayerPosition(px, py, pz) || !Rd(v + 0xD4, x) || !Rd(v + 0xD8, y) || !Rd(v + 0xDC, z)) return -1.f;
            const float dx = x - px, dy = y - py, dz = z - pz;
            return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.f;
        }

        void Cleanup()
        {
            // forget vehicles that are gone (destroyed by the game, exploded and cleaned up ...)
            // (a vehicle younger than 5 s is kept regardless: the liveness check needs its physics set up)
            int w = 0;
            const ULONGLONG now = GetTickCount64();
            for (int i = 0; i < g_trackedCount; ++i)
                if (now - g_tracked[i].when < 5000 || IsLiveVehicle(reinterpret_cast<void*>(g_tracked[i].ptr))) g_tracked[w++] = g_tracked[i];
            g_trackedCount = w;

            const uintptr_t playerCar = reinterpret_cast<uintptr_t>(g_pVehicle);
            const long mode = InterlockedExchange(&g_removeMode, 0);
            bool remove[kMaxTracked] = {};
            int removable = 0;
            float dist[kMaxTracked];
            for (int i = 0; i < g_trackedCount; ++i)
            {
                dist[i] = DistanceMeters(g_tracked[i].ptr);
                const bool mine = g_tracked[i].ptr == playerCar;
                if (mine) continue;
                if (mode == 1) remove[i] = true;
                else if (mode == 2 && dist[i] >= keepNearMeters) remove[i] = true;
                else if (mode == 0 && AutoRemoveActive())
                {
                    const bool isNear = keepNear && dist[i] >= 0.f && dist[i] < keepNearMeters;
                    if (!isNear && dist[i] > autoRemoveMeters) remove[i] = true;
                }
            }
            if (mode == 0 && AutoRemoveActive())
            {
                // over the limit: the oldest ones go first, but never a vehicle that is near the player / the player's own
                int count = g_trackedCount;
                for (int i = 0; i < g_trackedCount; ++i) if (remove[i]) --count;
                for (int i = 0; i < g_trackedCount && count > maxSpawned; ++i)
                {
                    if (remove[i] || g_tracked[i].ptr == playerCar) continue;
                    if (keepNear && dist[i] >= 0.f && dist[i] < keepNearMeters) continue;
                    remove[i] = true; --count;
                }
            }
            (void)removable;
            int kept = 0;
            for (int i = 0; i < g_trackedCount; ++i)
            {
                if (remove[i])
                {
                    LogF("Spawner: removing vehicle 0x%08X (%.0f m away)", static_cast<unsigned>(g_tracked[i].ptr), dist[i]);
                    if (DestroyVehicle(g_tracked[i].ptr)) ++removedTotal;
                }
                else g_tracked[kept++] = g_tracked[i];
            }
            g_trackedCount = kept;
        }
    }

    int SpawnedCount() { return g_trackedCount; }
    bool DestroyActorNow(uintptr_t actor) { return DestroyVehicle(actor); }
    void RequestRemoveAll() { InterlockedExchange(&g_removeMode, 1); }
    void RequestRemoveFar() { InterlockedExchange(&g_removeMode, 2); }

    bool HasPending()
    {
        return g_pending != 0 || g_catPending != 0 || g_removeMode != 0 || (AutoRemoveActive() && g_trackedCount > 0);
    }

    void RunPending()
    {
        // vehicle cleanup runs twice a second, or at once when the user asked for it
        const ULONGLONG now = GetTickCount64();
        if (g_removeMode != 0 || (AutoRemoveActive() && g_trackedCount > 0 && now - g_lastCleanup > 500))
        {
            g_lastCleanup = now;
            Cleanup();
        }
        if (g_catPending)
        {
            InterlockedExchange(&g_catPending, 0);
            if (g_catIndex >= 0 && g_catIndex < kCatalogCount) RunCatalog();
            return;
        }
        if (!g_pending) return;
        InterlockedExchange(&g_pending, 0);
        DoSpawn();
    }

    void DoSpawn()
    {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastSpawn < 1500) { status = 3; lastMessage = "too soon after the previous spawn"; return; }
        g_lastSpawn = now;

        const uintptr_t f = g_spawnFactory;
        uint32_t vt = 0, oldClass = 0, oldDef = 0, oldCur = 0;
        if (!f || !Rd(f, vt) || vt < 0x1000000 || vt > 0x1800000 ||
            !Rd(f + 0x54, oldClass) || !Rd(f + 0x5C, oldDef) || !Rd(f + 0x40, oldCur))
        { status = 3; lastMessage = "factory not readable"; return; }

        uint32_t nameIdx = 0, loaded = 0;
        Rd(g_def + 0x20, nameIdx); Rd(g_def + 0x34, loaded);
        char note[128];
        sprintf_s(note, "spawn vehicle factory=%08X class=%08X def=%08X type=0x%X loaded=%d", static_cast<unsigned>(f),
                  static_cast<unsigned>(g_class), static_cast<unsigned>(g_def), static_cast<unsigned>(nameIdx), loaded != 0);
        CrashHandler::Note(note);

        Wr<uint32_t>(f + 0x54, static_cast<uint32_t>(g_class));
        Wr<uint32_t>(f + 0x5C, static_cast<uint32_t>(g_def));
        void* result = nullptr;
        bool threw = false;
        g_inOwnSpawn = true;
        __try { result = CallFactorySpawn(reinterpret_cast<void*>(f), g_loc, g_rot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { threw = true; }
        g_inOwnSpawn = false;
        Wr<uint32_t>(f + 0x54, oldClass);
        Wr<uint32_t>(f + 0x5C, oldDef);
        Wr<uint32_t>(f + 0x40, oldCur);

        if (threw) { status = 3; lastMessage = "factory spawn raised an exception"; LogF("Spawner: factory spawn raised an exception"); return; }
        lastSpawned = reinterpret_cast<uintptr_t>(result);
        if (result) Track(reinterpret_cast<uintptr_t>(result));
        status = result ? 2 : 3;
        lastMessage = result ? "spawned" : "factory spawn returned null (spot blocked?)";
        LogF("Spawner: factory 0x%08X (vtable 0x%08X) -> 0x%p", static_cast<unsigned>(f), static_cast<unsigned>(vt), result);
    }
}

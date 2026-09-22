#include <Windows.h>
#include <cmath>
#include "LoadGuard.h"
#include "Npc.h"
#include "Overlay.h"

namespace Npc
{
    bool scanning = false;

    namespace
    {
        constexpr uint32_t kVtbl = 0x013803C8;
        constexpr int kMaxPawns = 256;

        CRITICAL_SECTION g_cs;
        bool g_csInit = false;
        uintptr_t g_list[kMaxPawns];
        int g_count = 0;
        volatile long g_workerStarted = 0;

        template <class T> bool Rd(uintptr_t a, T& out)
        {
            __try { out = *reinterpret_cast<const T*>(a); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        int g_playerType = 1;   // the player pawn's m_CityCharacterType; refreshed in Snapshot

        // TArray<byte> at `off`: does it contain `type`?
        bool TypeListHas(uintptr_t off, int type)
        {
            uint32_t data = 0; int32_t num = 0;
            if (!Rd(off, data) || !Rd(off + 4, num) || data < 0x10000 || data > 0x7FFF0000 || num <= 0 || num > 32) return false;
            for (int i = 0; i < num; ++i)
            {
                uint8_t v = 0;
                if (Rd(static_cast<uintptr_t>(data) + i, v) && v == type) return true;
            }
            return false;
        }

        bool ReadPawn(uintptr_t p, Info& info)
        {
            uint32_t vt = 0; int32_t mark = 0, hp = 0, bits = 0;
            float x = 0, y = 0, z = 0;
            if (!Rd(p, vt) || vt != kVtbl || !Rd(p + 0x18, mark) || mark != -1) return false;
            if (!Rd(p + 0x2AC, hp) || !Rd(p + 0xD4, x) || !Rd(p + 0xD8, y) || !Rd(p + 0xDC, z)) return false;
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
            if (std::fabs(x) > 5.0e6f || std::fabs(y) > 5.0e6f || std::fabs(z) > 5.0e6f) return false;
            if (x == 0.f && y == 0.f && z == 0.f) return false;      // Default__ object / not placed
            if (hp < -100000 || hp > 100000) return false;
            Rd(p + 0x7D0, bits);
            int32_t flags1E8 = 0; uint8_t city = 0;
            Rd(p + 0x1E8, flags1E8); Rd(p + 0x5F4, city);
            info.address = p; info.health = hp; info.x = x; info.y = y; info.z = z;
            info.cityType = city;
            {
                // heading (rotator yaw, 65536 = a full turn): the vehicle's while riding one (vehicle vtable 0x132D930), else the pawn's
                int32_t yaw = 0; uint32_t veh = 0, vvt = 0;
                if (Rd(p + 0x4BC, veh) && veh > 0x10000 && Rd(static_cast<uintptr_t>(veh), vvt) && vvt == 0x0132D930u && Rd(static_cast<uintptr_t>(veh) + 0xE4, yaw)) {}
                else Rd(p + 0xE4, yaw);
                info.yaw = static_cast<float>(yaw) * (6.2831853f / 65536.f);
            }
            info.rawEnemy = (bits & (1 << 3)) != 0;
            info.invincible = (flags1E8 & ((1 << 14) | (1 << 15))) != 0;
            const bool kismetInv = (flags1E8 & (1 << 15)) != 0;
            const bool allyRel = TypeListHas(p + 0x810, g_playerType);
            const bool hostileRel = TypeListHas(p + 0x804, g_playerType);
            info.vip = city == 6 || (kismetInv && info.rawEnemy && !hostileRel && !allyRel);
            info.ally = !info.vip && allyRel && !hostileRel;
            info.friendly = info.vip || info.ally;
            info.police = city == 5 && !info.friendly;
            info.enemy = (info.rawEnemy || hostileRel) && !info.friendly && !info.police;
            return true;
        }

        // Component-space bone matrices -> world positions, hierarchy from the ref skeleton, head estimate.
        bool ReadBones(uintptr_t pawn, Info& info)
        {
            uint32_t comp = 0, data = 0;
            int32_t num = 0;
            if (!Rd(pawn + 0x354, comp) || comp < 0x10000) return false;
            if (!Rd(comp + 0x22C, data) || !Rd(comp + 0x230, num)) return false;
            if (num < 2 || num > kMaxBones || data < 0x10000) return false;
            float M[16];
            for (int i = 0; i < 16; ++i) if (!Rd(comp + 0x60 + i * 4, M[i])) return false;

            uint32_t mesh = 0, rdata = 0; int32_t rnum = 0;
            bool haveRef = Rd(comp + 0x1F0, mesh) && mesh > 0x10000 && Rd(mesh + 0x74, rdata) && Rd(mesh + 0x78, rnum) &&
                           rnum == num && rdata > 0x10000;
            for (int i = 0; i < num; ++i)
            {
                float lx, ly, lz;
                if (!Rd(data + i * 64 + 48, lx) || !Rd(data + i * 64 + 52, ly) || !Rd(data + i * 64 + 56, lz)) return false;
                if (!std::isfinite(lx) || !std::isfinite(ly) || !std::isfinite(lz)) return false;
                info.bone[i][0] = lx * M[0] + ly * M[4] + lz * M[8]  + M[12];
                info.bone[i][1] = lx * M[1] + ly * M[5] + lz * M[9]  + M[13];
                info.bone[i][2] = lx * M[2] + ly * M[6] + lz * M[10] + M[14];
                int32_t par = i;
                if (haveRef) { Rd(rdata + i * 68 + 0x38, par); if (par < 0 || par >= num) par = i; }
                else par = i > 0 ? i - 1 : 0;
                info.parent[i] = par;
            }
            // The pose must be near the pawn, otherwise the buffer is stale / not a skeleton.
            float dx = info.bone[0][0] - info.x, dy = info.bone[0][1] - info.y, dz = info.bone[0][2] - info.z;
            if (dx * dx + dy * dy + dz * dz > 600.f * 600.f) return false;

            // Head = the highest bone; if it is a leaf (eyes etc.) use the midpoint with its parent.
            int hi = 0;
            for (int i = 1; i < num; ++i) if (info.bone[i][2] > info.bone[hi][2]) hi = i;
            bool leaf = true;
            for (int i = 0; i < num; ++i) if (i != hi && info.parent[i] == hi) { leaf = false; break; }
            int hp = info.parent[hi];
            if (leaf && hp != hi)
                for (int k = 0; k < 3; ++k) info.head[k] = (info.bone[hi][k] + info.bone[hp][k]) * 0.5f;
            else
                for (int k = 0; k < 3; ++k) info.head[k] = info.bone[hi][k];
            info.boneCount = num;
            return true;
        }

        void ScanOnce()
        {
            uintptr_t found[kMaxPawns]; int n = 0;
            SYSTEM_INFO si; GetSystemInfo(&si);
            uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
            const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
            while (addr < maxAddr && n < kMaxPawns)
            {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
                uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = start + mbi.RegionSize;
                if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= 0x1000)
                {
                    __try
                    {
                        for (uintptr_t p = start; p + 0x8B0 <= end && n < kMaxPawns; p += 4)
                        {
                            if (*reinterpret_cast<const uint32_t*>(p) != kVtbl) continue;
                            Info tmp;
                            if (ReadPawn(p, tmp)) found[n++] = p;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                addr = end;
            }
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < n; ++i) g_list[i] = found[i];
            g_count = n;
            LeaveCriticalSection(&g_cs);
        }

        DWORD WINAPI Worker(LPVOID)
        {
            for (;;)
            {
                if (scanning && !LoadGuard::Quiet()) ScanOnce();
                Sleep(scanning ? 1500 : 500);
            }
        }
    }

    void Tick()
    {
        if (!scanning || g_workerStarted) return;
        if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
        g_workerStarted = 1;
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    int Snapshot(Info* out, int maxCount, bool withBones)
    {
        if (!g_csInit) return 0;
        if (const uintptr_t player = Overlay::GetPlayerPawn())
        {
            uint8_t t = 0;
            if (Rd(player + 0x5F4, t) && t < 32) g_playerType = t;
        }
        uintptr_t local[kMaxPawns]; int n;
        EnterCriticalSection(&g_cs);
        n = g_count;
        for (int i = 0; i < n; ++i) local[i] = g_list[i];
        LeaveCriticalSection(&g_cs);
        int m = 0;
        for (int i = 0; i < n && m < maxCount; ++i)
            if (ReadPawn(local[i], out[m]))
            {
                out[m].hasBones = withBones && ReadBones(local[i], out[m]);
                ++m;
            }
        return m;
    }
}

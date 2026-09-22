#pragma once
#include <cstdint>

// Enumerates AI pawns (WheelmanAIPawn instances) by memory scan.
// Found live: class vtable 0x013803C8, UObject +0x18 == -1, Actor.Location +0xD4,
// Pawn.Health +0x2AC, m_bIsEnemy = bit 3 of the dword at +0x7D0. The NPC to be rescued also has
// m_bIsEnemy set, but differs from real enemies by m_bKismetInvincible (bit 15 of +0x1E8),
// hp 200 and m_CityCharacterType (+0x5F4) = 6 (enemies 7, civilians 0).
//
// Skeleton (verified on a 29-bone NPC): Pawn.Mesh = pawn+0x354 -> SkeletalMeshComponent
//   comp+0x60   LocalToWorld (4x4, row-vector, translation in row 3)
//   comp+0x22C  TArray of 4x4 matrices (component-space bone transforms, "SpaceBases"),
//               a second copy at +0x238 (previous/next buffer)
//   comp+0x1F0  SkeletalMesh -> RefSkeleton TArray at +0x74, 68-byte records:
//               dword14 = ParentIndex, dword15 = NumChildren
namespace Npc
{
    constexpr int kMaxBones = 128;   // civilians have 29 bones, the enemy checked live has 83

    struct Info
    {
        uintptr_t address = 0;
        // Classification (see ReadPawn): every AI pawn keeps two byte arrays - m_aEnemyTypes (+0x804) and m_aAllyTypes
        // (+0x810) - of m_CityCharacterType values it is hostile / friendly to. The player's own type (pawn +0x5F4 = 1)
        // being in one of them says how the pawn treats the player. Mission allies riding bikes have it in m_aAllyTypes
        // although m_bIsEnemy is set on them too.
        bool enemy = false;      // hostile to the player (bosses included)
        bool ally = false;       // fights on the player's side
        bool vip = false;        // the NPC to protect / rescue (city type 6, or protected neutral)
        bool friendly = false;   // ally || vip
        bool police = false;     // m_CityCharacterType 5 = police (not counted as "enemy")
        bool rawEnemy = false;   // m_bIsEnemy exactly as stored (also set on protected NPCs)
        bool invincible = false; // m_bInvincible or m_bKismetInvincible (pawn +0x1E8 bits 14 / 15)
        int cityType = 0;        // WheelmanPawn.m_CityCharacterType (+0x5F4): 0 civilian, 7 enemy, 6 rescue target
        int health = 0;
        float x = 0, y = 0, z = 0;
        float yaw = 0;           // heading in radians (the vehicle's while riding one); forward = (cos, sin) in (X, Y)

        // Filled only when bones were requested and could be read.
        bool hasBones = false;
        int boneCount = 0;
        int parent[kMaxBones];
        float bone[kMaxBones][3];   // world-space positions
        float head[3];              // estimated head centre (world space)
    };

    extern bool scanning;                 // background scan runs only while this is on
    void Tick();                          // per frame, starts the worker on demand
    // Validated pawns with current values; bones (skeleton) are read only if withBones is set.
    int Snapshot(Info* out, int maxCount, bool withBones = false);
}

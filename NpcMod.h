#pragma once

// Modifications of AI pawns found by Npc.h. Offsets:
//   pawn +0x1E8 flag bits: b10 bNoWeaponFiring, b14 m_bInvincible, b15 m_bKismetInvincible
//   pawn +0x7D0 flag bits: b1 m_bShooting
//   pawn +0x2AC Health, +0x6F8 max/initial health
//   pawn +0x54C MwyAICoreVisionComponent (+0x80 bit0 m_bBlind)
//   pawn +0x834 WheelmanCombatComponent (bit dword +0x54: b0 AllowCombat, b4 HuntPlayer,
//                b5 TargetPlayer, b6 ForgetTarget; +0x74 m_pEnemyTarget)
namespace NpcMod
{
    extern bool keepDisarmed;      // enemies can't fire weapons (re-applied continuously)
    extern bool enemiesIgnore;     // enemies are blind, don't target the player and forget their target
    extern bool alliesInvincible;  // friendly NPCs can't be hurt
    extern bool alliesKeepHealth;  // friendly NPCs are kept at full health
    extern bool enemiesVulnerable; // enemies flagged invincible (bosses, scripted) lose that protection, re-applied continuously

    extern bool policeDisarmed;    // police officers can't fire weapons (same mechanism as keepDisarmed)
    extern bool blindAll;          // stealth / tailing missions: every AI pawn (mission targets, pedestrians, police) gets a blind vision component
    extern bool policeIgnore;      // police are blind, don't target the player and forget their target

    // Per-NPC invincibility (pawn +0x1E8 bits 14 / 15). The address must come from a fresh Npc::Snapshot.
    void SetInvincible(uintptr_t pawn, bool on);

    void DisarmNow();              // one shot: every tracked enemy gets bNoWeaponFiring
    void HealAlliesNow();          // one shot: full health for every friendly NPC
    void Tick();                   // per frame (throttled internally)

    // Counters for the tab.
    extern int statEnemies, statAllies;
}


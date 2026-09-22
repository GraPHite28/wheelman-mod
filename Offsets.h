#pragma once
#include <cstdint>

// AOB signatures copied verbatim (wildcards preserved) from the community
// Cheat Engine table's aobscan() calls. Addresses are resolved at runtime by
// scanning the loaded module for these, because the static "Wheelman.exe+X"
// comments in that table only matched the table author's exe build - this
// repack's Wheelman.exe has different code at those raw offsets.
namespace Signatures
{
    struct Pattern { const unsigned char* bytes; const char* mask; };

    constexpr unsigned char MissionTimerBytes[] = { 0xF3,0,0,0,0,0x0F,0,0,0xF3,0,0,0,0,0,0,0,0x72,0,0xF3,0,0,0,0,0,0,0,0xEB,0,0x0F,0,0,0 };
    constexpr char MissionTimerMask[] = "x????x??x???????x?x???????x?x???";

    constexpr unsigned char RamBoomBytes[] = { 0x83,0,0,0,0,0,0,0x89,0,0,0,0xBA,0,0,0,0,0x7E,0,0x8B,0,0,0,0xF6,0,0,0,0,0,0,0x75,0,0 };
    constexpr char RamBoomMask[] = "x??????x???x????x?x???x??????x??";

    constexpr unsigned char ManaBytes[] = { 0xF3,0,0,0,0,0x83,0,0,0x0F,0,0,0xF2,0,0,0,0,0,0,0xE8,0,0,0,0,0x83,0,0,0xF6,0,0,0,0x75 };
    constexpr char ManaMask[] = "x????x??x??x??????x????x??x???x";

    constexpr unsigned char NoReloadBytes[] = { 0x8B,0,0,0,0,0,0x3B,0,0,0,0,0,0x74,0,0x83,0,0,0,0,0,0,0x75,0,0xF6,0,0,0,0,0,0,0x74 };
    constexpr char NoReloadMask[] = "x?????x?????x?x??????x?x??????x";

    constexpr Pattern MissionTimer = { MissionTimerBytes, MissionTimerMask };
    constexpr Pattern RamBoom      = { RamBoomBytes, RamBoomMask };
    constexpr Pattern Mana         = { ManaBytes, ManaMask };
    constexpr Pattern NoReload     = { NoReloadBytes, NoReloadMask };
}

// Struct field offsets, discovered via the community Cheat Engine table and
// confirmed by live testing in-game.
namespace Offsets
{
    constexpr int Vehicle_Health     = 0x2AC; // int32, read via FILD (int->float)
    constexpr int Vehicle_ActiveFlag = 0x560; // int32, ==1 specifically for the player's own vehicle
    constexpr int Pawn_Boost         = 0x05C; // float, nitro/boost-like resource
    constexpr int Pawn_Ammo          = 0x2F0; // int32

    // Found by disassembling the RamBoom hook site: it computes a squared
    // distance using three consecutive floats at these offsets against
    // another position, i.e. this is the vehicle's world X/Y/Z.
    constexpr int Vehicle_PosX = 0x0D4;
    constexpr int Vehicle_PosY = 0x0D8;
    constexpr int Vehicle_PosZ = 0x0DC;

    // Found by scanning the whole binary for writes to +0x3F4: a dedicated
    // AddClamped(delta)-style method adds to it and clamps against +0x3F8;
    // another function zeroes +0x3F4 as part of what looks like an on-death
    // log/reset path; a third does "if (+0x3F4 == 0) call OnSomething()".
    // Very consistent with Health/MaxHealth (float, not int).
    constexpr int Pawn_Health    = 0x3F4;
    constexpr int Pawn_MaxHealth = 0x3F8;

    // Two more player-health candidates the user found by their own live
    // testing (method/tool not yet documented here) - NOT independently
    // confirmed against Pawn_Health above yet. Kept for reference until
    // cross-checked (e.g. via ScanSnapshot/ScanNarrow in Patches.cpp while
    // taking damage, comparing against these exact offsets).
    constexpr int Pawn_HealthCandidate2 = 0x228;
    constexpr int Pawn_HealthCandidate3 = 0x428;

    // Found in the SAME function as one of the confirmed +0x2AC (health)
    // checks (0x00509D73 - anchored to a real vehicle object, not a
    // coincidentally-shared base-class offset like an earlier, wrong guess
    // at +0xE0/E4/E8 was). Confirmed CORRECT by live testing: matches actual
    // speed/direction while driving. However it's read-only in practice -
    // unlike the position mirror (+0xD4/D8/DC), which at least flashes
    // before Havok reverts it, writing here has NO visible effect at all,
    // even via a Havok rigid-body candidate found by correlating pointers
    // against both this and the position mirror at once. Conclusion: this
    // (like position) is a one-way Havok-to-game mirror - nothing reads it
    // back into the simulation, so it's only useful as telemetry (e.g. a
    // speedometer), not as a way to apply a real impulse.
    constexpr int Vehicle_VelX = 0x0EC;
    constexpr int Vehicle_VelY = 0x0F0;
    constexpr int Vehicle_VelZ = 0x0F4;

    // ---- Experimental: found via decompiling AI-perception code that's
    // anchored to a confirmed vehicle (same function reads +0x2AC health and
    // +0xD4/D8/DC position on the identical pointer). Semantics inferred
    // from code shape, NOT independently confirmed like the fields above -
    // treat as "probably useful, exact meaning fuzzy" and please correlate
    // visually in-game (that's *why* these are exposed as raw ESP readouts
    // instead of a guessed label, unlike the earlier wrong +0xE0/E4/E8 pick).

    // Pointer, null-checked before the game proceeds to run FOV/distance
    // "can I perceive this vehicle" math and then calls a virtual method
    // THROUGH this pointer (not on the vehicle itself) to deliver an AI
    // stimulus/notification. Originally guessed as "has an active driver" -
    // DISPROVEN by live testing: it reads non-null on every vehicle,
    // including ones visibly parked with nobody in them. So it's some
    // universally-present per-vehicle component (AI/perception-related
    // going by the code it's used from), not a driver-presence flag. Kept
    // here for reference but no longer surfaced in the ESP.
    constexpr int Vehicle_AIControllerPtr = 0x4D4;

    // Byte. CONFIRMED by live visual correlation (drove around, matched
    // values against real vehicle types): 0 = car (including minibuses),
    // 1 = any motorcycle/two-wheeler, 2 = truck or heavy van. Also compared
    // against 3 and 4 at other call sites in the binary (e.g. one AI-
    // reaction function skips ramming/chase logic when this equals 3), but
    // those two values weren't encountered while sampling ordinary traffic -
    // meaning unconfirmed (wrecked state? mission-specific/special vehicle?
    // emergency services?).
    constexpr int Vehicle_Category = 0x5AC;

    // Byte. Required to be 0 (alongside health>0 and the AI controller
    // pointer above) for a vehicle to be treated as a valid AI-perception
    // target - guessing "wrecked/hidden/despawning". Live sampling only ever
    // saw 0 across every vehicle checked, so still unconfirmed and not
    // currently useful for anything - kept for reference.
    constexpr int Vehicle_StateFlag = 0x5F4;

    // Byte, CONFIRMED by live visual correlation: steering wheel angle,
    // 0 = full left lock, 255 = full right lock. Changes continuously while
    // driving - not an identity trait, just current input state.
    constexpr int Vehicle_SteeringAngle = 0x54C;

    // Dword, CONFIRMED by live visual correlation as a finer body-style
    // subclass than Vehicle_Category (which only says car/moto/truck).
    // Known values so far: 56 = any motorcycle; 67 = heavy truck (e.g. the
    // user's reference point was a KamAZ-style truck); 70 = shared by retro
    // convertible, regular convertible, regular minibus AND heavy minibus
    // (messy grouping - these don't share an obvious single label); 71 =
    // SUV/off-roader and most other cars including the racing convertible;
    // 72 = small 4-wheelers (quad/buggy-type). Values are assigned per
    // vehicle template, not cleanly per body shape, so treat groupings as
    // "whatever the game happens to use the same ID for" rather than a
    // clean taxonomy. Story-specific vehicles that don't spawn via the
    // normal traffic system haven't been sampled - may use other values.
    constexpr int Vehicle_BodyStyle = 0x500;

    // Dword. Varies noticeably even between different instances of the SAME
    // model, which first looked promising for paint color - but DISPROVEN
    // by live testing: it doesn't move when repainting your own car in the
    // garage. Real meaning still unknown. Kept as "Tag1" in the ESP in case
    // a pattern turns up for something else.
    constexpr int Vehicle_ColorCandidate = 0x190;

    // Pointer on the vehicle that gets REALLOCATED on every repaint in the
    // garage (confirmed via the Snapshot/Compare diff tool in Patches.cpp) -
    // so Vehicle_PaintObjectPtr itself is meaningless moment to moment, but
    // +0x24 inside whatever it points to is the real paint color index:
    // CONFIRMED by live testing across multiple repaints (40->45 in one
    // session; 9->30->31 across three repaints in another) - always a
    // small, clean integer that changes exactly when and only when
    // repainting. Read it as:
    //   int32_t paintObjPtr; ReadInt(vehicle, Vehicle_PaintObjectPtr, paintObjPtr);
    //   int32_t colorIndex;  ReadInt((void*)paintObjPtr, Vehicle_PaintIndexOffset, colorIndex);
    // (see Overlay.cpp ReadPaintIndexCandidate for the actual helper).
    // Specific index->real-color mapping not yet catalogued.
    // ---- Real field names, derived from Default__ object dumps in
    // MwyVehicle.upk (see Tools\decompiler\DumpDefaults.cs): a
    // "SerializedGroup" tag is a raw memory image with the true byte offset
    // of its first field. Chain anchored on offsets confirmed live earlier:
    //   0x54C = m_SimState.AnalogLStickX (what we called SteeringAngle)
    //   0x54D AnalogLStickY, 0x54E AnalogButton1, 0x54F AnalogButton2,
    //   0x550 bDigitalButton1, 0x554 ServerViewPitch, 0x558 ServerViewYaw,
    //   0x55C m_AngErrorAccumulator (float)
    //   0x560 m_iNumPlayersOnBoard  (== our "ActiveFlag": 1 = player inside)
    //   0x564 m_pNextMwyVehicle, 0x568 m_pParentVehicle, 0x56C m_pConstraintToParent
    //   0x570 m_apChildVehicles, 0x57C m_apConstraintsToChildren,
    //   0x588 m_aWindowBoneNames, 0x594 m_aSirenBoneNames, 0x5A0 m_aExplosiveBones
    //   0x5AC m_eVehicleType (== our Category), 0x5AD m_eLastCollisionVehicleType
    // Older/upstream part (derived by walking backwards, assumes
    // sizeof(RBState)=56 - verify live): 0x4D4 bool bitfield (m_bPostBeginPlay
    // etc. - so NOT "has driver"), 0x4D8 m_Simulations, 0x4E4 m_SeatInstances,
    // 0x4F0 m_DoorInstances, 0x4FC m_EffectInstances (num at 0x500 == the
    // "BodyStyle" value: it's an array COUNT, not a body id), 0x508 m_Cameras
    // (TArray of MwyVehicleCamera*), 0x514 m_SimState.RBState.
    // NOTE: m_iHealth/m_iMaxHealth are fields of struct ExplosiveBone, NOT of
    // the vehicle - the real vehicle health at 0x2AC is inherited (Pawn.Health).
    // Camera object: +0x30 m_Name, +0x38 m_FOV, +0x3C m_bIsFirstPerson bits,
    // +0x40 m_Height, +0x44 m_Distance, +0x48 m_CurrentYaw.
    constexpr int Vehicle_CamerasArray = 0x508;

    constexpr int Vehicle_PaintObjectPtr  = 0x170;
    constexpr int Vehicle_PaintIndexOffset = 0x24;
}

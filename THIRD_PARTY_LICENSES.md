# Third-party software and what may be shared

A working note about the licence position of everything the mod uses. It is **not legal advice**.

## What is in this repository

| Component | Where | Licence | Notes |
|---|---|---|---|
| **Dear ImGui** 1.92.x (overlay UI) by Omar Cornut | `imgui/` | MIT (`imgui/LICENSE.txt`) | Contains small marked changes ("WheelmanMod") for the UI translation hook. |
| **UELib** (Unreal package reader) by Eliot van Uytfanghe | `tools/MapDataBuilder/lib/Eliot.UELib.dll` (+ `UELib-LICENSE.txt`) | MIT | A local build of the upstream sources, used only by the map data builder. |
| `System.Buffers`, `System.Memory`, `System.Numerics.Vectors`, `System.IO.Hashing`, `System.Runtime.CompilerServices.Unsafe` | `tools/MapDataBuilder/lib/` | MIT (Microsoft) | Runtime dependencies of UELib. |
| **FXAA kernel** in `PostFx.cpp` | source | Timothy Lottes' public FXAA 2 algorithm (public domain), re-typed | Credit kept in the source comment. |
| Everything else | this repository | MIT (`LICENSE`) | |

## What is NOT in this repository and must stay out

* **The game "Wheelman"** (© Midway / Warner Bros. / Tigon): no game files, no packages (`*.xxx`, `*.upk`), no textures, no decompiled
  scripts, no edited `Coalesced.ini`.
* **`MapData\`** (`citymap.dxt1`, `icons.bgra`, `roads.bin`): the city map picture, the marker icons and the road mask are the game's
  artwork. Every player generates them from their own game copy (Launcher button "Prepare the map and icons from my game" or
  `tools\MapDataBuilder`). They are git-ignored.
* **ReShade** (BSD 3-clause, © Patrick Mours) and any ReShade shader packs: the mod does not need them (it has its own post-processing
  in `PostFx.cpp`). The ReShade site asks people not to redistribute its binaries and shader files; link to https://reshade.me instead.
  Shader collections such as qUINT (© Pascal Gilcher, all rights reserved) must not be redistributed.
* **DXVK, x64dbg, umodel, UE Explorer**, ... : development helpers that were used on the way; not part of this project.

## The game and the law, briefly

* The mod contains no game code or assets. It changes the memory of a running game process and draws an overlay; it does not remove or
  bypass copy protection and does not enable piracy. You need a legal copy of the game.
* It is meant for the **single-player** game. There is no anti-cheat, and the online mode of the game is dead; do not use tools like
  this in online games of any kind.
* Reverse engineering for interoperability is permitted in many jurisdictions (for example EU Directive 2009/24/EC art. 6, US DMCA
  §1201(f)), but the game's EULA may forbid it and the rules differ from country to country. Contributors are responsible for what they
  add. Do not contribute leaked source code, game assets or anything under an incompatible licence.
* "Wheelman" and the names of the game's companies are trademarks of their owners and are used here only to say what the mod is for.
  This project is not affiliated with or endorsed by them.

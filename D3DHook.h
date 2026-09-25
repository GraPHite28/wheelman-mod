#pragma once
#include <Windows.h>
#include <d3d9.h>

// Called once we intercept the game's own, real CreateDevice call (see
// IATHook.cpp). Patches EndScene/Reset on that real device's vtable.
void HookDeviceVTable(IDirect3DDevice9* pDevice);

// Cleans up ImGui / restores the window proc. Does not touch the vtable
// (see the note in D3DHook.cpp).
void ShutdownOverlay();

// The game is shader-driven (fixed-function GetTransform always reads back
// identity), so there's no D3DTS_VIEW/PROJECTION to query directly. Found by
// hooking SetVertexShaderConstantF and watching which register updates
// rarely per frame (a handful of times, unlike per-object world matrices
// which update per draw call) yet visibly rotates with the camera and
// carries world-scale translation values - vertex shader constant register
// c240, confirmed empirically. Row-major, row-vector convention (translation
// in the last row), consistent with plain D3DXMATRIX usage elsewhere in the
// game. Returns false if no such constant has been observed yet this session.
bool GetCandidateViewProjMatrix(float outRowMajor16[16]);
extern int g_viewportWidth, g_viewportHeight;

// Graphics debugging: logs every draw call of the next frame (states, shaders, textures, render targets) to %TEMP%\WheelmanMod_draws.txt.
// Sky experiment: the game draws two identical sky domes in a row (2048x512 DXT1 textures, same shaders and states, no blending),
// which flickers. 0 = as the game does, 1 = draw only the first dome, 2 = draw only the last one.
extern int g_skyMode;
// Shadow map pass tuning (live): the game's own depth bias / slope bias of the shadow pass are scaled by these; 1 = as the game does.
extern float g_shadowBiasScale, g_shadowSlopeScale;
void RequestFrameCapture();
const char* FrameCaptureStatus();

// EXPERIMENTAL, Debug tab only (2026-09-25): wraps the game's own per-frame call (everything hkEndScene does,
// including the real EndScene call into the game/engine itself) in a structured-exception __try/__except, so a
// hardware exception there (an access violation, ...) is caught and that one frame is skipped instead of the whole
// process dying. This does NOT fix whatever caused the fault or guarantee the game keeps working correctly
// afterwards - it only stops that specific crash from being fatal; corrupted state can still show up as visual
// glitches, a frozen screen, or a crash somewhere else next frame. Auto-disables itself (falls back to letting a
// crash be a crash) if it has to catch an unreasonable number of exceptions in a short time, so a crash loop can't
// hang the process forever silently.
extern bool g_catchMainLoopExceptions;
int MainLoopExceptionsCaught();      // total caught this session, for the UI
const char* MainLoopExceptionStatus(); // last caught exception (code/address), or "" if none yet

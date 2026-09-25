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

// Frame debugger (2026-09-25): a home-grown, much smaller RenderDoc/PIX - those don't do D3D9 (RenderDoc dropped it
// entirely, current PIX is D3D12-only), so this piggybacks on the same one-frame capture above. While it runs, a
// small (160x90) downscaled copy of the render target is grabbed right after every draw call, kept as its own tiny
// GPU texture - so once the capture finishes the Debug tab can show, for any draw call index, what the screen looked
// like right after it, and you can step through the frame one draw at a time. Capped at kFrameThumbMax draw calls
// (a very draw-heavy frame only gets thumbnails for the first that many; the full per-draw state list in
// WheelmanMod_draws.txt still covers everything). A draw whose target isn't a StretchRect-copyable colour format
// (shadow maps, the depth buffer itself, ...) just has no thumbnail for that slot - harmless, skipped.
constexpr int kFrameThumbMax = 600;
constexpr UINT kFrameThumbW = 320, kFrameThumbH = 180;   // only full-screen-sized draws are kept now, so 600 slots go a lot further
int FrameThumbCount();                 // how many full-screen-sized draw calls got a thumbnail from the last capture (0 if none yet); smaller/differently-shaped off-screen passes (shadow maps, reflections, ...) are skipped entirely, so this is a gap-free, meaningful list
int FrameThumbDrawIndex(int index);    // the real draw-call index (matches WheelmanMod_draws.txt) this thumbnail slot came from, -1 if out of range
void* FrameThumbTexture(int index);    // LPDIRECT3DTEXTURE9 for that slot, or nullptr if out of range

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

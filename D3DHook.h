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

#pragma once
#include <d3d9.h>

// A small post-processing pass drawn over the finished frame (right before the overlay), the way ReShade does it, without
// installing anything: the back buffer is copied to a texture and drawn back through pixel shaders that do
//   * ambient occlusion (SSAO) - needs the scene depth, see below,
//   * FXAA (edge anti-aliasing: the game has no anti-aliasing at all),
//   * sharpening (lifts the softness of the 2009 textures),
//   * colour grading (saturation, contrast, gamma).
// The shaders are compiled at run time with the system's d3dcompiler_47.dll. Nothing runs while every effect is off.
//
// Depth: the game's scene depth buffer is a plain D24S8 surface that cannot be read by a shader. The device's
// CreateDepthStencilSurface is hooked and every big D24S8 surface is created as an INTZ texture surface instead (INTZ is the
// vendor-neutral "depth as texture" format of D3D9; the game keeps using it as an ordinary depth-stencil surface, and
// the stencil bits stay usable). The surface bound last as depth-stencil at frame end is the scene depth. The projection
// (near/far constants and the focal lengths) is taken from the game's view-projection constant register c240 as it was at
// the first draw after the depth buffer was bound.
namespace PostFx
{
    extern bool enabled;
    extern float fxaa;          // 0 = off, 1 = full strength
    extern float sharpen;       // 0 = off, ~0.3 mild, 1 strong
    extern float saturation;    // 1 = unchanged
    extern float contrast;      // 1 = unchanged
    extern float gamma;         // 1 = unchanged (>1 brighter mid-tones)
    extern float aoStrength;    // 0 = off, 1 = full
    extern float aoRadius;      // world units (cm) the occlusion looks around a pixel
    extern float aoBias;        // fraction of the distance that is ignored (kills self-shadowing of flat surfaces)
    extern float aoMaxDistance; // no occlusion beyond this distance from the camera (cm)
    extern float fogStrength, fogStart, fogDistance, fogR, fogG, fogB;   // distance haze (start / distance in metres)
    extern float dofAmount, dofFocus, dofZone, dofTransition;            // depth of field (px / metres)
    extern bool dofAuto;
    extern bool debugDepth;    // show the linearised depth instead of the picture (to check the depth is right)
    extern bool debugAO;        // show the occlusion buffer only
    extern const char* status;
    extern char depthStatus[160];

    void InstallHooks(void** vTable);       // CreateDepthStencilSurface (29) and SetDepthStencilSurface (39)
    void Apply(IDirect3DDevice9* dev);      // render thread, at the end of the frame, before the overlay
    void ReleaseDeviceObjects();            // before Device::Reset
    extern volatile long wantMatrix;        // set when the scene depth was just bound: the next draw call snapshots c240
    void CaptureMatrix();
    void OnDraw();                          // every DrawIndexedPrimitive: matrix snapshot + draw counter of the current depth surface
}

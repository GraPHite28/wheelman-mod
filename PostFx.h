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

    // Fake ray-traced lighting (Beta, rewritten 2026-09-25 as its own pass, separate from AO): for each pixel, casts
    // several real multi-step rays (actual marching + reprojection, not a fixed small hemisphere like AO) inside a
    // cone around a direction that blends from the mirror reflection vector (roughness 0) to the surface normal
    // (roughness 1) - one slider covers both "sharp fake reflection" and "diffuse bounced light", the same way a
    // roughness parameter does in a real renderer. Where a ray hits something, the hit's own colour is picked up
    // weighted by the cosine terms at BOTH ends (does our surface receive light from that direction; is the hit
    // surface even facing back towards us) - a one-bounce light transport estimate, not just a mirror copy. The
    // noisy per-pixel result is smoothed with the same depth-aware blur AO/SSR's normal use. Adds light on top of
    // the picture, does not replace anything the game itself draws or remove real shadows.
    extern bool fakeRTEnabled;
    extern float fakeRTStrength;      // 0 = off
    extern float fakeRTRoughness;     // 0 = tight around the mirror reflection, 1 = spread around the normal (diffuse)
    extern float fakeRTStepCm;        // first ray step (accelerates after that), per sample
    extern float fakeRTMaxDistanceCm; // give up past this distance from the camera
    extern bool debugFakeRT;          // show the raw (blurred, unscaled by strength, pre-AO-link) bounce buffer only

    // Finer control over the ray marching itself (2026-09-24): more rays/steps cost roughly linearly more GPU time;
    // bounces multiply that cost by roughly the same amount again (a ray that hits something keeps going from there).
    extern int fakeRTRayCount;    // cone-sampled rays per pixel, 1-8 (default 4, matches the original fixed count)
    extern int fakeRTStepCount;   // ray-march steps per ray/bounce, 2-16 (default 8, matches the original fixed count)
    extern int fakeRTBounces;     // extra bounces after the first hit, 1-3 (default 1 = old single-bounce behaviour)

    // Experimental temporal reprojection (2026-09-24): see the comment by kTemporalBlend in PostFx.cpp for exactly
    // what this can and cannot do - there is no real motion vector, only a same-pixel depth-rejection test standing
    // in for one, so it helps with flicker while the camera holds still and visibly smears during fast motion.
    extern bool fakeRTTemporal;
    extern float fakeRTTemporalWeight; // 0 = no history kept (same as temporal off); close to 1 = very smooth but laggy

    // Screen-space reflections - crude first pass (Debug tab only, 2026-09-25): ray-marches the existing depth
    // buffer using a per-pixel normal reconstructed the same way SSAO does, no material/roughness data (the game
    // has none we can get at - it renders forward, not deferred), so this reflects EVERYTHING by the same amount
    // rather than only genuinely reflective surfaces. Expect it to look wrong on matte surfaces; it exists to see
    // whether the reflections themselves line up before spending more effort finding a material signal.
    extern bool ssrEnabled;
    extern float ssrStrength;      // 0 = off, 1 = full mix at grazing angles
    extern float ssrStepCm;        // world-space distance covered by the first ray step (accelerates after that)
    extern float ssrMaxDistanceCm; // give up past this distance from the camera
    extern float ssrGroundBias;    // 0 = reflect every surface the same, 1 = only near-horizontal ("ground-like") ones

    void InstallHooks(void** vTable);       // CreateDepthStencilSurface (29) and SetDepthStencilSurface (39)
    void Apply(IDirect3DDevice9* dev);      // render thread, at the end of the frame, before the overlay
    void ReleaseDeviceObjects();            // before Device::Reset
    extern volatile long wantMatrix;        // set when the scene depth was just bound: the next draw call snapshots c240
    void CaptureMatrix();
    void OnDraw(IDirect3DDevice9* dev);     // every DrawIndexedPrimitive: matrix snapshot + draw counter of the current depth surface
}

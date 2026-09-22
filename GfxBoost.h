#pragma once
#include <d3d9.h>

// Runtime picture improvements done on the D3D9 device (nothing is written to the game files):
//   * anisotropic filtering: every LINEAR min/mag sampler state the game sets is turned into ANISOTROPIC (2..16x). The
//     game (2009) only ever asks for bilinear/trilinear, so roads and walls seen at an angle are blurry.
//   * texture LOD bias: shifts the mip level the GPU picks (negative = sharper textures).
namespace GfxBoost
{
    extern int anisotropy;      // 1 = as the game does, 2..16
    extern float lodBias;       // 0 = as the game does, -2..+1 (mip level offset)

    void Install(void** vTable);          // patches IDirect3DDevice9::SetSamplerState (vtable slot 69)
    void OnEndScene(IDirect3DDevice9* dev);   // render thread, once per frame: re-applies the states after a setting change
}

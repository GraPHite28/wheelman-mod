#include <Windows.h>
#include <cstring>
#include "GfxBoost.h"
#include "Log.h"

namespace GfxBoost
{
    int anisotropy = 16;        // the user's choice after testing: sharp roads and walls, no visible side effects
    float lodBias = -1.0f;

    namespace
    {
        typedef HRESULT(__stdcall* SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
        SetSamplerState_t oSetSamplerState = nullptr;

        constexpr int kStages = 16;
        constexpr int kAnisoStages = 8;              // world textures live in the first stages; the rest (shadow maps, post-processing) stay untouched
        DWORD g_raw[kStages][16] = {};               // the value the game asked for
        bool g_seen[kStages][16] = {};
        int g_appliedAniso = 1; float g_appliedBias = 0.0f;

        DWORD Bits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

        DWORD Xform(DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value)
        {
            if (anisotropy > 1 && stage < kAnisoStages && (type == D3DSAMP_MINFILTER || type == D3DSAMP_MAGFILTER) && value == D3DTEXF_LINEAR)
                return D3DTEXF_ANISOTROPIC;
            return value;
        }

        // Sets the extra states that go with the filter change of one stage.
        void ApplyExtras(IDirect3DDevice9* dev, DWORD stage)
        {
            if (stage >= kAnisoStages) return;
            if (anisotropy > 1) oSetSamplerState(dev, stage, D3DSAMP_MAXANISOTROPY, static_cast<DWORD>(anisotropy));
            if (lodBias != 0.0f || g_appliedBias != 0.0f)
            {
                float engineBias = 0.0f;
                if (g_seen[stage][D3DSAMP_MIPMAPLODBIAS]) memcpy(&engineBias, &g_raw[stage][D3DSAMP_MIPMAPLODBIAS], 4);
                oSetSamplerState(dev, stage, D3DSAMP_MIPMAPLODBIAS, Bits(engineBias + lodBias));
            }
        }

        HRESULT __stdcall hkSetSamplerState(IDirect3DDevice9* self, DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value)
        {
            if (stage < kStages && type < 16) { g_raw[stage][type] = value; g_seen[stage][type] = true; }
            if (stage < kAnisoStages && type == D3DSAMP_MIPMAPLODBIAS && lodBias != 0.0f)
            {
                float f; memcpy(&f, &value, 4);
                return oSetSamplerState(self, stage, type, Bits(f + lodBias));
            }
            const HRESULT hr = oSetSamplerState(self, stage, type, Xform(stage, type, value));
            if (type == D3DSAMP_MINFILTER && stage < kAnisoStages) ApplyExtras(self, stage);
            return hr;
        }
    }

    void Install(void** vTable)
    {
        oSetSamplerState = reinterpret_cast<SetSamplerState_t>(vTable[69]);
        DWORD old;
        VirtualProtect(&vTable[69], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vTable[69] = reinterpret_cast<void*>(&hkSetSamplerState);
        VirtualProtect(&vTable[69], sizeof(void*), old, &old);
    }

    void OnEndScene(IDirect3DDevice9* dev)
    {
        if (!oSetSamplerState) return;
        if (anisotropy == g_appliedAniso && lodBias == g_appliedBias) return;
        // A setting changed: re-issue the last known state of every stage so the change shows without waiting for the game
        // to touch each sampler again.
        for (DWORD s = 0; s < kAnisoStages; ++s)
        {
            for (int k = 0; k < 2; ++k)
            {
                const D3DSAMPLERSTATETYPE t = k == 0 ? D3DSAMP_MINFILTER : D3DSAMP_MAGFILTER;
                if (g_seen[s][t]) oSetSamplerState(dev, s, t, Xform(s, t, g_raw[s][t]));
            }
            if (anisotropy > 1) oSetSamplerState(dev, s, D3DSAMP_MAXANISOTROPY, static_cast<DWORD>(anisotropy));
            float engineBias = 0.0f;
            if (g_seen[s][D3DSAMP_MIPMAPLODBIAS]) memcpy(&engineBias, &g_raw[s][D3DSAMP_MIPMAPLODBIAS], 4);
            oSetSamplerState(dev, s, D3DSAMP_MIPMAPLODBIAS, Bits(engineBias + lodBias));
        }
        g_appliedAniso = anisotropy; g_appliedBias = lodBias;
        LogF("GfxBoost: anisotropy %d, LOD bias %.2f applied", anisotropy, lodBias);
    }
}

#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "PostFx.h"
#include "D3DHook.h"
#include "Log.h"

namespace PostFx
{
    // Defaults = the values the user settled on while testing in game (2026-09-21).
    bool enabled = true;
    float fxaa = 1.0f;
    float sharpen = 0.35f;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float gamma = 1.0f;
    float aoStrength = 1.0f;
    float aoRadius = 77.0f;
    float aoBias = 0.0f;
    float aoMaxDistance = 30000.0f;
    float fogStrength = 1.0f;       // distance haze
    float fogStart = 215.0f;        // metres
    float fogDistance = 483.0f;     // metres over which the haze builds up to ~63 %
    float fogR = 154.0f / 255.0f, fogG = 198.0f / 255.0f, fogB = 226.0f / 255.0f;
    float dofAmount = 2.0f;         // depth of field: maximum blur radius in pixels, 0 = off
    float dofFocus = 112.0f;        // metres
    float dofZone = 100.0f;         // metres around the focus that stay sharp
    float dofTransition = 200.0f;   // metres until the blur is at its maximum
    bool dofAuto = false;           // focus on what is in the middle of the screen (off: the manual focus distance)
    bool debugDepth = false;
    bool debugAO = false;
    const char* status = "off";
    char depthStatus[160] = "no depth surface yet";
    volatile long wantMatrix = 0;

    namespace
    {
        constexpr D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

        // ---- shaders -----------------------------------------------------------------------------------------------
        const char* kCommon = R"(
sampler2D scene : register(s0);
sampler2D depthTex : register(s1);
sampler2D aoTex : register(s2);
float4 p0 : register(c0);   // x,y = 1/width, 1/height; z = fxaa; w = sharpen
float4 p1 : register(c1);   // x = saturation, y = contrast, z = 1/gamma, w = debug (1 depth, 2 occlusion)
float4 p2 : register(c2);   // x = ao strength, y = ao radius (world units), z = ao bias, w = ao max distance
float4 p3 : register(c3);   // x,y = width, height
float4 cam : register(c4);  // x = A, y = B  (depth = A + B / viewDepth), z = focal x, w = focal y  (NDC = focal * view / viewDepth)
float4 p5 : register(c5);   // x = fog strength, y = fog start (cm), z = 1 / fog distance (1/cm), w = blur radius of the depth of field (px)
float4 p6 : register(c6);   // x = focus distance (cm), y = in-focus half range (cm), z = transition length (cm), w = 1 when the focus follows the screen centre
float4 p7 : register(c7);   // xyz = fog colour

float LinW(float2 uv)
{
    float z = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    float d = z - cam.x;
    return abs(d) < 1e-7 ? 1e9 : cam.y / d;
}
float3 VP(float2 uv, float w) { return float3((uv.x * 2.0 - 1.0) / cam.z * w, (1.0 - uv.y * 2.0) / cam.w * w, w); }
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
)";

        const char* kAO = R"(
float Noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = LinW(uv);
    if (w0 <= 0.0 || w0 > p2.w) return float4(1, 1, 1, 1);
    float2 px = p0.xy;
    float3 pc = VP(uv, w0);
    float wr = LinW(uv + float2(px.x, 0)), wl = LinW(uv - float2(px.x, 0));
    float wu = LinW(uv - float2(0, px.y)), wd = LinW(uv + float2(0, px.y));
    float3 dx = abs(wr - w0) < abs(w0 - wl) ? VP(uv + float2(px.x, 0), wr) - pc : pc - VP(uv - float2(px.x, 0), wl);
    float3 dy = abs(wu - w0) < abs(w0 - wd) ? VP(uv - float2(0, px.y), wu) - pc : pc - VP(uv + float2(0, px.y), wd);
    float3 n = normalize(cross(dy, dx));
    if (n.z > 0.0) n = -n;

    float R = p2.y;
    float radiusPx = clamp(R * cam.w * 0.5 * p3.y / w0, 3.0, 160.0);
    float rot = Noise(uv * p3.xy) * 6.2831853;
    float occ = 0.0;
    [unroll] for (int i = 0; i < 10; i++)
    {
        float t = (i + 0.5) / 10.0;
        float a = rot + i * 2.3999632;
        float r = radiusPx * (0.2 + 0.8 * t);
        float2 suv = uv + float2(cos(a), sin(a)) * r * px;
        float3 v = VP(suv, LinW(suv)) - pc;
        float d = length(v);
        float ndv = dot(n, v) / max(d, 1e-3);
        occ += saturate(ndv - p2.z) * saturate(1.0 - d / R);
    }
    occ = occ / 10.0 * 2.2 * (1.0 - saturate(w0 / p2.w));
    float ao = saturate(1.0 - occ);
    return float4(ao, ao, ao, 1);
}
)";

        const char* kBlur = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = LinW(uv);
    float sum = 0.0, ws = 0.0;
    [unroll] for (int y = -2; y <= 2; y++)
    [unroll] for (int x = -2; x <= 2; x++)
    {
        float2 o = uv + float2(x, y) * p0.xy;
        float a = tex2Dlod(aoTex, float4(o, 0, 0)).r;
        float wgt = saturate(1.0 - abs(LinW(o) - w0) / (w0 * 0.04 + 4.0));
        sum += a * wgt; ws += wgt;
    }
    float v = sum / max(ws, 1e-3);
    return float4(v, v, v, 1);
}
)";

        // Everything that needs the depth, in one pass: ambient occlusion is multiplied in, then depth of field, then distance haze.
        const char* kEffects = R"(
float3 Col(float2 uv)
{
    float3 c = tex2Dlod(scene, float4(uv, 0, 0)).rgb;
    if (p2.x > 0.001) c *= lerp(1.0, tex2Dlod(aoTex, float4(uv, 0, 0)).r, saturate(p2.x));
    return c;
}
float Dist(float2 uv)                         // distance in cm; the sky counts as very far
{
    float w = LinW(uv);
    return (w > 0.0 && w < 1e8) ? w : 1e7;
}
float Coc(float w, float focus)               // blur radius in pixels
{
    float d = abs(w - focus);
    return saturate((d - p6.y) / max(p6.z, 1.0)) * p5.w;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = Dist(uv);
    float3 c = Col(uv);

    if (p5.w > 0.05)
    {
        float focus = p6.x;
        if (p6.w > 0.5)
        {
            float f = Dist(float2(0.5, 0.5));
            focus = (f > 1e6) ? 20000.0 : f;
        }
        float cc = Coc(w0, focus);
        if (cc > 0.3)
        {
            float3 sum = c; float wsum = 1.0;
            [loop] for (int i = 0; i < 16; i++)
            {
                float r = sqrt((i + 0.5) / 16.0) * cc;
                float a = i * 2.3999632;
                float2 suv = uv + float2(cos(a), sin(a)) * r * p0.xy;
                float sw = Dist(suv);
                float sc = Coc(sw, focus);
                // a tap only counts if its own blur reaches this pixel, and a sharp nearer object does not bleed into a far one
                float wt = saturate(sc - r + 1.0) * ((sw < w0 * 0.85) ? saturate(sc / max(cc, 0.001)) : 1.0);
                sum += Col(suv) * wt; wsum += wt;
            }
            c = sum / wsum;
        }
    }

    if (p5.x > 0.001 && w0 < 1e6)
    {
        float amount = 1.0 - exp(-max(w0 - p5.y, 0.0) * p5.z);
        amount *= 1.0 - saturate((w0 - 150000.0) / 150000.0);    // whatever is further than 1.5-3 km is sky / backdrop geometry: no haze on it
        c = lerp(c, p7.xyz, saturate(amount * p5.x));
    }
    return float4(c, 1);
}
)";

        const char* kFinal = R"(
float3 S(float2 uv)
{
    return tex2Dlod(scene, float4(uv, 0, 0)).rgb;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    if (p1.w > 1.5) { float a = tex2Dlod(aoTex, float4(uv, 0, 0)).r; return float4(a, a, a, 1); }
    if (p1.w > 0.5)
    {
        // linear distance from the camera as grey (near dark, far bright, sky white), with faint contour lines every 5 m
        float w = LinW(uv);
        float g = (w > 0.0 && w < 1e8) ? saturate(sqrt(w / p2.w)) : 1.0;
        float c = (w > 0.0 && w < 1e8) ? 0.12 * step(0.9, frac(w / 500.0)) : 0.0;
        return float4(g - c, g - c, g, 1);
    }

    float2 px = p0.xy;
    float3 rgbM = S(uv);
    float3 result = rgbM;

    // ---- FXAA (Lottes' public FXAA 2 kernel) ----
    if (p0.z > 0.001)
    {
        float3 rgbNW = S(uv + float2(-1, -1) * px), rgbNE = S(uv + float2(1, -1) * px);
        float3 rgbSW = S(uv + float2(-1, 1) * px),  rgbSE = S(uv + float2(1, 1) * px);
        float lumaNW = Luma(rgbNW), lumaNE = Luma(rgbNE), lumaSW = Luma(rgbSW), lumaSE = Luma(rgbSE), lumaM = Luma(rgbM);
        float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
        float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
        float2 dir;
        dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
        dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
        float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
        float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
        dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;
        float3 rgbA = 0.5 * (S(uv + dir * (1.0 / 3.0 - 0.5)) + S(uv + dir * (2.0 / 3.0 - 0.5)));
        float3 rgbB = rgbA * 0.5 + 0.25 * (S(uv + dir * -0.5) + S(uv + dir * 0.5));
        float lumaB = Luma(rgbB);
        float3 aa = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
        result = lerp(rgbM, aa, saturate(p0.z));
    }

    // ---- sharpening: unsharp mask limited to the local range so it cannot ring ----
    if (p0.w > 0.001)
    {
        float3 n = S(uv + float2(0, -1) * px), s = S(uv + float2(0, 1) * px), w = S(uv + float2(-1, 0) * px), e = S(uv + float2(1, 0) * px);
        float3 blur = (n + s + w + e) * 0.25;
        float3 lo = min(min(n, s), min(w, e)), hi = max(max(n, s), max(w, e));
        float3 sharp = result + (result - blur) * p0.w * 2.0;
        result = clamp(sharp, min(lo, result) - 0.03, max(hi, result) + 0.03);
    }

    // ---- colour grading ----
    float l = Luma(result);
    result = lerp(float3(l, l, l), result, p1.x);
    result = (result - 0.5) * p1.y + 0.5;
    result = pow(saturate(result), p1.z);
    return float4(result, 1);
}
)";

        // Copies the INTZ depth (raw values) into an R32F render target. The scene's depth is saved this way right before the
        // game clears it / switches away from it: at the end of the frame the depth buffer itself no longer holds the scene.
        const char* kDepthCopy = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float z = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    return float4(z, z, z, 1);
}
)";

        // ---- device objects ----------------------------------------------------------------------------------------
        IDirect3DTexture9* g_copy = nullptr; IDirect3DSurface9* g_copySurface = nullptr;
        IDirect3DTexture9* g_aoA = nullptr; IDirect3DSurface9* g_aoASurface = nullptr;
        IDirect3DTexture9* g_aoB = nullptr; IDirect3DSurface9* g_aoBSurface = nullptr;
        IDirect3DTexture9* g_work = nullptr; IDirect3DSurface9* g_workSurface = nullptr;   // result of the depth effects
        IDirect3DPixelShader9 *g_psFinal = nullptr, *g_psAO = nullptr, *g_psBlur = nullptr, *g_psDepthCopy = nullptr, *g_psEffects = nullptr;
        IDirect3DStateBlock9* g_state = nullptr;
        IDirect3DStateBlock9* g_state2 = nullptr;           // for the depth save that runs in the middle of the game's frame
        IDirect3DTexture9* g_depthCopy = nullptr; IDirect3DSurface9* g_depthCopySurface = nullptr;
        UINT g_depthW = 0, g_depthH = 0;
        float g_savedCam[4] = {}; bool g_savedValid = false; int g_saves = 0;
        UINT g_w = 0, g_h = 0; D3DFORMAT g_fmt = D3DFMT_UNKNOWN;
        bool g_compileFailed = false;

        // ---- depth surfaces ----------------------------------------------------------------------------------------
        typedef HRESULT(__stdcall* CreateDepthStencilSurface_t)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
        typedef HRESULT(__stdcall* SetDepthStencilSurface_t)(IDirect3DDevice9*, IDirect3DSurface9*);
        CreateDepthStencilSurface_t oCreateDS = nullptr;
        SetDepthStencilSurface_t oSetDS = nullptr;
        // Every INTZ surface the game binds as depth-stencil gets a slot with the number of draw calls made with it this frame;
        // the scene depth is the one with the most draws (the game has more than one big depth surface: last-bound is not enough).
        struct Slot { IDirect3DSurface9* surf; int draws; int saved; float cam[4]; bool camValid; };
        constexpr int kSlots = 8;
        Slot g_slots[kSlots] = {};
        int g_curSlot = -1;
        IDirect3DSurface9* g_scene = nullptr;      // the chosen scene depth (a slot's surface, no extra reference)
        int g_intzCreated = 0, g_intzFailed = 0;
        float g_cam[4] = {}; bool g_camValid = false;
        int g_sceneDraws = 0;

        HRESULT __stdcall hkCreateDS(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL discard, IDirect3DSurface9** out, HANDLE* handle)
        {
            // only the scene's D24S8 depth (2560x1440 here); the shadow map's own depth (2048x2048 D24X8) stays as it is
            if (out && fmt == D3DFMT_D24S8 && ms == D3DMULTISAMPLE_NONE && w >= 1280 && h >= 720 && w != h)
            {
                IDirect3DTexture9* tex = nullptr;
                if (SUCCEEDED(self->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &tex, nullptr)) && tex)
                {
                    IDirect3DSurface9* surf = nullptr;
                    if (SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf)
                    {
                        tex->Release();       // the surface keeps its texture alive
                        *out = surf;
                        ++g_intzCreated;
                        if (g_intzCreated <= 4) LogF("PostFx: depth-stencil surface %ux%u created as INTZ texture", w, h);
                        return D3D_OK;
                    }
                    tex->Release();
                }
                ++g_intzFailed;
                if (g_intzFailed <= 2) LogF("PostFx: INTZ depth texture %ux%u not available, using the plain surface", w, h);
            }
            return oCreateDS(self, w, h, fmt, ms, msq, discard, out, handle);
        }

        typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        IDirect3DPixelShader9* Compile(IDirect3DDevice9* dev, D3DCompile_t compile, const char* body, const char* name);
        bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DTexture9** tex, IDirect3DSurface9** surf);
        struct Vtx { float x, y, z, rhw, u, v; };
        void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h);
        template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

        bool DepthWanted() { return enabled && (aoStrength > 0.001f || debugAO || debugDepth); }

        // Renders the raw INTZ depth of `slot` into g_depthCopy. Runs inside the game's own frame (when it clears the depth or
        // binds another depth surface), so every render state is saved / restored around it.
        void SaveDepth(IDirect3DDevice9* dev, int slot)
        {
            if (slot < 0 || !g_slots[slot].surf) return;
            IDirect3DSurface9* src = g_slots[slot].surf;
            D3DSURFACE_DESC sd{};
            if (FAILED(src->GetDesc(&sd))) return;
            IDirect3DTexture9* srcTex = nullptr;
            if (FAILED(src->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&srcTex))) || !srcTex) return;

            if (!g_psDepthCopy)
            {
                HMODULE mod = LoadLibraryA("d3dcompiler_47.dll");
                D3DCompile_t compile = mod ? reinterpret_cast<D3DCompile_t>(GetProcAddress(mod, "D3DCompile")) : nullptr;
                if (compile) g_psDepthCopy = Compile(dev, compile, kDepthCopy, "postfx_depthcopy");
                if (!g_psDepthCopy) { srcTex->Release(); return; }
            }
            if (g_depthCopy && (g_depthW != sd.Width || g_depthH != sd.Height)) { SafeRelease(g_depthCopySurface); SafeRelease(g_depthCopy); }
            if (!g_depthCopy)
            {
                if (!MakeTarget(dev, sd.Width, sd.Height, D3DFMT_R32F, &g_depthCopy, &g_depthCopySurface)) { srcTex->Release(); return; }
                g_depthW = sd.Width; g_depthH = sd.Height;
            }
            if (!g_state2 && (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state2)) || !g_state2)) { srcTex->Release(); return; }

            IDirect3DSurface9* prevRT = nullptr; IDirect3DSurface9* prevDS = nullptr;
            dev->GetRenderTarget(0, &prevRT); dev->GetDepthStencilSurface(&prevDS);
            g_state2->Capture();

            oSetDS(dev, nullptr);                       // the INTZ surface must not be bound while it is sampled
            dev->SetRenderTarget(0, g_depthCopySurface);
            D3DVIEWPORT9 vp = { 0, 0, sd.Width, sd.Height, 0.0f, 1.0f };
            dev->SetViewport(&vp);
            dev->SetVertexShader(nullptr); dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            dev->SetPixelShader(g_psDepthCopy);
            dev->SetTexture(1, srcTex);
            dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT); dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            DrawQuad(dev, sd.Width, sd.Height);

            dev->SetTexture(1, nullptr);
            dev->SetRenderTarget(0, prevRT);
            oSetDS(dev, prevDS);
            g_state2->Apply();
            SafeRelease(prevRT); SafeRelease(prevDS); srcTex->Release();

            g_slots[slot].saved = g_slots[slot].draws;
            if (g_slots[slot].camValid) { memcpy(g_savedCam, g_slots[slot].cam, sizeof(g_savedCam)); g_savedValid = true; }
            if (++g_saves <= 3) LogF("PostFx: depth saved (%d draws on the surface, projection %s)", g_slots[slot].draws, g_slots[slot].camValid ? "ok" : "unknown");
        }

        typedef HRESULT(__stdcall* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
        Clear_t oClear = nullptr;
        HRESULT __stdcall hkClear(IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
        {
            if ((flags & D3DCLEAR_ZBUFFER) && g_curSlot >= 0 && DepthWanted() && g_slots[g_curSlot].draws - g_slots[g_curSlot].saved >= 20)
                SaveDepth(self, g_curSlot);      // the scene is about to be wiped
            return oClear(self, count, rects, flags, color, z, stencil);
        }

        HRESULT __stdcall hkSetDS(IDirect3DDevice9* self, IDirect3DSurface9* surf)
        {
            const int leaving = g_curSlot;
            const HRESULT hr = oSetDS(self, surf);
            if (leaving >= 0 && surf != g_slots[leaving].surf && DepthWanted() && g_slots[leaving].draws - g_slots[leaving].saved >= 20)
                SaveDepth(self, leaving);        // the game moves on to another depth surface (or none): keep what the scene wrote
            g_curSlot = -1;
            if (SUCCEEDED(hr) && surf)
            {
                D3DSURFACE_DESC d{};
                if (SUCCEEDED(surf->GetDesc(&d)) && d.Format == kINTZ && d.Width >= 1280 && d.Width != d.Height)
                {
                    int slot = -1, freeSlot = -1;
                    for (int i = 0; i < kSlots; ++i)
                    {
                        if (g_slots[i].surf == surf) slot = i;
                        else if (!g_slots[i].surf && freeSlot < 0) freeSlot = i;
                    }
                    if (slot < 0 && freeSlot >= 0) { slot = freeSlot; g_slots[slot].surf = surf; surf->AddRef(); g_slots[slot].draws = 0; g_slots[slot].camValid = false; }
                    g_curSlot = slot;
                    if (slot >= 0) InterlockedExchange(&wantMatrix, 1);
                }
            }
            return hr;
        }


        IDirect3DPixelShader9* Compile(IDirect3DDevice9* dev, D3DCompile_t compile, const char* body, const char* name)
        {
            char* src = new char[strlen(kCommon) + strlen(body) + 2];
            strcpy_s(src, strlen(kCommon) + strlen(body) + 2, kCommon);
            strcat_s(src, strlen(kCommon) + strlen(body) + 2, body);
            ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
            HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err);
            delete[] src;
            IDirect3DPixelShader9* ps = nullptr;
            if (FAILED(hr) || !code) LogF("PostFx: shader '%s' compile failed: %s", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            else if (FAILED(dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps))) { LogF("PostFx: CreatePixelShader '%s' failed", name); ps = nullptr; }
            if (code) code->Release();
            if (err) err->Release();
            return ps;
        }

        bool CompileShaders(IDirect3DDevice9* dev)
        {
            HMODULE mod = LoadLibraryA("d3dcompiler_47.dll");
            if (!mod) { LogF("PostFx: d3dcompiler_47.dll not found"); return false; }
            D3DCompile_t compile = reinterpret_cast<D3DCompile_t>(GetProcAddress(mod, "D3DCompile"));
            if (!compile) return false;
            g_psFinal = Compile(dev, compile, kFinal, "postfx_final");
            g_psEffects = Compile(dev, compile, kEffects, "postfx_effects");
            g_psAO = Compile(dev, compile, kAO, "postfx_ao");
            g_psBlur = Compile(dev, compile, kBlur, "postfx_blur");
            LogF("PostFx: shaders compiled (final %d, ao %d, blur %d)", g_psFinal ? 1 : 0, g_psAO ? 1 : 0, g_psBlur ? 1 : 0);
            return g_psFinal != nullptr;
        }

        bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DTexture9** tex, IDirect3DSurface9** surf)
        {
            if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, tex, nullptr)) || !*tex) return false;
            (*tex)->GetSurfaceLevel(0, surf);
            return *surf != nullptr;
        }

        bool EnsureResources(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d, bool needAO, bool needWork)
        {
            if (g_compileFailed) return false;
            if (!g_psFinal && !CompileShaders(dev)) { g_compileFailed = true; status = "shader failed"; return false; }
            if (g_copy && (g_w != d.Width || g_h != d.Height || g_fmt != d.Format)) ReleaseDeviceObjects();
            if (!g_copy)
            {
                if (!MakeTarget(dev, d.Width, d.Height, d.Format, &g_copy, &g_copySurface)) { status = "cannot create the copy texture"; return false; }
                g_w = d.Width; g_h = d.Height; g_fmt = d.Format;
            }
            if (needAO && !g_aoA)
            {
                if (!MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_aoA, &g_aoASurface) || !MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_aoB, &g_aoBSurface))
                { status = "cannot create the occlusion targets"; return false; }
            }
            if (needWork && !g_work && !MakeTarget(dev, d.Width, d.Height, d.Format, &g_work, &g_workSurface)) { status = "cannot create the effects target"; return false; }
            return true;
        }

        void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h)
        {
            const float fw = static_cast<float>(w) - 0.5f, fh = static_cast<float>(h) - 0.5f;
            const Vtx quad[4] = { { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f }, { fw, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
                                  { -0.5f, fh, 0.0f, 1.0f, 0.0f, 1.0f },     { fw, fh, 0.0f, 1.0f, 1.0f, 1.0f } };
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));
        }

        void SetSampler(IDirect3DDevice9* dev, DWORD s, D3DTEXTUREFILTERTYPE filter)
        {
            dev->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
            dev->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
            dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, 0);
        }
    }

    void InstallHooks(void** vTable)
    {
        DWORD old;
        oCreateDS = reinterpret_cast<CreateDepthStencilSurface_t>(vTable[29]);
        VirtualProtect(&vTable[29], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vTable[29] = reinterpret_cast<void*>(&hkCreateDS);
        VirtualProtect(&vTable[29], sizeof(void*), old, &old);
        oSetDS = reinterpret_cast<SetDepthStencilSurface_t>(vTable[39]);
        VirtualProtect(&vTable[39], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vTable[39] = reinterpret_cast<void*>(&hkSetDS);
        VirtualProtect(&vTable[39], sizeof(void*), old, &old);
        oClear = reinterpret_cast<Clear_t>(vTable[43]);
        VirtualProtect(&vTable[43], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vTable[43] = reinterpret_cast<void*>(&hkClear);
        VirtualProtect(&vTable[43], sizeof(void*), old, &old);
    }

    // The projection behind the scene: c240 is View*Projection (row vectors). For a standard perspective projection
    //   depth = A + B / viewDepth,  A = M[0][2] / M[0][3],  B = M[3][2] - A * M[3][3],
    // and the focal lengths are the lengths of the x / y columns' rotation part.
    void CaptureMatrix()
    {
        InterlockedExchange(&wantMatrix, 0);
        float m[16];
        if (!GetCandidateViewProjMatrix(m)) return;
        auto M = [&](int r, int c) { return m[r * 4 + c]; };
        int row = -1;
        for (int r = 0; r < 3; ++r) if (std::fabs(M(r, 3)) > 1e-6f) { row = r; break; }
        if (row < 0) return;
        const float A = M(row, 2) / M(row, 3);
        const float B = M(3, 2) - A * M(3, 3);
        const float fx = std::sqrt(M(0, 0) * M(0, 0) + M(1, 0) * M(1, 0) + M(2, 0) * M(2, 0));
        const float fy = std::sqrt(M(0, 1) * M(0, 1) + M(1, 1) * M(1, 1) + M(2, 1) * M(2, 1));
        if (g_curSlot < 0) return;
        Slot& s = g_slots[g_curSlot];
        if (!(std::fabs(A) < 4.0f) || std::fabs(B) < 1e-6f || fx < 0.05f || fy < 0.05f || fx > 20.0f || fy > 20.0f) { s.camValid = false; return; }
        s.cam[0] = A; s.cam[1] = B; s.cam[2] = fx; s.cam[3] = fy; s.camValid = true;
    }

    void OnDraw()
    {
        if (wantMatrix) CaptureMatrix();
        if (g_curSlot >= 0) ++g_slots[g_curSlot].draws;
    }

    void ReleaseDeviceObjects()
    {
        for (Slot& s : g_slots) { if (s.surf) s.surf->Release(); s = Slot(); }
        g_curSlot = -1; g_scene = nullptr; g_camValid = false;
        SafeRelease(g_copySurface); SafeRelease(g_copy);
        SafeRelease(g_aoASurface); SafeRelease(g_aoA);
        SafeRelease(g_aoBSurface); SafeRelease(g_aoB);
        SafeRelease(g_workSurface); SafeRelease(g_work);
        SafeRelease(g_state); SafeRelease(g_state2);
        SafeRelease(g_depthCopySurface); SafeRelease(g_depthCopy); g_depthW = g_depthH = 0; g_savedValid = false;
        g_w = g_h = 0;
    }

    void Apply(IDirect3DDevice9* dev)
    {
        // The scene depth = the INTZ surface with the most draw calls this frame; its projection comes with it.
        {
            int best = -1;
            for (int i = 0; i < kSlots; ++i) if (g_slots[i].surf && (best < 0 || g_slots[i].draws > g_slots[best].draws)) best = i;
            if (best >= 0 && g_slots[best].draws > 0)
            {
                g_scene = g_slots[best].surf; g_sceneDraws = g_slots[best].draws;
                g_camValid = g_slots[best].camValid;
                if (g_camValid) memcpy(g_cam, g_slots[best].cam, sizeof(g_cam));
            }
            for (Slot& s : g_slots) { s.draws = 0; s.saved = 0; }
        }
        // depth status for the menu (also shown while the effects are off, to check that the depth surface exists)
        {
            D3DSURFACE_DESC sd{};
            if (g_scene && SUCCEEDED(g_scene->GetDesc(&sd)))
                snprintf(depthStatus, sizeof(depthStatus), "scene depth %ux%u (%d draws; %d INTZ surfaces made), projection %s: A=%.5f B=%.3f focal=%.3f/%.3f",
                         sd.Width, sd.Height, g_sceneDraws, g_intzCreated, g_camValid ? "ok" : "unknown", g_cam[0], g_cam[1], g_cam[2], g_cam[3]);
            else
                snprintf(depthStatus, sizeof(depthStatus), "no scene depth surface (INTZ surfaces made: %d, failed: %d)", g_intzCreated, g_intzFailed);
        }

        const bool grading = saturation != 1.0f || contrast != 1.0f || gamma != 1.0f;
        const bool depthReady = g_depthCopy && g_savedValid;
        const bool aoOn = depthReady && (aoStrength > 0.001f || debugAO || debugDepth);     // the occlusion passes
        const bool fxOn = depthReady && (aoStrength > 0.001f || fogStrength > 0.001f || dofAmount > 0.05f);   // the depth-effects pass
        if (!enabled || (fxaa <= 0.001f && sharpen <= 0.001f && !grading && !aoOn && !fxOn)) { status = "off"; return; }

        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
        D3DSURFACE_DESC desc{}; bb->GetDesc(&desc);
        if (!EnsureResources(dev, desc, aoOn, fxOn) || (aoOn && (!g_psAO || !g_psBlur)) || (fxOn && !g_psEffects))
        {
            bb->Release();
            if (aoOn && !g_psAO) status = "occlusion shader failed"; else if (fxOn && !g_psEffects) status = "effects shader failed";
            return;
        }

        if (!g_state && (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state)) || !g_state)) { bb->Release(); status = "no state block"; return; }
        g_state->Capture();

        if (FAILED(dev->StretchRect(bb, nullptr, g_copySurface, nullptr, D3DTEXF_NONE))) { status = "StretchRect failed"; bb->Release(); return; }

        IDirect3DTexture9* depthTex = aoOn ? g_depthCopy : nullptr;      // the scene depth saved during the frame (R32F)
        const bool haveDepthTex = aoOn && depthTex != nullptr;

        D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        for (DWORD s = 3; s < 8; ++s) dev->SetTexture(s, nullptr);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);

        const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);
        const float c0[4] = { 1.0f / w, 1.0f / h, fxaa, sharpen };
        const float c1[4] = { saturation, contrast, 1.0f / (gamma > 0.05f ? gamma : 1.0f), debugAO ? 2.0f : (debugDepth ? 1.0f : 0.0f) };
        const float c2[4] = { haveDepthTex ? aoStrength : 0.0f, aoRadius, aoBias, aoMaxDistance };
        const float c3[4] = { w, h, 0.0f, 0.0f };
        dev->SetPixelShaderConstantF(0, c0, 1); dev->SetPixelShaderConstantF(1, c1, 1);
        dev->SetPixelShaderConstantF(2, c2, 1); dev->SetPixelShaderConstantF(3, c3, 1);
        dev->SetPixelShaderConstantF(4, g_savedCam, 1);
        const bool wantFx = fxOn && g_depthCopy != nullptr;
        const float c5[4] = { fogStrength, fogStart * 100.0f, 1.0f / (fogDistance * 100.0f > 1.0f ? fogDistance * 100.0f : 1.0f), dofAmount };
        const float c6[4] = { dofFocus * 100.0f, dofZone * 100.0f, dofTransition * 100.0f, dofAuto ? 1.0f : 0.0f };
        const float c7[4] = { fogR, fogG, fogB, 0.0f };
        dev->SetPixelShaderConstantF(5, c5, 1); dev->SetPixelShaderConstantF(6, c6, 1); dev->SetPixelShaderConstantF(7, c7, 1);

        if (haveDepthTex)
        {
            dev->SetTexture(1, depthTex); SetSampler(dev, 1, D3DTEXF_POINT);
            // pass 1: raw occlusion
            dev->SetRenderTarget(0, g_aoASurface);
            dev->SetPixelShader(g_psAO);
            dev->SetTexture(0, g_copy); SetSampler(dev, 0, D3DTEXF_POINT);
            dev->SetTexture(2, nullptr);
            DrawQuad(dev, desc.Width, desc.Height);
            // pass 2: depth-aware blur
            dev->SetRenderTarget(0, g_aoBSurface);
            dev->SetPixelShader(g_psBlur);
            dev->SetTexture(2, g_aoA); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);
            dev->SetRenderTarget(0, bb);
            dev->SetTexture(2, g_aoB); SetSampler(dev, 2, D3DTEXF_LINEAR);
        }
        else
        {
            dev->SetTexture(1, nullptr); dev->SetTexture(2, nullptr);
        }

        // depth effects (occlusion, depth of field, haze) into the work texture
        IDirect3DTexture9* finalSource = g_copy;
        if (wantFx && g_work)
        {
            dev->SetTexture(1, depthTex ? depthTex : g_depthCopy); SetSampler(dev, 1, D3DTEXF_POINT);
            dev->SetRenderTarget(0, g_workSurface);
            dev->SetPixelShader(g_psEffects);
            dev->SetTexture(0, g_copy); SetSampler(dev, 0, D3DTEXF_LINEAR);
            dev->SetTexture(2, aoOn ? g_aoB : nullptr); if (aoOn) SetSampler(dev, 2, D3DTEXF_LINEAR);
            DrawQuad(dev, desc.Width, desc.Height);
            finalSource = g_work;
        }

        // final pass over the back buffer
        dev->SetRenderTarget(0, bb);
        dev->SetPixelShader(g_psFinal);
        dev->SetTexture(0, finalSource); SetSampler(dev, 0, D3DTEXF_LINEAR);
        // (the game's scene is still open: this runs inside the EndScene hook, so no Begin/EndScene here)
        DrawQuad(dev, desc.Width, desc.Height);

        dev->SetTexture(0, nullptr); dev->SetTexture(1, nullptr); dev->SetTexture(2, nullptr);
        g_state->Apply();
        bb->Release();
        status = haveDepthTex ? "active (with depth)" : "active";
    }
}

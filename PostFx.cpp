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
    // Defaults = the values the user settled on while testing in game (2026-09-21; AO retuned 2026-09-24 after the
    // horizontal+vertical separable blur replaced the old single 5x5 pass - the wider, softer blur let the strength
    // come down from 1.0 without losing the effect, and a touch of bias/shorter max distance became usable once the
    // blur stopped exaggerating every low-poly facet seam into a dark smudge).
    bool enabled = true;
    float fxaa = 1.0f;
    float sharpen = 0.35f;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float gamma = 1.0f;
    float aoStrength = 0.45f;
    float aoRadius = 72.0f;
    float aoBias = 0.20f;
    float aoMaxDistance = 21146.0f;
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

    bool ssrEnabled = false;   // off by default - this feature is still beta
    float ssrStrength = 0.16f;
    float ssrStepCm = 200.0f;
    float ssrMaxDistanceCm = 9136.0f;
    float ssrGroundBias = 0.85f;

    bool fakeRTEnabled = false;   // off by default - beta
    float fakeRTStrength = 0.18f;
    float fakeRTRoughness = 0.87f;
    float fakeRTStepCm = 2.0f;
    float fakeRTMaxDistanceCm = 8000.0f;
    bool debugFakeRT = false;
    int fakeRTRayCount = 8;
    int fakeRTStepCount = 16;
    int fakeRTBounces = 3;
    bool fakeRTTemporal = false;
    float fakeRTTemporalWeight = 0.85f;

    namespace
    {
        constexpr D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

        // ---- shaders -----------------------------------------------------------------------------------------------
        const char* kCommon = R"(
sampler2D scene : register(s0);
sampler2D depthTex : register(s1);
sampler2D aoTex : register(s2);
sampler2D normalTex : register(s3);   // packed (n*0.5+0.5): SSR reads this instead of reconstructing its own normal,
                                       // so it can use one that has been smoothed across low-poly facet seams (see kNormalPack)
sampler2D aoRawTex : register(s4);        // the finished (blurred) SSAO occlusion buffer - kAddBounce scales the fake-RT
                                           // bounce light by it, the same way AO already darkens direct light, so an
                                           // occluded corner does not get "un-occluded" by the bounce term on top.
sampler2D historyTex : register(s5);      // previous frame's temporally-accumulated fake-RT bounce (see kTemporalBlend)
sampler2D historyDepthTex : register(s6); // previous frame's scene depth at the same pixels, for rejecting stale history
float4 p0 : register(c0);   // x,y = 1/width, 1/height; z = fxaa; w = sharpen
float4 p1 : register(c1);   // x = saturation, y = contrast, z = 1/gamma, w = debug (1 depth, 2 occlusion)
float4 p2 : register(c2);   // x = ao strength, y = ao radius (world units), z = ao bias, w = ao max distance
float4 p3 : register(c3);   // x,y = width, height
float4 cam : register(c4);  // x = A, y = B  (depth = A + B / viewDepth), z = focal x, w = focal y  (NDC = focal * view / viewDepth)
float4 p5 : register(c5);   // x = fog strength, y = fog start (cm), z = 1 / fog distance (1/cm), w = blur radius of the depth of field (px)
float4 p6 : register(c6);   // x = focus distance (cm), y = in-focus half range (cm), z = transition length (cm), w = 1 when the focus follows the screen centre
float4 p7 : register(c7);   // xyz = fog colour
float4 p8 : register(c8);   // x = depth-texture vertical offset (colour-space uv.y - x = where to sample the depth
                             // texture): a letterboxed cut-scene can leave the depth data anchored to the top of the
                             // buffer while the final, displayed colour picture has the same content centred with
                             // matching bars top and bottom - found live, 2026-09-24. 0 = no offset (normal gameplay).
float4 p9 : register(c9);   // x = SSR strength, y = SSR first step length (cm), z = SSR max distance (cm),
                             // w = SSR ground bias (0 = reflect everything the same, 1 = only near-horizontal)
float4 p10 : register(c10); // x = fake-RT strength, y = roughness (0 mirror .. 1 diffuse), z = first ray step (cm), w = max distance (cm)
float4 p11 : register(c11); // x = fake-RT rays per pixel, y = ray-march steps per ray, z = extra bounces per ray, w unused
float4 p12 : register(c12); // x = temporal history blend weight (0 = temporal reprojection off)

float LinW(float2 uv)
{
    float dy = uv.y - p8.x;
    if (dy < 0.0 || dy > 1.0) return 1e9;   // outside the depth texture's own data: CLAMP addressing would repeat
                                             // the edge row instead of correctly reporting "nothing here"
    float z = tex2Dlod(depthTex, float4(uv.x, dy, 0, 0)).r;
    float d = z - cam.x;
    return abs(d) < 1e-7 ? 1e9 : cam.y / d;
}
float3 VP(float2 uv, float w) { return float3((uv.x * 2.0 - 1.0) / cam.z * w, (1.0 - uv.y * 2.0) / cam.w * w, w); }
// Inverse of VP(): a view-space position (p.z = view depth) back to the screen uv it would rasterise to.
float2 ProjectUV(float3 p) { return float2((p.x * cam.z / p.z + 1.0) * 0.5, (1.0 - p.y * cam.w / p.z) * 0.5); }
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
// Screen-space normal from the depth buffer alone (no real normal data available) - picks whichever neighbour on
// each axis is the closer depth match, so it does not blend across a silhouette edge into the background.
float3 ReconstructNormal(float2 uv, float3 pc, float w0)
{
    float2 px = p0.xy;
    float wr = LinW(uv + float2(px.x, 0)), wl = LinW(uv - float2(px.x, 0));
    float wu = LinW(uv - float2(0, px.y)), wd = LinW(uv + float2(0, px.y));
    float3 dx = abs(wr - w0) < abs(w0 - wl) ? VP(uv + float2(px.x, 0), wr) - pc : pc - VP(uv - float2(px.x, 0), wl);
    float3 dy = abs(wu - w0) < abs(w0 - wd) ? VP(uv - float2(0, px.y), wu) - pc : pc - VP(uv + float2(0, px.y), wd);
    float3 n = normalize(cross(dy, dx));
    if (n.z > 0.0) n = -n;
    return n;
}
float Noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
// GGX-style importance sample: a direction inside a cone around centerDir, width set by roughness (0 = dir itself,
// 1 = full hemisphere around it) - the same formula real-time renderers use to pick a random reflection direction
// for a given roughness, reused here for the fake-RT pass (kFakeRT) to turn a single "roughness" slider into
// anything between a sharp mirror bounce and a diffuse-looking scatter.
float3 ConeSample(float3 centerDir, float roughness, float2 xi)
{
    float a = max(roughness * roughness, 0.001);
    float phi = 6.2831853 * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(saturate(1.0 - cosT * cosT));
    float3 h = float3(sinT * cos(phi), sinT * sin(phi), cosT);
    float3 up = abs(centerDir.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, centerDir));
    float3 bitangent = cross(centerDir, tangent);
    return normalize(tangent * h.x + bitangent * h.y + centerDir * h.z);
}
)";

        const char* kAO = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = LinW(uv);
    if (w0 <= 0.0 || w0 > p2.w) return float4(1, 1, 1, 1);
    float2 px = p0.xy;
    float3 pc = VP(uv, w0);
    float3 n = ReconstructNormal(uv, pc, w0);

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

        // Packs the raw, per-pixel reconstructed normal into a colour target so it can go through the SAME
        // horizontal+vertical blur as SSAO's occlusion (kBlur already blurs whatever is in aoTex's .rgb, weighted by
        // depth similarity - it does not care whether that is an AO value or a normal). Smoothing the normal itself,
        // not the reflected picture, is what actually removes the low-poly facet seams from SSR (found live,
        // 2026-09-25: reflections on a car showed its polygon mesh clearly). "no data" packs to (0,0,1) - pointing
        // straight at the camera - so it fades out rather than reflecting garbage.
        const char* kNormalPack = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = LinW(uv);
    if (w0 <= 0.0 || w0 > p9.z) return float4(0.5, 0.5, 1.0, 1);
    float3 n = ReconstructNormal(uv, VP(uv, w0), w0);
    return float4(n * 0.5 + 0.5, 1);
}
)";

        // Crude screen-space reflections (Debug tab, experimental, 2026-09-25): no material/roughness data exists to
        // tell a mirror from a brick wall, so this reflects every surface by the same amount, weighted only by a
        // Fresnel-style falloff (more reflective at grazing angles, like everything slightly wet would look) - a
        // rough first pass to check the ray marching itself lines up before chasing a real material signal.
        const char* kSSR = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 c = tex2Dlod(scene, float4(uv, 0, 0)).rgb;
    float w0 = LinW(uv);
    if (w0 <= 0.0 || w0 > p9.z) return float4(c, 1);
    float3 pc = VP(uv, w0);
    float3 n = normalize(tex2Dlod(normalTex, float4(uv, 0, 0)).rgb * 2.0 - 1.0);   // smoothed, see kNormalPack
    float3 viewDir = normalize(pc);
    float3 refl = reflect(viewDir, n);
    if (refl.z <= 0.0) return float4(c, 1);   // pointing back towards the camera: no forward ray to march

    // Dithering: without it every pixel's steps land at the exact same set of distances along its ray, so a curved
    // or angled surface samples a coarse, aliased subset of what is actually there - visible as banding/ripples
    // (seen live, 2026-09-25, worst on the curved awning). Starting each pixel's march at a different fraction of
    // the first step scrambles that into noise instead, which reads much softer even though the ray count is the
    // same - the standard fix for this without more steps.
    const float jitter = Noise(uv * p3.xy);
    float3 rayPos = pc;
    float stepLen = max(p9.y, 1.0);
    bool hit = false;
    float2 hitUV = uv;
    float3 prevPos = pc;
    [loop] for (int i = 0; i < 24; i++)
    {
        prevPos = rayPos;
        rayPos += refl * stepLen * (i == 0 ? (0.35 + 0.65 * jitter) : 1.0);
        stepLen *= 1.12;                       // a few fine steps close up, coarser further out
        if (rayPos.z <= 0.0 || rayPos.z > p9.z) break;
        float2 suv = ProjectUV(rayPos);
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
        float sceneW = LinW(suv);
        if (sceneW > 0.0 && sceneW < rayPos.z)
        {
            // binary refine between the last miss and this hit: 6 halvings gets much closer to the real surface
            // than the coarse step alone, without marching the whole way there one small step at a time
            float3 lo = prevPos, hi = rayPos;
            [unroll] for (int j = 0; j < 6; j++)
            {
                float3 mid = (lo + hi) * 0.5;
                float2 muv = ProjectUV(mid);
                float mw = LinW(muv);
                if (mw > 0.0 && mw < mid.z) hi = mid; else lo = mid;
            }
            hitUV = ProjectUV(hi);
            hit = true; break;
        }
    }
    if (!hit) return float4(c, 1);

    float3 reflCol = tex2Dlod(scene, float4(hitUV, 0, 0)).rgb;
    float fresnel = pow(saturate(1.0 + dot(n, viewDir)), 2.0);
    // Workaround for having no real material data (2026-09-25): weight by how close the normal is to "up/down in
    // view space" as a stand-in for "world-horizontal" - for a roughly level camera (this game's driving chase cam),
    // that is the road/floor, which is what usually looks wet/reflective anyway; a wall's normal sits mostly in the
    // view's left-right/forward plane instead, so it is not "ground-like" and gets pulled down. p9.w=0 disables this
    // (old behaviour: every surface reflects the same); p9.w=1 reflects close to nothing but near-horizontal ground.
    float groundLike = abs(n.y);
    float groundBias = lerp(1.0, groundLike, p9.w);
    float amt = saturate(p9.x * (0.25 + 0.75 * fresnel) * groundBias);
    return float4(lerp(c, reflCol, amt), 1);
}
)";

        // Fake ray-traced bounced light (Beta, rewritten 2026-09-25 as its own pass, extended 2026-09-24 with
        // configurable ray/step/bounce counts - see the comment by PostFx::fakeRTEnabled). A configurable number of
        // real rays per pixel, each independently marched and reprojected like SSR's single ray (short-range: this
        // is about nearby bounce/contact light, not distant reflections), inside a cone that blends from the mirror
        // direction (roughness 0) to the normal (roughness 1). A hit is weighted by the cosine at the ORIGIN (does
        // this surface receive light from that direction at all) and at the HIT (is the surface we found even
        // facing back towards us, using its own smoothed normal - a hit facing away is a silhouette edge, not
        // something that could bounce light our way) - a one-bounce estimate, not a copy. With extra bounces
        // enabled, a ray that hits something keeps going from there in a new cone-sampled direction around THAT
        // surface's own normal, picking up more (decayed) light the same way - there is no real albedo to separate
        // from the lit colour, so each extra bounce's contribution is just weighted down (kBounceDecay) as a stand-in
        // for the energy a real surface would absorb. Output is raw and noisy (rgb = bounce colour, a = how many of
        // the rays found anything at all on their first step); kBlur smooths it before kTemporalBlend/kAddBounce.
        const char* kFakeRT = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = LinW(uv);
    if (w0 <= 0.0 || w0 > p10.w) return float4(0, 0, 0, 0);
    float3 pc = VP(uv, w0);
    float3 n0 = normalize(tex2Dlod(normalTex, float4(uv, 0, 0)).rgb * 2.0 - 1.0);
    float3 viewDir = normalize(pc);
    float3 reflDir = reflect(viewDir, n0);
    float roughness = saturate(p10.y);
    float3 centerDir0 = normalize(lerp(reflDir, n0, roughness));
    float rot = Noise(uv * p3.xy);

    int rayCount = clamp((int)p11.x, 1, 8);
    int stepCount = clamp((int)p11.y, 2, 16);
    int bounceCount = clamp((int)p11.z, 1, 3);
    float firstStep = max(p10.z, 1.0);
    float maxDist = p10.w;
    const float kBounceDecay = 0.6;   // energy stand-in: how much of a bounce's light carries into the next one

    float3 accum = 0.0; float wsum = 0.0; float hits = 0.0;
    [loop] for (int i = 0; i < rayCount; i++)
    {
        float2 seed = float2(frac(rot + i * 0.61803399), frac(rot * 1.37 + i * 0.38196601));
        float3 dir = ConeSample(centerDir0, roughness, seed);
        if (dot(dir, n0) <= 0.0) dir = reflect(dir, n0);   // stay on the visible side of the surface

        float3 origin = pc;
        float3 curN = n0;
        float energy = 1.0;
        bool firstHit = false;

        [loop] for (int b = 0; b < bounceCount; b++)
        {
            float3 rayPos = origin;
            float step = firstStep * (0.6 + 0.8 * seed.x);   // also jitter the step length itself between samples
            bool hit = false; float2 hitUV = uv;
            [loop] for (int s = 0; s < stepCount; s++)
            {
                rayPos += dir * step;
                step *= 1.35;
                if (rayPos.z <= 0.0 || rayPos.z > maxDist) break;
                float2 suv = ProjectUV(rayPos);
                if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
                float sceneW = LinW(suv);
                if (sceneW > 0.0 && sceneW < rayPos.z) { hit = true; hitUV = suv; break; }
            }
            if (!hit) break;
            if (b == 0) firstHit = true;

            float3 hitN = normalize(tex2Dlod(normalTex, float4(hitUV, 0, 0)).rgb * 2.0 - 1.0);
            float3 toOrigin = normalize(origin - rayPos);
            float ndl = saturate(dot(curN, dir));
            float backface = saturate(dot(hitN, toOrigin));
            float w = ndl * backface * energy;
            accum += tex2Dlod(scene, float4(hitUV, 0, 0)).rgb * w;
            wsum += w;

            if (b + 1 >= bounceCount) break;
            origin = rayPos; curN = hitN; energy *= kBounceDecay;
            float3 bounceSeed = frac(float3(seed, rot) * 1.6180339 + b * 0.4142136);
            float3 bounceReflDir = reflect(dir, curN);
            float3 centerDir = normalize(lerp(bounceReflDir, curN, roughness));
            dir = ConeSample(centerDir, roughness, bounceSeed.xy);
            if (dot(dir, curN) <= 0.0) dir = reflect(dir, curN);
        }
        if (firstHit) hits += 1.0;
    }
    float3 result = wsum > 0.001 ? accum / wsum : 0.0;
    return float4(result, hits / (float)rayCount);
}
)";

        // Adds the (already blurred, possibly temporally accumulated) fake-RT bounce colour on top of whatever the
        // pipeline has produced so far, scaled by strength and by its own hit rate (alpha) so a pixel where few/no
        // rays hit anything does not get an over-confident full-strength tint from the handful that did. Linked to
        // SSAO (2026-09-24): also scaled by the same finished occlusion buffer AO already darkened the direct
        // picture with, so a corner AO has darkened does not get its darkness undone by the bounce term landing on
        // top of it - real indirect light is occluded by the same geometry that occludes direct light, and AO is
        // already our best (if crude) estimate of that. p2.x is 0 whenever AO itself is off, so this is then a no-op.
        const char* kAddBounce = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float3 c = tex2Dlod(scene, float4(uv, 0, 0)).rgb;
    float4 rt = tex2Dlod(aoTex, float4(uv, 0, 0));
    float aoFactor = 1.0;
    if (p2.x > 0.001) aoFactor = lerp(1.0, tex2Dlod(aoRawTex, float4(uv, 0, 0)).r, saturate(p2.x));
    return float4(c + rt.rgb * rt.a * p10.x * aoFactor, 1);
}
)";

        // Experimental temporal reprojection for the fake-RT bounce buffer (2026-09-24): no camera transform is
        // captured anywhere in this mod, so there is no real motion vector to reproject with - this instead just
        // keeps blending the SAME screen uv across frames (cheap, no history warp) and falls back to the fresh,
        // noisier frame wherever the depth under that pixel changed too much since last frame (an edge, a moving
        // object, the camera swinging around, a cut). That rejection test is the only thing standing in for real
        // reprojection, so it genuinely reduces flicker while the camera holds still, but visibly smears/ghosts
        // during fast camera motion or a moving car, where a surface's screen position changes between frames but
        // its depth does not necessarily change enough to be caught by the test. p12.x = 0 disables it entirely.
        const char* kTemporalBlend = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 cur = tex2Dlod(aoTex, float4(uv, 0, 0));
    if (p12.x <= 0.001) return cur;
    float w0 = LinW(uv);
    float wh = tex2Dlod(historyDepthTex, float4(uv, 0, 0)).r;
    float valid = (w0 > 0.0 && w0 < 1e8 && wh > 0.0 && wh < 1e8) ? saturate(1.0 - abs(w0 - wh) / (w0 * 0.05 + 20.0)) : 0.0;
    float4 hist = tex2Dlod(historyTex, float4(uv, 0, 0));
    return lerp(cur, hist, saturate(p12.x) * valid);
}
)";

        // Depth-aware blur, one direction at a time (run twice: horizontal then vertical - a "separable" blur, the
        // usual way to afford a much wider kernel than a single 2D pass could: 2x13 taps here versus the old single
        // 5x5 = 25-tap pass, for a kernel more than twice as wide). p3.zw carries the direction (1,0) or (0,1), set
        // by the caller around each of the two draws. Widened 2026-09-24: low-poly models (few, large flat faces)
        // showed visibly faceted SSAO - the per-pixel reconstructed normal is genuinely blocky on those, and the old
        // kernel/tolerance were too tight to smooth it out even though the depth stays continuous across a facet
        // seam (only the normal jumps, not the depth) - a wider reach and a looser depth tolerance blur across that
        // fine, unlike a real silhouette edge, which is a much bigger depth jump and still resists the blur.
        // Blurs whatever is in aoTex, all four channels - AO's rgb-replicated occlusion value; a packed normal
        // (rgb, alpha unused); or the fake-RT pass's noisy raw bounce colour (rgb) + hit rate (alpha) - all blur the
        // same way, depth-similarity weighting is identical regardless of what is actually being blurred.
        const char* kBlur = R"(
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float w0 = LinW(uv);
    float2 dir = p3.zw;
    float4 sum = 0.0; float ws = 0.0;
    [unroll] for (int i = -6; i <= 6; i++)
    {
        float2 o = uv + dir * (float)i * p0.xy;
        float4 a = tex2Dlod(aoTex, float4(o, 0, 0));
        float wgt = saturate(1.0 - abs(LinW(o) - w0) / (w0 * 0.06 + 12.0));
        sum += a * wgt; ws += wgt;
    }
    return sum / max(ws, 1e-3);
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
bool Valid(float2 uv) { float w = LinW(uv); return w > 0.0 && w < 1e8; }   // false where the depth buffer has no real scene in it
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
    // A cut-scene can leave part of the depth buffer with no scene in it at all (found live, 2026-09-24: a dialogue
    // shot only had real depth in the top ~75% of the frame, the rest was never drawn to that frame's depth surface).
    // Dist() treats "no depth" the same as "very far" (1e7) so the sky is hazy/out of focus like everything else far
    // away, but depth of field must NOT do the same: a "very far" blur radius there would smear that empty area
    // heavily and bleed into the real picture at the boundary. Skip depth of field outright wherever there is no
    // real depth; haze already effectively excludes it (distance clamped past the 1e6 cut-off below).
    bool validHere = Valid(uv);
    float w0 = Dist(uv);
    float3 c = Col(uv);

    if (p5.w > 0.05 && validHere)
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
                if (!Valid(suv)) continue;   // do not pull colour from a tap that has no real depth of its own
                float sw = Dist(suv);
                float sc = Coc(sw, focus);
                // a tap only counts if its own blur reaches this pixel, and a sharp nearer object does not bleed into a far one
                float wt = saturate(sc - r + 1.0) * ((sw < w0 * 0.85) ? saturate(sc / max(cc, 0.001)) : 1.0);
                sum += Col(suv) * wt; wsum += wt;
            }
            c = sum / wsum;
        }
    }

    if (p5.x > 0.001 && w0 < 1e6 && validHere)
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
        IDirect3DTexture9* g_ssrWork = nullptr; IDirect3DSurface9* g_ssrWorkSurface = nullptr;   // result of the SSR pass
        IDirect3DTexture9* g_nrmA = nullptr; IDirect3DSurface9* g_nrmASurface = nullptr;   // packed normal, ping-ponged through kBlur same as g_aoA/g_aoB
        IDirect3DTexture9* g_nrmB = nullptr; IDirect3DSurface9* g_nrmBSurface = nullptr;
        IDirect3DTexture9* g_rtA = nullptr; IDirect3DSurface9* g_rtASurface = nullptr;   // fake-RT raw bounce, ping-ponged through kBlur
        IDirect3DTexture9* g_rtB = nullptr; IDirect3DSurface9* g_rtBSurface = nullptr;
        IDirect3DTexture9* g_rtWork = nullptr; IDirect3DSurface9* g_rtWorkSurface = nullptr;   // result of kAddBounce
        IDirect3DTexture9* g_rtHistColor = nullptr; IDirect3DSurface9* g_rtHistColorSurface = nullptr;   // previous frame's accumulated bounce (temporal)
        IDirect3DTexture9* g_rtHistDepth = nullptr; IDirect3DSurface9* g_rtHistDepthSurface = nullptr;   // previous frame's depth, for the temporal rejection test
        IDirect3DPixelShader9 *g_psFinal = nullptr, *g_psAO = nullptr, *g_psBlur = nullptr, *g_psDepthCopy = nullptr, *g_psEffects = nullptr,
                               *g_psSSR = nullptr, *g_psNormalPack = nullptr, *g_psFakeRT = nullptr, *g_psAddBounce = nullptr, *g_psTemporalBlend = nullptr;
        IDirect3DStateBlock9* g_state = nullptr;
        IDirect3DStateBlock9* g_state2 = nullptr;           // for the depth save that runs in the middle of the game's frame
        IDirect3DTexture9* g_depthCopy = nullptr; IDirect3DSurface9* g_depthCopySurface = nullptr;
        UINT g_depthW = 0, g_depthH = 0;
        float g_savedCam[4] = {}; bool g_savedValid = false; int g_saves = 0;
        float g_savedClearZ = 1.0f;   // the raw Z the saved slot was last cleared to (see Slot::clearZ, Calibrate())

        // ---- depth-vs-colour vertical offset (letterboxed cut-scenes) -----------------------------------------------
        // Found live, 2026-09-24: a letterboxed cut-scene can leave real scene depth only in part of the depth buffer
        // (screen-space) while the final colour picture the player sees has the same content correctly centred with
        // matching bars top and bottom. Two earlier attempts didn't pan out: reading the depth content itself for the
        // "no depth" sentinel value was wrong because the untouched region is not reliably that sentinel - it's
        // whatever was left over from an earlier, differently framed draw to the same surface (confirmed live: the
        // same boundary read back as the sentinel pattern once and as ordinary-looking near-camera geometry another
        // time); and this particular game's Clear() calls always cover the whole surface (no partial rect), so that
        // signal never fired either. What actually works (idea from the user, 2026-09-24): a letterboxed cut-scene
        // still clears the WHOLE Z-buffer, but only draws over PART of it - so the untouched part is a perfectly
        // flat band, bit-for-bit the exact Clear() colour, which ordinary rendered geometry (even flat-looking sky)
        // never produces by coincidence. See Calibrate(). Assumes symmetric letterbox bars (bar top == bar bottom,
        // essentially always true for a cinematic crop) and that real content always starts at the very top (every
        // observation so far): the colour picture shows the same content centred, i.e. starting at
        // (1 - contentFraction) / 2, which is therefore also the needed vertical offset. 0 = no cut-off (ordinary
        // gameplay).
        float g_depthYOffset = 0.0f;
        IDirect3DTexture9* g_calibTex = nullptr; IDirect3DSurface9* g_calibSurf = nullptr;   // small R32F downsample target
        IDirect3DSurface9* g_calibSys = nullptr;                                             // matching SYSTEMMEM readback surface
        ULONGLONG g_lastCalibrate = 0;
        constexpr int kCalibSamples = 64;
        // Frames since the last time any INTZ depth surface actually had draw calls on it. Real gameplay (including
        // real-time/Matinee cutscenes) draws the 3D scene every frame; a pre-rendered movie (Bink) does not touch the
        // depth pipeline at all, so the saved depth just sits there stale. Past this many misses, the depth-based
        // effects (AO / haze / depth of field) are skipped instead of compositing onto a scene that is no longer there.
        int g_sceneMissFrames = 1000;
        constexpr int kSceneMissLimit = 2;
        // Draw/save counters reset to 0 every frame (in Apply()), so this threshold is "draws THIS frame since the
        // last save THIS frame" - a low-poly-count shot (a two-person dialogue close-up: just two heads and a bit of
        // background) can easily draw under 20 things a frame, so with a threshold of 20 the depth was never being
        // saved at all for the whole shot, leaving the AO reconstructed from whatever older shot's depth happened to
        // still be sitting in g_depthCopy - visible as ghosted double eyebrows/eyelids where the face had moved since
        // then (found live, 2026-09-24). 1 means "save on the next Clear/leave if anything at all was drawn".
        constexpr int kMinDrawsToSave = 1;
        UINT g_w = 0, g_h = 0; D3DFORMAT g_fmt = D3DFMT_UNKNOWN;
        bool g_compileFailed = false;

        // ---- depth surfaces ----------------------------------------------------------------------------------------
        typedef HRESULT(__stdcall* CreateDepthStencilSurface_t)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
        typedef HRESULT(__stdcall* SetDepthStencilSurface_t)(IDirect3DDevice9*, IDirect3DSurface9*);
        CreateDepthStencilSurface_t oCreateDS = nullptr;
        SetDepthStencilSurface_t oSetDS = nullptr;
        // Every INTZ surface the game binds as depth-stencil gets a slot with the number of draw calls made with it this frame;
        // the scene depth is the one with the most draws (the game has more than one big depth surface: last-bound is not enough).
        struct Slot
        {
            IDirect3DSurface9* surf; int draws; int saved; float cam[4]; bool camValid;
            // The raw Z the game last cleared this slot's Z-buffer to (the Clear() call's own "z" argument - hkClear
            // already receives it, no need to guess it). Real rendered geometry, even a flat-looking sky, varies
            // pixel to pixel once it goes through interpolation/mip selection; a wide band that is bit-for-bit this
            // exact value was never touched by a draw this frame at all - ordinary gameplay never produces that, so
            // finding one is itself the "this is a letterboxed cut-scene" signal (idea from the user, 2026-09-24).
            float clearZ;
        };
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
            g_savedClearZ = g_slots[slot].clearZ;

            if (++g_saves <= 3) LogF("PostFx: depth saved (%d draws on the surface, projection %s)", g_slots[slot].draws, g_slots[slot].camValid ? "ok" : "unknown");
        }

        // Downsamples one column of the just-saved depth copy to find how far up from the bottom a perfectly flat
        // band of "still the Clear() colour" runs - see the comment by Slot::clearZ for why that band means "a
        // letterboxed cut-scene, and this is the untouched part" - and turns that into g_depthYOffset (see the
        // comment by its declaration). Needs a GPU->CPU readback (Lock stalls for the copy to finish), so this only
        // runs on a timer, not every frame.
        void Calibrate(IDirect3DDevice9* dev)
        {
            const ULONGLONG now = GetTickCount64();
            if (now - g_lastCalibrate < 800 || !g_depthCopy || !g_savedValid || g_depthW == 0 || g_depthH == 0) return;
            g_lastCalibrate = now;

            if (!g_calibTex)
            {
                if (FAILED(dev->CreateTexture(1, kCalibSamples, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_calibTex, nullptr)) || !g_calibTex) return;
                g_calibTex->GetSurfaceLevel(0, &g_calibSurf);
                if (FAILED(dev->CreateOffscreenPlainSurface(1, kCalibSamples, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &g_calibSys, nullptr)) || !g_calibSys) return;
            }
            if (!g_calibSurf || !g_calibSys) return;

            const LONG cx = static_cast<LONG>(g_depthW / 2);
            RECT col = { cx, 0, cx + 1, static_cast<LONG>(g_depthH) };
            if (FAILED(dev->StretchRect(g_depthCopySurface, &col, g_calibSurf, nullptr, D3DTEXF_POINT))) return;
            if (FAILED(dev->GetRenderTargetData(g_calibSurf, g_calibSys))) return;

            D3DLOCKED_RECT lr{};
            if (FAILED(g_calibSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
            // Scan from the bottom up: the flat "still the clear colour" band, if any, is at the bottom (every
            // observation so far has real content starting right at the very top, y=0).
            int contentRows = kCalibSamples;
            for (int i = kCalibSamples - 1; i >= 0; --i)
            {
                const float z = *reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(lr.pBits) + i * lr.Pitch);
                if (std::fabs(z - g_savedClearZ) > 1e-5f) { contentRows = i + 1; break; }
                if (i == 0) contentRows = 0;
            }
            g_calibSys->UnlockRect();

            float newOffset = 0.0f;
            const float contentFrac = static_cast<float>(contentRows) / static_cast<float>(kCalibSamples);
            if (contentFrac > 0.05f && contentFrac < 0.999f)
                newOffset = (1.0f - contentFrac) * 0.5f;   // depthTop is always 0 so far: colourTop - 0 = colourTop
            if (std::fabs(newOffset - g_depthYOffset) > 0.001f)
                LogF("PostFx: depth y-offset %.4f -> %.4f (clearZ=%.6f, content rows %d/%d)", g_depthYOffset, newOffset, g_savedClearZ, contentRows, kCalibSamples);
            g_depthYOffset = newOffset;
        }

        typedef HRESULT(__stdcall* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
        Clear_t oClear = nullptr;
        HRESULT __stdcall hkClear(IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
        {
            if ((flags & D3DCLEAR_ZBUFFER) && g_curSlot >= 0)
            {
                // Save using the PREVIOUS Clear's z (still sitting in the slot from last time): that is what the
                // content being saved right now was cleared to before it was drawn. This Clear's own z is for the
                // frame that is about to start, recorded afterwards for the NEXT save to use.
                if (DepthWanted() && g_slots[g_curSlot].draws - g_slots[g_curSlot].saved >= kMinDrawsToSave)
                    SaveDepth(self, g_curSlot);      // the scene is about to be wiped
                g_slots[g_curSlot].clearZ = z;
            }
            return oClear(self, count, rects, flags, color, z, stencil);
        }

        HRESULT __stdcall hkSetDS(IDirect3DDevice9* self, IDirect3DSurface9* surf)
        {
            const int leaving = g_curSlot;
            const HRESULT hr = oSetDS(self, surf);
            if (leaving >= 0 && surf != g_slots[leaving].surf && DepthWanted() && g_slots[leaving].draws - g_slots[leaving].saved >= kMinDrawsToSave)
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
            g_psSSR = Compile(dev, compile, kSSR, "postfx_ssr");
            g_psNormalPack = Compile(dev, compile, kNormalPack, "postfx_normalpack");
            g_psFakeRT = Compile(dev, compile, kFakeRT, "postfx_fakert");
            g_psAddBounce = Compile(dev, compile, kAddBounce, "postfx_addbounce");
            g_psTemporalBlend = Compile(dev, compile, kTemporalBlend, "postfx_temporalblend");
            LogF("PostFx: shaders compiled (final %d, ao %d, blur %d, ssr %d, normalpack %d, fakert %d, addbounce %d, temporal %d)",
                 g_psFinal ? 1 : 0, g_psAO ? 1 : 0, g_psBlur ? 1 : 0, g_psSSR ? 1 : 0, g_psNormalPack ? 1 : 0,
                 g_psFakeRT ? 1 : 0, g_psAddBounce ? 1 : 0, g_psTemporalBlend ? 1 : 0);
            return g_psFinal != nullptr;
        }

        bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DTexture9** tex, IDirect3DSurface9** surf)
        {
            if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, tex, nullptr)) || !*tex) return false;
            (*tex)->GetSurfaceLevel(0, surf);
            return *surf != nullptr;
        }

        // Clears a render target to bit-pattern zero (D3D9 converts a black D3DCOLOR to 0.0 in every channel
        // regardless of the target's format, including a float format like R32F) - used to seed history buffers so
        // their first read, before anything has ever been written into them, reads back as "no history yet"
        // rather than whatever driver-dependent garbage a fresh D3DPOOL_DEFAULT allocation might contain.
        void ClearToZero(IDirect3DDevice9* dev, IDirect3DSurface9* surf)
        {
            IDirect3DSurface9* prevRT = nullptr;
            dev->GetRenderTarget(0, &prevRT);
            dev->SetRenderTarget(0, surf);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 0, 0);
            dev->SetRenderTarget(0, prevRT);
            SafeRelease(prevRT);
        }

        bool EnsureResources(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d, bool needAO, bool needWork, bool needSSR, bool needFakeRT, bool needTemporal)
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
            if (needSSR && !g_ssrWork && !MakeTarget(dev, d.Width, d.Height, d.Format, &g_ssrWork, &g_ssrWorkSurface)) { status = "cannot create the SSR target"; return false; }
            if ((needSSR || needFakeRT) && !g_nrmA)
            {
                if (!MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_nrmA, &g_nrmASurface) || !MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_nrmB, &g_nrmBSurface))
                { status = "cannot create the normal targets"; return false; }
            }
            if (needFakeRT && !g_rtA)
            {
                // Explicit A8R8G8B8 (not the backbuffer's own format, which is often alpha-less) - the hit-rate
                // weight kFakeRT writes into alpha, and the temporal blend below, both need a real alpha channel.
                if (!MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_rtA, &g_rtASurface) || !MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_rtB, &g_rtBSurface))
                { status = "cannot create the fake-RT targets"; return false; }
            }
            if (needFakeRT && !g_rtWork && !MakeTarget(dev, d.Width, d.Height, d.Format, &g_rtWork, &g_rtWorkSurface)) { status = "cannot create the fake-RT composite target"; return false; }
            if (needTemporal && !g_rtHistColor)
            {
                if (!MakeTarget(dev, d.Width, d.Height, D3DFMT_A8R8G8B8, &g_rtHistColor, &g_rtHistColorSurface) ||
                    !MakeTarget(dev, d.Width, d.Height, D3DFMT_R32F, &g_rtHistDepth, &g_rtHistDepthSurface))
                { status = "cannot create the fake-RT temporal history targets"; return false; }
                ClearToZero(dev, g_rtHistColorSurface);
                ClearToZero(dev, g_rtHistDepthSurface);   // depth 0.0 reads as "no history yet" in kTemporalBlend (wh > 0.0 check)
            }
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

    void OnDraw(IDirect3DDevice9* dev)
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
        SafeRelease(g_ssrWorkSurface); SafeRelease(g_ssrWork);
        SafeRelease(g_nrmASurface); SafeRelease(g_nrmA);
        SafeRelease(g_nrmBSurface); SafeRelease(g_nrmB);
        SafeRelease(g_rtASurface); SafeRelease(g_rtA);
        SafeRelease(g_rtBSurface); SafeRelease(g_rtB);
        SafeRelease(g_rtWorkSurface); SafeRelease(g_rtWork);
        SafeRelease(g_rtHistColorSurface); SafeRelease(g_rtHistColor);
        SafeRelease(g_rtHistDepthSurface); SafeRelease(g_rtHistDepth);
        SafeRelease(g_state); SafeRelease(g_state2);
        SafeRelease(g_depthCopySurface); SafeRelease(g_depthCopy); g_depthW = g_depthH = 0; g_savedValid = false;
        SafeRelease(g_calibSurf); SafeRelease(g_calibTex); SafeRelease(g_calibSys); g_depthYOffset = 0.0f; g_lastCalibrate = 0;
        g_sceneMissFrames = 1000;
        g_w = g_h = 0;
    }

    void Apply(IDirect3DDevice9* dev)
    {
        // The scene depth = the INTZ surface with the most draw calls this frame; its projection comes with it.
        {
            int best = -1;
            for (int i = 0; i < kSlots; ++i) if (g_slots[i].surf && (best < 0 || g_slots[i].draws > g_slots[best].draws)) best = i;
            const bool sceneThisFrame = best >= 0 && g_slots[best].draws > 0;
            if (sceneThisFrame)
            {
                g_scene = g_slots[best].surf; g_sceneDraws = g_slots[best].draws;
                g_camValid = g_slots[best].camValid;
                if (g_camValid) memcpy(g_cam, g_slots[best].cam, sizeof(g_cam));
                g_sceneMissFrames = 0;
            }
            else if (g_sceneMissFrames < 1000000)
                ++g_sceneMissFrames;
            for (Slot& s : g_slots) { s.draws = 0; s.saved = 0; }
        }
        // depth status for the menu (also shown while the effects are off, to check that the depth surface exists)
        {
            D3DSURFACE_DESC sd{};
            char extra[64] = "";
            if (g_sceneMissFrames > kSceneMissLimit) snprintf(extra, sizeof(extra), " [stale: %d frames without a scene]", g_sceneMissFrames);
            if (g_scene && SUCCEEDED(g_scene->GetDesc(&sd)))
                snprintf(depthStatus, sizeof(depthStatus), "scene depth %ux%u (%d draws; %d INTZ surfaces made), projection %s: A=%.5f B=%.3f focal=%.3f/%.3f%s",
                         sd.Width, sd.Height, g_sceneDraws, g_intzCreated, g_camValid ? "ok" : "unknown", g_cam[0], g_cam[1], g_cam[2], g_cam[3], extra);
            else
                snprintf(depthStatus, sizeof(depthStatus), "no scene depth surface (INTZ surfaces made: %d, failed: %d)%s", g_intzCreated, g_intzFailed, extra);
        }

        if (!enabled) { status = "off"; return; }

        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &bb)) || !bb) return;
        D3DSURFACE_DESC desc{}; bb->GetDesc(&desc);

        // The depth copy was made against whatever the scene's depth surface measured at save time. If the current
        // back buffer is a different size (a letterboxed cutscene rendering into a differently sized target is the
        // known case) the two are no longer pixel-aligned, so sampling one against the other would misplace the
        // depth-based effects rather than just being wrong - skip them instead. Likewise if no depth surface has had
        // draws on it recently (a pre-rendered/Bink cutscene playing over the last real 3D frame's depth), the saved
        // depth is stale and must not be composited onto whatever is on screen now.
        const bool grading = saturation != 1.0f || contrast != 1.0f || gamma != 1.0f;
        const bool dimsMatch = g_depthW == desc.Width && g_depthH == desc.Height;
        const bool depthReady = g_depthCopy && g_savedValid && g_sceneMissFrames <= kSceneMissLimit && dimsMatch;
        bool aoOn = depthReady && (aoStrength > 0.001f || debugAO || debugDepth);     // the occlusion pass
        bool fxOn = depthReady && (aoStrength > 0.001f || fogStrength > 0.001f || dofAmount > 0.05f);   // the depth-effects pass
        bool ssrOn = depthReady && ssrEnabled && ssrStrength > 0.001f;                // experimental screen-space reflections
        bool fakeRTOn = depthReady && fakeRTEnabled && fakeRTStrength > 0.001f;       // fake ray-traced bounced light
        bool fakeRTTemporalOn = fakeRTOn && fakeRTTemporal && fakeRTTemporalWeight > 0.001f;   // experimental temporal accumulation on top
        if (depthReady) Calibrate(dev);   // keep g_depthYOffset current for a letterboxed cut-scene (throttled: most calls are a no-op)
        // Diagnostic: log every time depth-effect readiness flips, with the numbers behind the decision. Cheap (only
        // fires on a change), and it is the only way to see, after the fact, what the back buffer / depth copy sizes
        // and the scene-miss counter actually were during a cutscene the user reproduces.
        {
            static bool everLogged = false;
            static bool lastReady = false;
            static float lastOffset = -1.0f;
            if (!everLogged || depthReady != lastReady || std::fabs(g_depthYOffset - lastOffset) > 0.005f)
            {
                LogF("PostFx: depth-effects %s - backbuffer=%ux%u depthcopy=%ux%u dimsMatch=%d sceneMiss=%d savedValid=%d yOffset=%.4f",
                     depthReady ? "READY" : "NOT ready", desc.Width, desc.Height, g_depthW, g_depthH,
                     dimsMatch ? 1 : 0, g_sceneMissFrames, g_savedValid ? 1 : 0, g_depthYOffset);
                everLogged = true; lastReady = depthReady; lastOffset = g_depthYOffset;
            }
        }
        if (fxaa <= 0.001f && sharpen <= 0.001f && !grading && !aoOn && !fxOn && !ssrOn && !fakeRTOn) { bb->Release(); status = "off"; return; }

        // A resource-creation failure (out of memory, device lost, ...) is fatal for this frame - bail entirely, as
        // before. A single effect's SHADER failing to compile is not: CompileShaders() (called from inside this)
        // tries every shader up front and leaves whichever ones failed permanently null, so a typo in just one of
        // them (found live, 2026-09-25: a bad tex2Dlod() call in the AO/GI shader) used to take the whole pass down
        // with it, including plain FXAA/sharpening that has nothing to do with AO - silently drop just the broken
        // effect instead and keep going with whatever still compiled.
        if (!EnsureResources(dev, desc, aoOn, fxOn, ssrOn, fakeRTOn, fakeRTTemporalOn)) { bb->Release(); return; }
        const char* failReason = nullptr;
        if (aoOn && (!g_psAO || !g_psBlur)) { aoOn = false; failReason = "occlusion/GI shader failed - skipped"; }
        if (fxOn && !g_psEffects) { fxOn = false; failReason = "effects shader failed - skipped"; }
        if (ssrOn && (!g_psSSR || !g_psNormalPack || !g_psBlur)) { ssrOn = false; failReason = "SSR shader failed - skipped"; }
        if (fakeRTOn && (!g_psFakeRT || !g_psAddBounce || !g_psNormalPack || !g_psBlur)) { fakeRTOn = false; fakeRTTemporalOn = false; failReason = "fake-RT shader failed - skipped"; }
        if (fakeRTTemporalOn && !g_psTemporalBlend) { fakeRTTemporalOn = false; failReason = "fake-RT temporal shader failed - skipped"; }
        {
            static const char* lastLoggedFail = nullptr;
            if (failReason && failReason != lastLoggedFail) { LogF("PostFx: %s", failReason); lastLoggedFail = failReason; }
        }
        if (fxaa <= 0.001f && sharpen <= 0.001f && !grading && !aoOn && !fxOn && !ssrOn && !fakeRTOn) { bb->Release(); status = failReason ? failReason : "off"; return; }

        if (!g_state && (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state)) || !g_state)) { bb->Release(); status = "no state block"; return; }
        g_state->Capture();

        if (FAILED(dev->StretchRect(bb, nullptr, g_copySurface, nullptr, D3DTEXF_NONE))) { status = "StretchRect failed"; bb->Release(); return; }

        IDirect3DTexture9* depthTex = (aoOn || ssrOn || fakeRTOn) ? g_depthCopy : nullptr;      // the scene depth saved during the frame (R32F)
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
        const float c8[4] = { g_depthYOffset, 0.0f, 0.0f, 0.0f };
        const float c9[4] = { ssrStrength, ssrStepCm, ssrMaxDistanceCm, ssrGroundBias };
        const float c10[4] = { fakeRTStrength, fakeRTRoughness, fakeRTStepCm, fakeRTMaxDistanceCm };
        const float c11[4] = { static_cast<float>(fakeRTRayCount), static_cast<float>(fakeRTStepCount), static_cast<float>(fakeRTBounces), 0.0f };
        const float c12[4] = { fakeRTTemporalOn ? fakeRTTemporalWeight : 0.0f, 0.0f, 0.0f, 0.0f };
        dev->SetPixelShaderConstantF(5, c5, 1); dev->SetPixelShaderConstantF(6, c6, 1); dev->SetPixelShaderConstantF(7, c7, 1);
        dev->SetPixelShaderConstantF(8, c8, 1); dev->SetPixelShaderConstantF(9, c9, 1); dev->SetPixelShaderConstantF(10, c10, 1);
        dev->SetPixelShaderConstantF(11, c11, 1); dev->SetPixelShaderConstantF(12, c12, 1);

        if (haveDepthTex)
        {
            dev->SetTexture(1, depthTex); SetSampler(dev, 1, D3DTEXF_POINT);
            // pass 1: raw occlusion
            dev->SetRenderTarget(0, g_aoASurface);
            dev->SetPixelShader(g_psAO);
            dev->SetTexture(0, g_copy); SetSampler(dev, 0, D3DTEXF_POINT);
            dev->SetTexture(2, nullptr);
            DrawQuad(dev, desc.Width, desc.Height);
            // pass 2+3: depth-aware blur, horizontal then vertical (separable - see the comment by kBlur)
            dev->SetPixelShader(g_psBlur);
            const float c3h[4] = { w, h, 1.0f, 0.0f };
            dev->SetPixelShaderConstantF(3, c3h, 1);
            dev->SetRenderTarget(0, g_aoBSurface);
            dev->SetTexture(2, g_aoA); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);
            const float c3v[4] = { w, h, 0.0f, 1.0f };
            dev->SetPixelShaderConstantF(3, c3v, 1);
            dev->SetRenderTarget(0, g_aoASurface);
            dev->SetTexture(2, g_aoB); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);
            dev->SetRenderTarget(0, bb);
            dev->SetTexture(2, g_aoA); SetSampler(dev, 2, D3DTEXF_LINEAR);
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
            dev->SetTexture(2, aoOn ? g_aoA : nullptr); if (aoOn) SetSampler(dev, 2, D3DTEXF_LINEAR);   // g_aoA holds the final (post horizontal+vertical blur) AO
            DrawQuad(dev, desc.Width, desc.Height);
            finalSource = g_work;
        }

        // Shared smoothed normal (pack + the same horizontal/vertical blur AO uses) for SSR and fake-RT below, so low-poly
        // facet seams do not show up as a visible mesh in either effect (found live, 2026-09-25). Built once, used by both.
        const bool wantNormal = (ssrOn || fakeRTOn) && g_nrmA && g_nrmB && depthTex;
        if (wantNormal)
        {
            dev->SetTexture(1, depthTex); SetSampler(dev, 1, D3DTEXF_POINT);
            dev->SetRenderTarget(0, g_nrmASurface);
            dev->SetPixelShader(g_psNormalPack);
            dev->SetTexture(0, nullptr); dev->SetTexture(2, nullptr);
            DrawQuad(dev, desc.Width, desc.Height);

            dev->SetPixelShader(g_psBlur);
            const float nc3h[4] = { w, h, 1.0f, 0.0f };
            dev->SetPixelShaderConstantF(3, nc3h, 1);
            dev->SetRenderTarget(0, g_nrmBSurface);
            dev->SetTexture(2, g_nrmA); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);
            const float nc3v[4] = { w, h, 0.0f, 1.0f };
            dev->SetPixelShaderConstantF(3, nc3v, 1);
            dev->SetRenderTarget(0, g_nrmASurface);
            dev->SetTexture(2, g_nrmB); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);
        }

        // experimental screen-space reflections (Debug tab): reads whatever the pipeline has produced so far and the
        // same depth texture, writes a reflected version into its own target.
        if (ssrOn && wantNormal && g_ssrWork)
        {
            dev->SetTexture(1, depthTex); SetSampler(dev, 1, D3DTEXF_POINT);
            dev->SetRenderTarget(0, g_ssrWorkSurface);
            dev->SetPixelShader(g_psSSR);
            dev->SetTexture(0, finalSource); SetSampler(dev, 0, D3DTEXF_LINEAR);
            dev->SetTexture(2, nullptr);
            dev->SetTexture(3, g_nrmA); SetSampler(dev, 3, D3DTEXF_LINEAR);
            DrawQuad(dev, desc.Width, desc.Height);
            dev->SetTexture(3, nullptr);
            finalSource = g_ssrWork;
        }

        // fake ray-traced bounced light (Beta): a handful of real jittered/cone-sampled multi-step (multi-bounce)
        // rays per pixel, reprojected and weighted by the cosine terms at both ends - see kFakeRT's comment. The
        // noisy per-pixel result is blurred the same way AO/the normal buffer are, optionally smoothed further
        // across frames (temporal, experimental), then added back on top of the picture, scaled by SSAO too.
        if (fakeRTOn && wantNormal && g_rtA && g_rtB && g_rtWork)
        {
            dev->SetTexture(1, depthTex); SetSampler(dev, 1, D3DTEXF_POINT);
            dev->SetRenderTarget(0, g_rtASurface);
            dev->SetPixelShader(g_psFakeRT);
            dev->SetTexture(0, finalSource); SetSampler(dev, 0, D3DTEXF_LINEAR);
            dev->SetTexture(2, nullptr);
            dev->SetTexture(3, g_nrmA); SetSampler(dev, 3, D3DTEXF_LINEAR);
            DrawQuad(dev, desc.Width, desc.Height);
            dev->SetTexture(3, nullptr);

            dev->SetPixelShader(g_psBlur);
            const float rc3h[4] = { w, h, 1.0f, 0.0f };
            dev->SetPixelShaderConstantF(3, rc3h, 1);
            dev->SetRenderTarget(0, g_rtBSurface);
            dev->SetTexture(0, nullptr);
            dev->SetTexture(2, g_rtA); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);
            const float rc3v[4] = { w, h, 0.0f, 1.0f };
            dev->SetPixelShaderConstantF(3, rc3v, 1);
            dev->SetRenderTarget(0, g_rtASurface);
            dev->SetTexture(2, g_rtB); SetSampler(dev, 2, D3DTEXF_POINT);
            DrawQuad(dev, desc.Width, desc.Height);

            IDirect3DTexture9* rtBounce = g_rtA;   // the spatially-blurred bounce buffer, possibly replaced by the temporally-blended one below

            if (fakeRTTemporalOn && g_psTemporalBlend && g_rtHistColor && g_rtHistDepth)
            {
                dev->SetRenderTarget(0, g_rtBSurface);   // g_rtB's blur ping-pong role is done for this frame - reuse it as the blend output
                dev->SetPixelShader(g_psTemporalBlend);
                dev->SetTexture(0, nullptr);
                dev->SetTexture(2, g_rtA); SetSampler(dev, 2, D3DTEXF_POINT);          // this frame's fresh, spatially-blurred bounce
                dev->SetTexture(5, g_rtHistColor); SetSampler(dev, 5, D3DTEXF_POINT);  // last frame's accumulated bounce
                dev->SetTexture(6, g_rtHistDepth); SetSampler(dev, 6, D3DTEXF_POINT);  // last frame's depth (rejection test)
                DrawQuad(dev, desc.Width, desc.Height);
                dev->SetTexture(5, nullptr); dev->SetTexture(6, nullptr);
                rtBounce = g_rtB;
                // Carry this frame's result into the history buffers for the next frame: a plain GPU copy, no shader
                // needed (StretchRect between two render targets of the same format). Safe to overwrite now - the
                // draw above that read the OLD history has already finished.
                dev->StretchRect(g_rtBSurface, nullptr, g_rtHistColorSurface, nullptr, D3DTEXF_NONE);
                dev->StretchRect(g_depthCopySurface, nullptr, g_rtHistDepthSurface, nullptr, D3DTEXF_NONE);
            }

            if (debugFakeRT)
            {
                finalSource = rtBounce;
            }
            else
            {
                dev->SetRenderTarget(0, g_rtWorkSurface);
                dev->SetPixelShader(g_psAddBounce);
                dev->SetTexture(0, finalSource); SetSampler(dev, 0, D3DTEXF_LINEAR);
                dev->SetTexture(2, rtBounce); SetSampler(dev, 2, D3DTEXF_LINEAR);
                dev->SetTexture(4, aoOn ? g_aoA : nullptr); if (aoOn) SetSampler(dev, 4, D3DTEXF_LINEAR);   // links the bounce to SSAO - see kAddBounce
                DrawQuad(dev, desc.Width, desc.Height);
                dev->SetTexture(4, nullptr);
                finalSource = g_rtWork;
            }
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

#include <Windows.h>
#include <d3d9.h>
#include <cstring>
#include <intrin.h>
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx9.h"
#include "D3DHook.h"
#include "Overlay.h"
#include "Police.h"
#include "VehicleMod.h"
#include "BuiltinCheats.h"
#include "MapTools.h"
#include "MiniMap.h"
#include "CrimeTools.h"
#include "Aimbot.h"
#include "NpcMod.h"
#include "WeaponMod.h"
#include "Perf.h"
#include "Binds.h"
#include "Theme.h"
#include "Patches.h"
#include "Offsets.h"
#include "Log.h"
#include "LoadGuard.h"
#include "KismetVars.h"
#include "GfxBoost.h"
#include "PostFx.h"
#include "MouseLook.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

int g_skyMode = 0;
float g_shadowBiasScale = 1.0f, g_shadowSlopeScale = 1.0f;
int g_viewportWidth = 1920;
int g_viewportHeight = 1080;

bool g_catchMainLoopExceptions = false;   // off by default - experimental, see D3DHook.h

namespace
{
    typedef HRESULT(__stdcall* EndScene_t)(IDirect3DDevice9*);
    typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    typedef HRESULT(__stdcall* SetVertexShaderConstantF_t)(IDirect3DDevice9*, UINT, const float*, UINT);

    EndScene_t oEndScene = nullptr;
    Reset_t oReset = nullptr;
    SetVertexShaderConstantF_t oSetVertexShaderConstantF = nullptr;
    WNDPROC oWndProc = nullptr;

    // Vertex shader constant register c240 - confirmed empirically (see the
    // comment in D3DHook.h) to hold a View*Projection-like matrix: it rotates
    // with the camera and its translation row is world-scale. Just keep the
    // latest value; no per-frame call counting needed anymore now that the
    // register is known.
    float g_viewProjCandidate[16] = {};
    bool g_haveViewProjCandidate = false;
    constexpr UINT kViewProjRegister = 240;

    HRESULT __stdcall hkSetVertexShaderConstantF(IDirect3DDevice9* self, UINT startRegister,
                                                  const float* data, UINT vector4fCount)
    {
        if (startRegister == kViewProjRegister && vector4fCount == 4 && data)
        {
            memcpy(g_viewProjCandidate, data, sizeof(float) * 16);
            g_haveViewProjCandidate = true;
        }
        return oSetVertexShaderConstantF(self, startRegister, data, vector4fCount);
    }

    HWND g_hWnd = nullptr;
    bool g_imguiInitialized = false;
    bool g_insertWasDown = false;
    bool g_homeWasDown = false;
    bool g_cursorVisible = false;

    LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (Overlay::visible)
        {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse &&
                (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN ||
                 msg == WM_RBUTTONUP || msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL))
                return TRUE;
            if (io.WantCaptureKeyboard && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR))
                return TRUE;
        }
        return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
    }

    void EnsureImGuiInitialized(IDirect3DDevice9* pDevice)
    {
        if (g_imguiInitialized) return;
        LogF("EnsureImGuiInitialized: first hkEndScene call, initializing");

        D3DDEVICE_CREATION_PARAMETERS params;
        pDevice->GetCreationParameters(&params);
        g_hWnd = params.hFocusWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        Theme::Apply();   // colours, square corners, font (must come before the renderer backend is initialised)

        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(pDevice);
        Overlay::InitFeatures();   // registers every bindable option, then loads the settings and the auto-load config

        oWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));

        g_imguiInitialized = true;
        LogF("EnsureImGuiInitialized: done, hwnd=0x%p", g_hWnd);
    }

    void HandleHotkeys()
    {
        const bool capturing = Binds::SystemKeyCapturing() != 0;   // a key is being chosen in Settings: no hotkey may fire
        bool insertDown = !capturing && (GetAsyncKeyState(Binds::menuKey) & 0x8000) != 0;
        if (insertDown && !g_insertWasDown)
            Overlay::visible = !Overlay::visible;
        g_insertWasDown = insertDown;

        // Home toggles the OS cursor so you can actually click on the
        // overlay's controls instead of fighting the game's mouse-look.
        bool homeDown = !capturing && (GetAsyncKeyState(Binds::cursorKey) & 0x8000) != 0;
        if (homeDown && !g_homeWasDown)
        {
            g_cursorVisible = !g_cursorVisible;
            Overlay::wantMouseCapture = g_cursorVisible;
            LogF("HandleHotkeys: cursor key pressed, wantMouseCapture=%s", g_cursorVisible ? "true" : "false");

            // ShowCursor is refcounted and the game may fight it every frame,
            // so don't rely on it for visibility - just try to nudge it and
            // let ImGui draw its own cursor instead (see hkEndScene).
            if (g_cursorVisible)
            {
                ClipCursor(nullptr);
                while (ShowCursor(TRUE) < 0) {}
            }
            else
            {
                while (ShowCursor(FALSE) >= 0) {}
            }
        }
        g_homeWasDown = homeDown;
    }

    void UpdateViewportSize(IDirect3DDevice9* pDevice)
    {
        D3DVIEWPORT9 vp;
        if (SUCCEEDED(pDevice->GetViewport(&vp)))
        {
            g_viewportWidth = static_cast<int>(vp.Width);
            g_viewportHeight = static_cast<int>(vp.Height);
        }
    }

    // The overlay must only be drawn into the real back buffer: the game also ends scenes of off-screen render targets
    // (Scaleform / minimap passes) and drawing into those would corrupt the game's own HUD.
    bool RenderTargetIsBackBuffer(IDirect3DDevice9* pDevice)
    {
        IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* bb = nullptr;
        bool same = true;
        if (SUCCEEDED(pDevice->GetRenderTarget(0, &rt)) && rt)
        {
            if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { same = (rt == bb); bb->Release(); }
            rt->Release();
        }
        return same;
    }

    // ---- one-frame capture of the draw calls -------------------------------------------------------------------------
    typedef HRESULT(__stdcall* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    typedef HRESULT(__stdcall* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    DrawIndexedPrimitive_t oDrawIndexedPrimitive = nullptr;
    DrawPrimitive_t oDrawPrimitive = nullptr;
    volatile long g_capState = 0;   // 0 idle, 1 armed (starts at the next frame end), 2 capturing
    FILE* g_capFile = nullptr;
    int g_capIndex = 0;
    char g_capStatus[96] = "idle";

    bool IsSkyDome(IDirect3DDevice9* dev, UINT primCount);
    void LogDraw(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE type, UINT count, INT baseVertex = 0, UINT numVertices = 0, UINT startIndex = 0)
    {
        if (!g_capFile) return;
        // Geometry identity: which buffers and which range of them this draw uses (tells duplicated meshes apart).
        {
            IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0; IDirect3DIndexBuffer9* ib = nullptr;
            dev->GetStreamSource(0, &vb, &off, &stride); dev->GetIndices(&ib);
            fprintf(g_capFile, "   geo: vb=%p+%u stride=%u ib=%p base=%d verts=%u start=%u\n", static_cast<void*>(vb), off, stride, static_cast<void*>(ib), baseVertex, numVertices, startIndex);
            if (vb) vb->Release();
            if (ib) ib->Release();
        }
        D3DVIEWPORT9 vp{}; dev->GetViewport(&vp);
        IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* ds = nullptr; D3DSURFACE_DESC rd{}, dd{};
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) { rt->GetDesc(&rd); }
        if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) { ds->GetDesc(&dd); }
        IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
        dev->GetVertexShader(&vs); dev->GetPixelShader(&ps);
        DWORD z = 0, zw = 0, zf = 0, ab = 0, sb = 0, db = 0, cull = 0, at = 0, bias = 0, slope = 0, cw = 0, st = 0;
        dev->GetRenderState(D3DRS_ZENABLE, &z); dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw); dev->GetRenderState(D3DRS_ZFUNC, &zf);
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab); dev->GetRenderState(D3DRS_SRCBLEND, &sb); dev->GetRenderState(D3DRS_DESTBLEND, &db);
        dev->GetRenderState(D3DRS_CULLMODE, &cull); dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
        dev->GetRenderState(D3DRS_DEPTHBIAS, &bias); dev->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &slope);
        dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw); dev->GetRenderState(D3DRS_STENCILENABLE, &st);
        fprintf(g_capFile, "#%d %s type=%d prims=%u vp=%u,%u %ux%u z[%.3f..%.3f] rt=%p %ux%u f%d ds=%p %ux%u f%d vs=%p ps=%p | Z%lu W%lu F%lu AB%lu(%lu,%lu) cull%lu AT%lu bias=%08lX slope=%08lX cw%lX st%lu |",
                g_capIndex++, kind, static_cast<int>(type), count, vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ,
                static_cast<void*>(rt), rd.Width, rd.Height, static_cast<int>(rd.Format), static_cast<void*>(ds), dd.Width, dd.Height, static_cast<int>(dd.Format),
                static_cast<void*>(vs), static_cast<void*>(ps), z, zw, zf, ab, sb, db, cull, at, bias, slope, cw, st);
        bool readsShadow = false;
        for (DWORD s = 0; s < 6; ++s)
        {
            IDirect3DBaseTexture9* tex = nullptr;
            if (SUCCEEDED(dev->GetTexture(s, &tex)) && tex)
            {
                const D3DRESOURCETYPE rtp = tex->GetType();
                if (rtp == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC td{}; static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &td); if (td.Format == 114 && td.Width == 2048 && td.Height == 2048) readsShadow = true; fprintf(g_capFile, " t%lu=%p(%ux%u f%d u%lX)", s, static_cast<void*>(tex), td.Width, td.Height, static_cast<int>(td.Format), td.Usage); }
                else fprintf(g_capFile, " t%lu=%p(type%d)", s, static_cast<void*>(tex), static_cast<int>(rtp));
                tex->Release();
            }
        }
        fprintf(g_capFile, "\n");
        if (IsSkyDome(dev, count))
        {
            float c[4 * 32] = {}; dev->GetVertexShaderConstantF(0, c, 32);
            for (int i = 0; i < 32; ++i) fprintf(g_capFile, "   vsc%d: %g %g %g %g\n", i, c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
            float p[4 * 8] = {}; dev->GetPixelShaderConstantF(0, p, 8);
            for (int i = 0; i < 8; ++i) fprintf(g_capFile, "   psc%d: %g %g %g %g\n", i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
        }
        // Shadow maps: the light matrices. Caster pass = rendering into the 2048x2048 R32F map (dump when the cascade viewport
        // changes), receiver pass = a draw that samples that map (dump the first ones and every 150th).
        {
            static UINT lastX = ~0u, lastY = ~0u; static int recv = 0;
            if (g_capIndex <= 1) { lastX = lastY = ~0u; recv = 0; }
            const bool caster = rd.Width == 2048 && rd.Height == 2048 && rd.Format == 114;
            bool dump = false; const char* tag = "";
            if (caster && (vp.X != lastX || vp.Y != lastY)) { dump = true; tag = "shadow caster cascade"; lastX = vp.X; lastY = vp.Y; }
            if (readsShadow && (recv++ % 150) == 0 && recv < 700) { dump = true; tag = "shadow receiver"; }
            if (dump)
            {
                fprintf(g_capFile, "   -- %s constants --\n", tag);
                float c[4 * 48] = {}; dev->GetVertexShaderConstantF(0, c, 48);
                for (int i = 0; i < 48; ++i) fprintf(g_capFile, "   vsc%d: %g %g %g %g\n", i, c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
                float p[4 * 24] = {}; dev->GetPixelShaderConstantF(0, p, 24);
                for (int i = 0; i < 24; ++i) fprintf(g_capFile, "   psc%d: %g %g %g %g\n", i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
            }
        }
        if (rt) rt->Release();
        if (ds) ds->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
    }

    // Sky domes: back-to-back draws of a 2048x512 DXT1 texture in stage 0. Counted per frame (reset at EndScene).
    volatile long g_skyDrawsThisFrame = 0;
    volatile long g_skyDrawsLastFrame = 0;

    bool IsSkyDome(IDirect3DDevice9* dev, UINT primCount)
    {
        if (primCount < 64) return false;
        IDirect3DBaseTexture9* tex = nullptr;
        bool sky = false;
        if (SUCCEEDED(dev->GetTexture(0, &tex)) && tex)
        {
            if (tex->GetType() == D3DRTYPE_TEXTURE)
            {
                D3DSURFACE_DESC td{}; static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &td);
                sky = td.Width == 2048 && td.Height == 512 && td.Format == D3DFMT_DXT1;
            }
            tex->Release();
        }
        return sky;
    }

    HRESULT __stdcall hkDrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT primCount)
    {
        PostFx::OnDraw(self);   // camera projection snapshot after a depth surface was bound; draw counter per depth surface
        if (g_capState == 2) LogDraw(self, "DIP", type, primCount, baseVertex, numVertices, startIndex);
        // Only the shadow map pass draws with a non-zero depth bias (every other pass in the capture has bias 0).
        if (g_shadowBiasScale != 1.0f || g_shadowSlopeScale != 1.0f)
        {
            DWORD bias = 0, slope = 0;
            self->GetRenderState(D3DRS_DEPTHBIAS, &bias);
            if (bias != 0)
            {
                self->GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, &slope);
                float b, s; memcpy(&b, &bias, 4); memcpy(&s, &slope, 4);
                b *= g_shadowBiasScale; s *= g_shadowSlopeScale;
                DWORD nb, ns; memcpy(&nb, &b, 4); memcpy(&ns, &s, 4);
                self->SetRenderState(D3DRS_DEPTHBIAS, nb);
                self->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, ns);
                const HRESULT hr = oDrawIndexedPrimitive(self, type, baseVertex, minVertex, numVertices, startIndex, primCount);
                self->SetRenderState(D3DRS_DEPTHBIAS, bias);
                self->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, slope);
                return hr;
            }
        }
        if (g_skyMode != 0 && IsSkyDome(self, primCount))
        {
            // Domes are counted per render target: other passes (reflections, off-screen views) may draw a sky of their own.
            IDirect3DSurface9* rt = nullptr; self->GetRenderTarget(0, &rt);
            static IDirect3DSurface9* lastRt = nullptr;
            if (rt != lastRt) { if (g_skyDrawsThisFrame > 0) g_skyDrawsLastFrame = g_skyDrawsThisFrame; g_skyDrawsThisFrame = 0; lastRt = rt; }
            if (rt) rt->Release();
            const long index = InterlockedIncrement(&g_skyDrawsThisFrame);   // 1-based
            const long total = g_skyDrawsLastFrame;                          // how many domes the previous run had
            const bool draw = g_skyMode == 1 ? index == 1 : (total <= 1 || index >= total);
            if (!draw) return D3D_OK;
        }
        return oDrawIndexedPrimitive(self, type, baseVertex, minVertex, numVertices, startIndex, primCount);
    }
    HRESULT __stdcall hkDrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount)
    {
        if (g_capState == 2) LogDraw(self, "DP ", type, primCount);
        return oDrawPrimitive(self, type, startVertex, primCount);
    }

    // Frame boundary of the capture: armed -> starts at this frame end, capturing -> stops at the next one.
    void CaptureFrameBoundary()
    {
        // Only the frame that ends on the back buffer counts (this runs for it only); off-screen scene ends never reach here.
        if (g_skyDrawsThisFrame > 0) { g_skyDrawsLastFrame = g_skyDrawsThisFrame; g_skyDrawsThisFrame = 0; }
        if (g_capState == 1)
        {
            char path[MAX_PATH]; GetTempPathA(MAX_PATH, path); strcat_s(path, "WheelmanMod_draws.txt");
            fopen_s(&g_capFile, path, "w");
            g_capIndex = 0;
            if (g_capFile) { g_capState = 2; strcpy_s(g_capStatus, "capturing one frame..."); }
            else { g_capState = 0; strcpy_s(g_capStatus, "cannot open the output file"); }
        }
        else if (g_capState == 2)
        {
            g_capState = 0;
            if (g_capFile) { fprintf(g_capFile, "-- end of frame: %d draw calls\n", g_capIndex); fclose(g_capFile); g_capFile = nullptr; }
            snprintf(g_capStatus, sizeof(g_capStatus), "done: %d draw calls in %%TEMP%%\\WheelmanMod_draws.txt", g_capIndex);
        }
    }

    HRESULT __stdcall hkEndSceneInner(IDirect3DDevice9* pDevice)
    {
        {
            static long total = 0, skipped = 0;
            ++total;
            if (!RenderTargetIsBackBuffer(pDevice))
            {
                if (++skipped == 1 || skipped % 2000 == 0) LogF("EndScene: %ld of %ld calls target an off-screen surface (skipped)", skipped, total);
                return oEndScene(pDevice);
            }
        }
        CaptureFrameBoundary();
        GfxBoost::OnEndScene(pDevice);
        PostFx::Apply(pDevice);   // over the finished game frame, before the overlay is drawn
        EnsureImGuiInitialized(pDevice);
        HandleHotkeys();

        // Loading protection: while the game loads a level or plays a cut-scene the mod stops WRITING into the game (no
        // ticks that change game memory, code patches switched off, no frame limiter). The overlay, the menu and the maps
        // stay: they only read, and what the user edits by hand in the menu is the user's own action.
        static PatchState savedPatches; static bool patchesSuspended = false;
        static bool wasQuiet = false;
        LoadGuard::Update();
        const bool quiet = LoadGuard::Quiet();
        // Throttle() is skipped entirely while quiet (see below), so its pacing baseline goes stale for as long as
        // that lasts; resync it the moment ticking resumes instead of leaving it to "catch up" from a large gap.
        // Found live, 2026-09-24: with loading protection turned off (so this transition, and the resync, never
        // happens) the frame limiter could end up not actually pacing anymore after a big frame-time spike, and the
        // game's own physics (tied to frame rate - see Perf.h) started showing its high-FPS bugs despite the target
        // still reading 60; toggling the limiter off and on by hand "fixed" it, which is exactly a manual resync.
        if (wasQuiet && !quiet) Perf::Resync();
        wasQuiet = quiet;
        if (quiet)
        {
            if (!patchesSuspended) { savedPatches = g_patches; g_patches = PatchState(); patchesSuspended = true; }
            if (!LoadGuard::Hard()) VehicleMod::KeepTopSpeed();   // slow motion only: the cap must not fall back
        }
        else
        {
            if (patchesSuspended) { g_patches = savedPatches; patchesSuspended = false; }
            VehicleMod::KeepTopSpeed();
            SyncGamePatches();
        }        UpdateViewportSize(pDevice);

        // Draw our own cursor instead of depending on the OS ShowCursor
        // refcount, which the game can (and does) fight every frame.
        ImGui::GetIO().MouseDrawCursor = Overlay::wantMouseCapture;

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        {
            // A minimised game window reports a tiny (or zero) client size; ImGui then squeezes every window into that
            // rectangle and the layout gets saved that way (the menu, speedometer and binds ended up in the top-left
            // corner after minimising). While the window is minimised / tiny keep the last normal display size.
            static float goodW = 1920.f, goodH = 1080.f;
            ImGuiIO& io = ImGui::GetIO();
            if (!IsIconic(g_hWnd) && io.DisplaySize.x >= 400.f && io.DisplaySize.y >= 300.f)
            {
                goodW = io.DisplaySize.x;
                goodH = io.DisplaySize.y;
            }
            else
                io.DisplaySize = ImVec2(goodW, goodH);
        }
        ImGui::NewFrame();

        Binds::Tick();
        MouseLook::Tick();   // direct mouse camera (checks the loading guard itself)
        if (!quiet)
        {
            Police::Tick();
            VehicleMod::Tick();
            BuiltinCheats::Tick();
            MapTools::markerWritesAllowed = Overlay::unsafeEnabled;
            MapTools::Tick();
            CrimeTools::Tick();
            KismetVars::Tick();
            Aimbot::Tick();
            NpcMod::Tick();
            WeaponMod::Tick();
            Overlay::PlayerTick();
        }

        if (Overlay::visible)
            Overlay::Draw();
        Overlay::DrawSpeedometer();
        Binds::DrawWindow(Overlay::visible);
        Overlay::DrawESPOverlay();
        MiniMap::SetDevice(pDevice);
        MiniMap::Draw();

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = oEndScene(pDevice);
        if (!quiet) Perf::Throttle();   // loading is not slowed down by the frame limiter
        return hr;
    }

    // ---- experimental "don't let the main loop crash the game" wrapper (see D3DHook.h) ------------------------------
    int g_mainLoopExceptionsCaught = 0;
    char g_mainLoopExceptionStatus[160] = "";
    DWORD g_lastExcCode = 0; void* g_lastExcAddr = nullptr;
    int MainLoopExceptionFilter(EXCEPTION_POINTERS* ep)
    {
        g_lastExcCode = ep->ExceptionRecord->ExceptionCode;
        g_lastExcAddr = ep->ExceptionRecord->ExceptionAddress;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HRESULT hkEndSceneGuarded(IDirect3DDevice9* pDevice)
    {
        __try
        {
            return hkEndSceneInner(pDevice);
        }
        __except (MainLoopExceptionFilter(GetExceptionInformation()))
        {
            ++g_mainLoopExceptionsCaught;
            snprintf(g_mainLoopExceptionStatus, sizeof(g_mainLoopExceptionStatus), "caught %08lX at %p (frame skipped, #%d)",
                     g_lastExcCode, g_lastExcAddr, g_mainLoopExceptionsCaught);
            LogF("hkEndScene: EXPERIMENTAL catch-all caught an exception - %s", g_mainLoopExceptionStatus);

            // A crash loop (the same broken state faulting again every single frame) would otherwise hang here
            // forever, silently, instead of the process just dying like it normally would - much worse than a
            // crash. If catches are coming in far faster than real frames could produce them, stop intercepting:
            // the next fault is then fatal again, same as if this feature had never been turned on.
            static ULONGLONG windowStart = 0; static int inWindow = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - windowStart > 1000) { windowStart = now; inWindow = 0; }
            if (++inWindow > 30)
            {
                g_catchMainLoopExceptions = false;
                LogF("hkEndScene: EXPERIMENTAL catch-all disabled itself - %d exceptions caught in under a second (probable crash loop)", inWindow);
            }
            return D3D_OK;
        }
    }

    HRESULT __stdcall hkEndScene(IDirect3DDevice9* pDevice)
    {
        // Only one thread at a time may run the overlay: during loading / cut-scenes the game ends scenes from several threads
        // (render thread, loading-movie thread) and the overlay and every tick below are not thread safe.
        static volatile long busy = 0;
        if (InterlockedCompareExchange(&busy, 1, 0) != 0) return oEndScene(pDevice);
        const HRESULT hr = g_catchMainLoopExceptions ? hkEndSceneGuarded(pDevice) : hkEndSceneInner(pDevice);
        InterlockedExchange(&busy, 0);
        return hr;
    }

    HRESULT __stdcall hkReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParams)
    {
        PostFx::ReleaseDeviceObjects();
        if (g_imguiInitialized)
            ImGui_ImplDX9_InvalidateDeviceObjects();

        HRESULT hr = oReset(pDevice, pParams);

        if (g_imguiInitialized)
            ImGui_ImplDX9_CreateDeviceObjects();

        return hr;
    }
}

void HookDeviceVTable(IDirect3DDevice9* pDevice)
{
    void** vTable = *reinterpret_cast<void***>(pDevice);
    oEndScene = reinterpret_cast<EndScene_t>(vTable[42]);
    oReset = reinterpret_cast<Reset_t>(vTable[16]);
    LogF("HookDeviceVTable: vTable=0x%p EndScene=0x%p Reset=0x%p", vTable, oEndScene, oReset);

    DWORD oldProtect;
    VirtualProtect(&vTable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vTable[42] = hkEndScene;
    VirtualProtect(&vTable[42], sizeof(void*), oldProtect, &oldProtect);

    VirtualProtect(&vTable[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vTable[16] = hkReset;
    VirtualProtect(&vTable[16], sizeof(void*), oldProtect, &oldProtect);

    oSetVertexShaderConstantF = reinterpret_cast<SetVertexShaderConstantF_t>(vTable[94]);
    VirtualProtect(&vTable[94], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vTable[94] = hkSetVertexShaderConstantF;
    VirtualProtect(&vTable[94], sizeof(void*), oldProtect, &oldProtect);

    oDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitive_t>(vTable[82]);
    VirtualProtect(&vTable[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vTable[82] = hkDrawIndexedPrimitive;
    VirtualProtect(&vTable[82], sizeof(void*), oldProtect, &oldProtect);
    oDrawPrimitive = reinterpret_cast<DrawPrimitive_t>(vTable[81]);
    VirtualProtect(&vTable[81], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vTable[81] = hkDrawPrimitive;
    VirtualProtect(&vTable[81], sizeof(void*), oldProtect, &oldProtect);

    GfxBoost::Install(vTable);
    PostFx::InstallHooks(vTable);

    LogF("HookDeviceVTable: patched EndScene(42), Reset(16), SetVertexShaderConstantF(94), DrawPrimitive(81), DrawIndexedPrimitive(82), SetSamplerState(69)");
}

void RequestFrameCapture() { if (g_capState == 0) { g_capState = 1; strcpy_s(g_capStatus, "armed: waiting for the next frame..."); } }
const char* FrameCaptureStatus() { return g_capStatus; }

int MainLoopExceptionsCaught() { return g_mainLoopExceptionsCaught; }
const char* MainLoopExceptionStatus() { return g_mainLoopExceptionStatus; }

bool GetCandidateViewProjMatrix(float outRowMajor16[16])
{
    if (!g_haveViewProjCandidate) return false;
    memcpy(outRowMajor16, g_viewProjCandidate, sizeof(float) * 16);
    return true;
}

void ShutdownOverlay()
{
    if (g_hWnd && oWndProc)
        SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));

    if (g_imguiInitialized)
    {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SR 3DGS OpenXR Ext VK - 3D Gaussian Splatting with OpenXR (Vulkan)
 *
 * Renders 3D Gaussian Splatting scenes on tracked 3D displays via OpenXR.
 * Based on cube_handle_vk with the cube/grid renderer replaced by
 * a 3DGS.cpp compute pipeline.  Features a "Load Scene" button as a
 * window-space layer overlay.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>   // DragAcceptFiles / DragQueryFileA / DragFinish (WM_DROPFILES)
#include <shlwapi.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "logging.h"
#include "input_handler.h"
#include "xr_session.h"
#include "gs_renderer_select.h"   // GsActiveRenderer = compute (x64) or graphics (_M_ARM64)
#include "gs_scene_loader.h"
#include "display3d_view.h"
#include "view_rig_math.h"
#include <openxr/XR_DXR_view_rig.h>

#include "hud_renderer.h"
#include "text_overlay.h"
#include "atlas_capture.h"
#include "auto_fit.h"         // dxr::AutoFitVHeight / FitTransition (shared width-aware framing)
#include "auto_fit_canvas.h"  // dxr::AutoFitCanvas — the runtime-resolved viewport (shell tile)
#include "vk_overlay_kit.h"   // dxr::CachedLayerUploader (#837 — no per-frame HUD upload+wait)
#include "vk_clickthrough_region.h" // dxr::ClickThroughRegion (#833 — transparent-mode punch-through)
#include "win_window_drag.h"        // dxr::RmbWindowDrag (move the borderless overlay)
// The "undock" launch contract (displayxr-common): one grammar + one security
// policy shared by every DisplayXR viewer a web page or a CAD app can spawn.
#include "launch_args.h"    // dxr::ParseLaunchArgsFromCommandLine -> dxr::LaunchArgs
#include "url_fetch.h"      // dxr::FetchUrlToCache — --src=<url> into the per-user cache
#include "view_protocol.h"  // displayxr-view: registration, sibling forward, single instance
#include "toast.h"          // dxr::ToastState — the only UI a shaped, chrome-free window has

#include <atomic>
#include <cstdarg>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace DirectX;

static const char* APP_NAME = "gaussian_splatting_handle_vk_win";

static const wchar_t* WINDOW_CLASS = L"SR3DGSOpenXRExtVKClass";
static const wchar_t* WINDOW_TITLE = L"DisplayXR Gaussian Splat Viewer Demo";

// HUD overlay fractions. Layer spans full window height so chrome buttons
// can sit at the window top while the info panel anchors to the bottom-left
// (matching the macOS demo's split). The vk_native compositor now uses an
// alpha-blended draw pass for window-space layers, so the empty middle of
// the texture stays invisible. Font sizing is anchored to the legacy
// 0.5-fraction so text doesn't grow with the taller texture.
static const float HUD_WIDTH_FRACTION = 0.30f;
static const float HUD_HEIGHT_FRACTION = 1.0f;
static const float HUD_FONT_BASE_FRACTION = 0.50f;

// Top-bar buttons live inside the HUD overlay's window-space footprint
// (left strip of the window, fracW × fracH = HUD_WIDTH_FRACTION × HUD_HEIGHT_FRACTION).
// All values are absolute window-fractions for hit-testing; they're
// translated into HUD-pixel coordinates when passed to RenderHudAndMap.
// Button positions in HWND fractions. Now that the runtime no longer
// applies a forwarding-rect inset (see displayxr-runtime fix), buttons
// can sit close to the edge without clicks being eaten.
static const float OPEN_BTN_X_FRACTION = 0.010f;
static const float OPEN_BTN_Y_FRACTION = 0.010f;
static const float OPEN_BTN_WIDTH_FRACTION  = 0.060f;
static const float OPEN_BTN_HEIGHT_FRACTION = 0.030f;

static const float MODE_BTN_X_FRACTION = 0.075f;
static const float MODE_BTN_Y_FRACTION = 0.010f;
static const float MODE_BTN_WIDTH_FRACTION  = 0.140f;
static const float MODE_BTN_HEIGHT_FRACTION = 0.030f;


// sim_display output mode switching (legacy — replaced by unified rendering mode)
typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

// Global state
static InputState g_inputState;
// Standalone demo: bare TAB toggles the HUD (displayxr::common defaults to
// SHIFT+TAB so runtime test apps don't shadow the workspace shell's
// focus-cycle binding).
static const bool g_inputInit = [] {
    g_inputState.hudToggleRequiresShift = false;
    return true;
}();
static std::mutex g_inputMutex;
static std::atomic<bool> g_running{true};
static XrSessionManager* g_xr = nullptr;
static UINT g_windowWidth = 1280;
static UINT g_windowHeight = 720;
// Published once the app window exists so the auto-fit viewport can be read
// live (GetClientRect) instead of trusting the WM_SIZE-updated statics — the
// bundled auto-load runs before ShowWindow.
static HWND g_appWindow = nullptr;

// 3DGS state
static GsActiveRenderer g_gsRenderer;
// Cross-thread scene-load queue: the file dialog runs on the main (message-pump)
// thread, but the actual GsRenderer::loadScene() submits Vulkan work on the
// graphics queue and so MUST run on the same thread that drives per-frame
// rendering — otherwise concurrent vkQueueSubmit/vkQueueWaitIdle from two
// threads on a single VkQueue is undefined behaviour and crashes some drivers
// (NVIDIA in particular). Main thread posts the picked path here; the render
// thread picks it up between frames.
static std::atomic<bool> g_loadRequested{false};
static std::string g_pendingLoadPath;
static std::mutex g_pendingLoadPathMutex;
// 'I' key: capture the multi-view atlas region (cols × rows × renderW × renderH)
// of the swapchain to a PNG in %USERPROFILE%\Pictures\DisplayXR\. Skipped for
// 1×1 (mono) layouts. Helper lives in test_apps/common/atlas_capture*.
static std::atomic<bool> g_captureAtlasRequested{false};
// Ctrl+T: opaque ⇄ transparent background. Always-on session-level
// transparency is wired at xrCreateSession; this flag only flips the
// renderer's output alpha (1 → 1-T) so background-uncovered pixels
// punch through to the desktop.
static std::atomic<bool> g_transparentBg{false};
// #833 punch-through (see docs/architecture/transparency-modes.md in the
// runtime repo): under baked composition (DXR_PRESENT_OPAQUE) transparent
// mode must CARVE the uncovered pixels out of the window and go borderless
// (a shaped NRB window can never paint a frame). kBorderlessMsg runs the
// style swap on the window-owning thread.
static const UINT kBorderlessMsg = WM_APP + 0x33;
static std::atomic<bool> g_borderless{false};
static dxr::ClickThroughRegion g_punch; // render-thread owned
static dxr::RmbWindowDrag g_windowDrag; // window-thread owned (WndProc)

// ── Undock launch contract state ─────────────────────────────────────────────
// Parsed once at the top of WinMain and then read-only. A second launch does
// NOT mutate it: WM_COPYDATA parses its own LaunchArgs and applies it live, so
// there is no shared mutable launch state to lock. Everything the render thread
// needs out of the launch is mirrored into an atomic below.
static dxr::LaunchArgs g_launch;
// Held for the process lifetime when this is the instance that owns the
// protocol scheme; releasing it would let a second launch open a rival window.
static HANDLE g_singleInstanceMutex = nullptr;
// A --src (local or URL) replaces the bundled butterfly.spz. For a URL the
// scene arrives seconds later on the fetch thread, so the auto-load must be
// suppressed rather than raced.
static std::atomic<bool> g_suppressBundledAutoLoad{false};
// --vh pins the virtual display height the asset was authored at. It is the
// launcher's statement about the ASSET, so it outranks the auto-fit rule (which
// only guesses from bounds) — but not the user's own zoom, which is a separate
// multiplier.
static std::atomic<bool> g_vhPinned{false};
static std::atomic<float> g_vhPinnedValue{0.0f};
// One download at a time; a second URL arriving over WM_COPYDATA while one is
// in flight is refused with a toast rather than queued.
static std::atomic<bool> g_fetchInFlight{false};

// The only UI a --transparent window has: chrome is hidden by design in
// transparent mode (#833), so a download that reported nothing would be a
// window that sits empty for 20 seconds with no explanation.
static dxr::ToastState g_toast;
// ASCII in / wide out — every message this app produces is ASCII, so %hs
// widening avoids a locale dependency (same helper as the avatar demo).
static void ToastF(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wchar_t w[224];
    swprintf(w, 224, L"%hs", buf);
    g_toast.Show(w);
}

/*!
 * DXR_LAUNCH_QUIET=1 suppresses every launch-time MessageBoxW (a refused URL,
 * a missing sibling viewer) and leaves the log line + the process exit code as
 * the whole report.
 *
 * A refusal is MODAL: the process sits there until a human clicks OK. That is
 * the right behaviour for a browser-initiated launch — nothing appeared, and
 * the user deserves to know why — but it makes the negative cases untestable
 * without a person, and an automated run that trips one leaves a dialog sitting
 * on the panel. GetEnvironmentVariableW, not getenv: the CRT's environment
 * snapshot is not the one a `set VAR=1 && app.exe` parent handed us.
 *
 * Exit codes are the machine-readable half and do not change: 2 = the launch
 * was refused, 3 = the URL belongs to a sibling viewer that is not installed.
 */
static bool LaunchQuiet() {
    wchar_t buf[16] = {};
    const DWORD n = GetEnvironmentVariableW(L"DXR_LAUNCH_QUIET", buf, 16);
    return n > 0 && n < 16 && buf[0] != L'\0' && buf[0] != L'0';
}

static void LaunchMessageBox(const std::wstring& text, UINT icon) {
    if (LaunchQuiet()) {
        LOG_WARN("DXR_LAUNCH_QUIET=1 - dialog suppressed: %ls", text.c_str());
        return;
    }
    MessageBoxW(nullptr, text.c_str(), L"DisplayXR Gaussian Splat Viewer", MB_OK | icon);
}

// Toast window-space layer resources (render-thread owned after creation).
// Sized to the chip, NOT the main HUD: RenderToastStandalone fills its whole
// texture with one pill.
static const uint32_t TOAST_TEX_W = 768;
static const uint32_t TOAST_TEX_H = 96;
static const uint32_t TOAST_FONT_BASE = TOAST_TEX_H * 11;  // ~45px glyphs in a 96px pill
static const float    TOAST_SIZE_FRACTION = 0.60f;         // of the shorter window side
static const float    TOAST_Y_FRACTION = 0.84f;
static SwapchainInfo  g_toastSwapchain;
static bool           g_hasToastSwapchain = false;
static HudRenderer    g_toastHud = {};
static bool           g_toastReady = false;
static std::vector<XrSwapchainImageVulkanKHR> g_toastSwapImages;
static std::string g_loadedFileName;
static std::mutex g_sceneMutex;

// Fallback vHeight when no scene is loaded or auto-fit hits a degenerate
// extent. Matches macOS demo's kDefaultVirtualDisplayHeightM (1.5m).
static constexpr float kFallbackVirtualDisplayHeightM = 1.5f;
// Fill fraction handed to dxr::AutoFitVHeight. The shared rule's default is
// 0.80 (asset spans 80% of the viewport on its binding axis), but
// getMainObjectBounds already bakes a 1.10x comfort margin into all three
// extent axes, so the extents we pass are 1.10x the true object size. Pass
// 0.88 = 0.80 x 1.10 to cancel that and net an 80% cap of the TRUE object.
// (Do not "fix" this by unbaking the comfort in getMainObjectBounds — that
// multiplier differs single-object vs scene and is shared with other legs.)
static constexpr float kAutoFitFill = 0.88f;

// Cached auto-fit pose for the currently loaded scene. Reused by Reset
// so 'Space' returns to the framed pose rather than world origin.
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight   = kFallbackVirtualDisplayHeightM;
static float g_fitYaw       = 0.0f;
static std::atomic<bool> g_fitValid{false};

// Refit state (displayxr-common common/auto_fit_canvas.h). vHeight is a
// function of (content, viewport), so the CONTENT half is cached here and the
// viewport half is re-read every frame — see RefitForViewport below for why
// the load-time fit alone is not enough under the shell.
static std::atomic<float> g_fitExtentW{0.0f};  //!< content — survives viewport changes
static std::atomic<float> g_fitExtentH{0.0f};
static std::atomic<float> g_fitAspect{0.0f};   //!< viewport the current base was derived for
static dxr::AutoFitCanvas g_autoFitCanvas;     //!< runtime-resolved canvas, published post-locate
static dxr::FitTransition g_fitTransition;     //!< render-thread only

// Latest computed frames-per-second, published each frame by the render thread
// (after UpdatePerformanceStats) so the XR_DXR_mcp_tools get_status handler can
// report it without reaching into the render thread's local PerformanceStats.
static std::atomic<float> g_currentFps{0.0f};

// Viewport the auto-fit width rule frames against. Only the aspect ratio
// matters to dxr::AutoFitVHeight, so pixels and meters mix freely.
//
// The app's own client rect is NOT the viewport under the shell. A
// shell-launched app is composed into a 3D window tile the shell owns; this
// window is hidden (SW_HIDE) and is never what the user sees, and the runtime
// only resizes it to the tile later — deferred and async, once the client is
// placed. The bundled scene auto-loads during init, long before that, so this
// used to fit the 1280x720 CREATION size on every run, shell or not (the
// "viewport=1280x720 aspect=1.778" in every log). In a square tile the
// butterfly then came out ~15% oversized and overflowed the sides.
//
// So: prefer the canvas the runtime RESOLVED (XR_DXR_view_rig's raw channel —
// the shell tile under a workspace, this window's client rect standalone), and
// keep the client rect only as the bootstrap for the fit that runs before the
// first locate. RefitForViewport re-derives once the real canvas arrives.
// Returns true when the dims came from the runtime canvas (metres) rather than
// the client-rect bootstrap (pixels) — the two are not comparable numbers, only
// their aspects are, so anything logging them must say which it got.
static bool GetAutoFitViewport(float& outW, float& outH) {
    float fallbackW = (float)g_windowWidth;
    float fallbackH = (float)g_windowHeight;
    RECT client = {};
    if (g_appWindow && GetClientRect(g_appWindow, &client) &&
        client.right > client.left && client.bottom > client.top) {
        fallbackW = (float)(client.right - client.left);
        fallbackH = (float)(client.bottom - client.top);
    }
    return g_autoFitCanvas.Viewport(fallbackW, fallbackH, outW, outH);
}

// Compute robust scene bounds (5th–95th percentile per axis) and stage
// new display-rig pose + vHeight on g_inputState. Display orientation is
// kept identity (forward = world −Z): splats have no canonical front, and
// any heuristic (PCA, etc.) can pick the wrong side; the user can rotate
// with mouse drag from a predictable starting pose.
// Caller must hold g_sceneMutex (we read pickData_ from the renderer).
static void ApplyAutoFitForLoadedScene_locked() {
    float center[3], extent[3];
    // Voxel-density flood-fill — see the macOS demo for rationale.
    bool ok = g_gsRenderer.getMainObjectBounds(64u, center, extent);
    if (ok) {
        g_fitCenter[0] = center[0];
        g_fitCenter[1] = center[1];
        g_fitCenter[2] = center[2];
        // Shared width-aware rule (displayxr-common common/auto_fit.h):
        // vHeight = max(H, W / aspect) / fill — the scene caps at `fill` of
        // the viewport in BOTH axes, so a wide scene no longer overflows
        // horizontally. See kAutoFitFill for why fill is 0.88, not 0.80.
        float viewportW = 0.0f, viewportH = 0.0f;
        const bool fromCanvas = GetAutoFitViewport(viewportW, viewportH);
        float vh = dxr::AutoFitVHeight(extent[0], extent[1], viewportW, viewportH, kAutoFitFill);
        // Degenerate scene (all splats in a thin slice) — fall back to a
        // sensible vHeight rather than failing the fit. Mirrors macOS:1399.
        if (!(vh > 1e-3f)) vh = kFallbackVirtualDisplayHeightM;
        // --vh wins over the guess. The launcher knows the metres the asset was
        // authored at; auto-fit only infers them from the bounding box, so
        // letting the fit overwrite an explicit --vh would silently discard the
        // one number the caller actually knew.
        if (g_vhPinned.load(std::memory_order_relaxed)) {
            const float pinned = g_vhPinnedValue.load(std::memory_order_relaxed);
            if (pinned > 1e-3f) {
                LOG_INFO("Auto-fit vHeight %.3f overridden by --vh=%.3f", vh, pinned);
                vh = pinned;
            }
        }
        g_fitVHeight = vh;
        // Cache the CONTENT half of the fit (extents are scene properties) and
        // the viewport this base was derived for, so RefitForViewport can
        // re-derive on an aspect change without re-measuring the splats.
        g_fitExtentW.store(extent[0], std::memory_order_relaxed);
        g_fitExtentH.store(extent[1], std::memory_order_relaxed);
        g_fitAspect.store((viewportH > 0.0f) ? (viewportW / viewportH) : 0.0f,
                          std::memory_order_relaxed);
        // A load lands the base immediately — the framing IS the load's result,
        // so there is nothing to animate away from.
        g_fitTransition.start(vh, vh, 0.0f);
        // Anchor at yaw=0 and trust the loader's RUB convention (PLY loader
        // converts RDF+X-mirror → RUB at load time; SPZ is RUB-native and
        // SuperSplat-authored scenes already face −Z at yaw=0). Matches
        // macOS:1407 — the user can drag with LMB if a particular asset's
        // authored orientation is off.
        g_fitYaw = 0.0f;
        const float aspect = (viewportH > 0.0f) ? (viewportW / viewportH) : 0.0f;
        const bool widthBound = (aspect > 0.0f) && (extent[0] / aspect > extent[1]);
        LOG_INFO("Auto-fit: center=(%.3f, %.3f, %.3f) extent=(%.3f, %.3f, %.3f) "
                 "viewport=%.3fx%.3f (%s) aspect=%.3f bound=%s fill=%.2f vHeight=%.3f yaw=%.0fdeg",
                 center[0], center[1], center[2],
                 extent[0], extent[1], extent[2],
                 viewportW, viewportH, fromCanvas ? "runtime canvas, m" : "client rect, px",
                 aspect, widthBound ? "width" : "height",
                 kAutoFitFill, vh, g_fitYaw * 57.2957795f);
    }
    g_fitValid.store(ok);

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputState.cameraPosX = ok ? g_fitCenter[0] : 0.0f;
    g_inputState.cameraPosY = ok ? g_fitCenter[1] : 0.0f;
    g_inputState.cameraPosZ = ok ? g_fitCenter[2] : 0.0f;
    g_inputState.yaw = ok ? g_fitYaw : 0.0f;
    g_inputState.pitch = 0.0f;
    g_inputState.viewParams.virtualDisplayHeight = ok ? g_fitVHeight : kFallbackVirtualDisplayHeightM;
    g_inputState.viewParams.scaleFactor = 1.0f;

    // Per-format orientation correction is now done at load time (PLY loader
    // converts RDF+X-mirror → canonical RUB; SPZ loader uses RUB natively).
    // Renderer's GsRenderer::updateUniforms negates the Y row of proj_mat to
    // match the +Y-up convention. No runtime view-stage flips needed.

    // Route the first post-load frame through the same reset path Space uses,
    // so app-start view params (perspectiveFactor, scaleFactor, etc.) match
    // the Space-reset state.
    g_inputState.resetViewRequested = true;

    // Treat scene load as a fresh user interaction so the auto-orbit idle
    // timer restarts. Without this, an asset loaded after the 10s idle
    // threshold starts rotating immediately on first display.
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
        g_inputState.animationActive = false;
    }
}

// Re-derive the base vHeight when the viewport's ASPECT changes, and animate
// the move. Render thread only.
//
// This is what makes the load-time fit correct under the shell. The scene
// auto-loads during init, before the first xrLocateViews, so the only viewport
// available then is this window's own client rect — which under a workspace is
// the hidden creation-size window, not the tile the user sees. The first
// located frame publishes the real canvas, the aspect gate sees it differ from
// the bootstrap one, and the fit lands on the tile. The same path handles a
// live 3D-window resize, which mis-framed identically before.
//
// Only the BASE moves: the render path computes rigVH = virtualDisplayHeight /
// scaleFactor, so the user's zoom stays relative (2x of the old fit becomes 2x
// of the new one) and orbit/pivot are untouched. A viewport change is not a
// request to undo deliberate user state — recentring belongs on Space.
static void RefitForViewport(float dtSeconds) {
    if (!g_fitValid.load(std::memory_order_relaxed)) {
        return;
    }
    // A pinned --vh is an absolute statement in metres, so it does not follow
    // the viewport. Returning before the transition update also leaves the
    // load-time value in place rather than animating away from it.
    if (g_vhPinned.load(std::memory_order_relaxed)) {
        return;
    }
    const float extW = g_fitExtentW.load(std::memory_order_relaxed);
    const float extH = g_fitExtentH.load(std::memory_order_relaxed);
    if (!(extH > 0.0f)) {
        return;
    }

    float vpW = 0.0f, vpH = 0.0f;
    const bool fromCanvas = GetAutoFitViewport(vpW, vpH);
    const float aspect = (vpH > 0.0f) ? (vpW / vpH) : 0.0f;
    if (dxr::AutoFitAspectChanged(g_fitAspect.load(std::memory_order_relaxed), aspect)) {
        const float vh = dxr::AutoFitVHeight(extW, extH, vpW, vpH, kAutoFitFill);
        if (vh > 1e-3f) {
            // Retarget rather than restart: a resize that settles in two steps
            // must not snap back to where it started.
            g_fitTransition.start(g_fitTransition.value(), vh);
            const float prev = g_fitVHeight;
            g_fitAspect.store(aspect, std::memory_order_relaxed);
            g_fitVHeight = vh;  // Space-reset target follows the live viewport
            LOG_INFO("Auto-fit refit: viewport=%.3fx%.3f (%s) aspect=%.3f bound=%s "
                     "base %.3f -> %.3f (zoom preserved)",
                     vpW, vpH, fromCanvas ? "runtime canvas, m" : "client rect, px", aspect,
                     (aspect > 0.0f && extW / aspect > extH) ? "width" : "height",
                     prev, vh);
        }
    }

    float animated = 0.0f;
    if (g_fitTransition.update(dtSeconds, &animated)) {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_inputState.viewParams.virtualDisplayHeight = animated;
    }
}

// ============================================================================
// Agent tools (XR_DXR_mcp_tools, #66) — dispatch, wired to the Windows viewer
// ============================================================================
//
// Windows port of the macOS build's HandleMcpToolCall. RegisterAgentTools()
// (windows/xr_session.cpp) declares the appId + tools and installs this as
// xr.mcpToolHandler; displayxr-common's shared PollEvents (v2.1.0 / #18) then
// fetches the call args, invokes this to map (toolName, argsJson) -> resultJson,
// and submits the result — so this must NOT call xrGetMCPToolCallArgsDXR /
// xrSubmitMCPToolResultDXR itself. It runs on the render thread (from
// PollEvents), so it mutates g_inputState under g_inputMutex and touches the
// renderer/scene state under g_sceneMutex — the same locks the render loop uses.
// The tool set, descriptions, schemas, and result JSON key shapes are identical
// to macOS (macos/main.mm).

static constexpr float kMcpRad2Deg   = 57.2957795f;
static constexpr float kMcpDeg2Rad   = 0.0174532925f;
static constexpr float kMcpMaxPitchRad = 1.5f;  // same clamp as mouse drag

// Find the value position of `"key" :` in a flat JSON object. Returns nullptr if
// absent. Does not handle keys nested inside sub-objects or string values — the
// registered schemas keep all arguments top-level.
static const char* JsonFindValue(const char* json, const char* key) {
    char quoted[96];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char* p = json;
    while ((p = strstr(p, quoted)) != nullptr) {
        const char* q = p + strlen(quoted);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            return q;
        }
        p = q;
    }
    return nullptr;
}

static bool JsonGetNumber(const char* json, const char* key, double* out) {
    const char* v = JsonFindValue(json, key);
    if (!v) return false;
    char* end = nullptr;
    double d = strtod(v, &end);
    if (end == v) return false;
    *out = d;
    return true;
}

static bool JsonGetBool(const char* json, const char* key, bool* out) {
    const char* v = JsonFindValue(json, key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

// Extract + unescape a JSON string value. Handles the standard escapes and BMP
// \uXXXX (encoded back to UTF-8; surrogate halves degrade to '?').
static bool JsonGetString(const char* json, const char* key, std::string* out) {
    const char* v = JsonFindValue(json, key);
    if (!v || *v != '"') return false;
    out->clear();
    for (const char* c = v + 1; *c; c++) {
        if (*c == '"') return true;
        if (*c != '\\') { out->push_back(*c); continue; }
        c++;
        switch (*c) {
            case '"':  out->push_back('"');  break;
            case '\\': out->push_back('\\'); break;
            case '/':  out->push_back('/');  break;
            case 'n':  out->push_back('\n'); break;
            case 't':  out->push_back('\t'); break;
            case 'r':  out->push_back('\r'); break;
            case 'b':  out->push_back('\b'); break;
            case 'f':  out->push_back('\f'); break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = c[i];
                    if (!isxdigit((unsigned char)h)) return false;
                    cp = cp * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0'
                                                                        : (tolower(h) - 'a' + 10));
                }
                c += 4;
                if (cp < 0x80) {
                    out->push_back((char)cp);
                } else if (cp < 0x800) {
                    out->push_back((char)(0xC0 | (cp >> 6)));
                    out->push_back((char)(0x80 | (cp & 0x3F)));
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    out->push_back('?'); // unpaired surrogate
                } else {
                    out->push_back((char)(0xE0 | (cp >> 12)));
                    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out->push_back((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return false; // invalid escape
        }
    }
    return false; // unterminated string
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

// Monotonic wall-clock seconds — matches the idle-timer stamp ApplyAutoFit uses.
static double McpNowSec() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

// Camera sub-object shared by get_status and set_camera responses. Locks
// g_inputMutex (the render loop / message pump both touch g_inputState); do NOT
// call while already holding it.
static std::string McpCameraJson() {
    float px, py, pz, yaw, pitch, zoom;
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        px = g_inputState.cameraPosX;
        py = g_inputState.cameraPosY;
        pz = g_inputState.cameraPosZ;
        yaw = g_inputState.yaw;
        pitch = g_inputState.pitch;
        zoom = g_inputState.viewParams.scaleFactor;
    }
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"position\":[%.4f,%.4f,%.4f],\"yaw_deg\":%.2f,\"pitch_deg\":%.2f,\"zoom\":%.3f}",
        px, py, pz, yaw * kMcpRad2Deg, pitch * kMcpRad2Deg, zoom);
    return buf;
}

std::string HandleAgentToolCall(XrSessionManager& xr, const std::string& toolName,
                                const std::string& argsJson, bool& success) {
    // PollEvents already fetched the JSON args (empty string == no args) and
    // will submit whatever we return. A no-arg tool gets ""; the flat-JSON
    // helpers below simply find no keys, matching the old "{}" default.
    const char* a = argsJson.c_str();
    const char* toolName_c = toolName.c_str();

    bool ok = true;
    std::string result;
    char buf[768];

    if (strcmp(toolName_c, "load_splat") == 0) {
        std::string path;
        if (!JsonGetString(a, "path", &path) || path.empty()) {
            ok = false;
            result = "{\"error\":\"missing required string arg 'path'\"}";
        } else if (!ValidateSceneFile(path)) {
            ok = false;
            result = "{\"error\":\"not a readable .ply/.spz scene: '" + JsonEscape(path) + "'\"}";
        } else {
            // On the render thread (same thread that drives per-frame Vulkan
            // submits), so loadScene() is safe here — mirror the render loop's
            // queued-load block: hold g_sceneMutex across loadScene + auto-fit.
            std::lock_guard<std::mutex> lock(g_sceneMutex);
            if (!g_gsRenderer.loadScene(path.c_str())) {
                ok = false;
                result = "{\"error\":\"failed to load '" + JsonEscape(path) + "' (corrupt or unsupported)\"}";
            } else {
                g_loadedFileName = GetPlyFilename(path);
                ApplyAutoFitForLoadedScene_locked();
                snprintf(buf, sizeof(buf), "\",\"splat_count\":%u}", g_gsRenderer.gaussianCount());
                result = "{\"file\":\"" + JsonEscape(path) + buf;
                LOG_INFO("Agent loaded scene: %s (%u splats)",
                         g_loadedFileName.c_str(), g_gsRenderer.gaussianCount());
            }
        }
    } else if (strcmp(toolName_c, "get_status") == 0) {
        const char* modeName = (xr.renderingModeCount > 0 &&
            xr.currentModeIndex < xr.renderingModeCount &&
            xr.renderingModeNames[xr.currentModeIndex][0] != '\0')
            ? xr.renderingModeNames[xr.currentModeIndex] : "unknown";
        double fps = (double)g_currentFps.load(std::memory_order_relaxed);
        std::string cameraJson = McpCameraJson();
        float vHeight; bool autoOrbit; bool orbitingNow;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            vHeight = g_inputState.viewParams.virtualDisplayHeight;
            autoOrbit = g_inputState.animateEnabled;
            orbitingNow = g_inputState.animationActive;
        }
        snprintf(buf, sizeof(buf),
            "\",\"has_scene\":%s,\"splat_count\":%u,\"fps\":%.1f,\"camera\":%s,"
            "\"virtual_display_height_m\":%.4f,"
            "\"rendering_mode\":{\"index\":%u,\"name\":\"%s\"},"
            "\"transparent_background\":%s,\"auto_orbit\":%s,\"orbiting_now\":%s,"
            "\"session_running\":%s}",
            g_gsRenderer.hasScene() ? "true" : "false",
            g_gsRenderer.gaussianCount(), fps, cameraJson.c_str(),
            vHeight, xr.currentModeIndex, modeName,
            g_transparentBg.load() ? "true" : "false",
            autoOrbit ? "true" : "false",
            orbitingNow ? "true" : "false",
            xr.sessionRunning ? "true" : "false");
        result = "{\"file\":\"" + JsonEscape(g_loadedFileName) + buf;
    } else if (strcmp(toolName_c, "set_camera") == 0) {
        double v;
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            if (JsonGetNumber(a, "position_x", &v)) { g_inputState.cameraPosX = (float)v; any = true; }
            if (JsonGetNumber(a, "position_y", &v)) { g_inputState.cameraPosY = (float)v; any = true; }
            if (JsonGetNumber(a, "position_z", &v)) { g_inputState.cameraPosZ = (float)v; any = true; }
            if (JsonGetNumber(a, "yaw_deg", &v))    { g_inputState.yaw = (float)(v * kMcpDeg2Rad); any = true; }
            if (JsonGetNumber(a, "pitch_deg", &v)) {
                float p = (float)(v * kMcpDeg2Rad);
                if (p > kMcpMaxPitchRad) p = kMcpMaxPitchRad;
                if (p < -kMcpMaxPitchRad) p = -kMcpMaxPitchRad;
                g_inputState.pitch = p;
                any = true;
            }
            if (JsonGetNumber(a, "zoom", &v)) {
                float z = (float)v;
                if (z < 0.1f) z = 0.1f;
                g_inputState.viewParams.scaleFactor = z;
                any = true;
            }
            if (any) {
                g_inputState.transitioning = false;  // agent pose wins over a running focus transition
                g_inputState.lastInputTimeSec = McpNowSec();  // restart auto-orbit idle countdown
                g_inputState.animationActive = false;
            }
        }
        if (!any) {
            ok = false;
            result = "{\"error\":\"no recognized args (position_x/y/z, yaw_deg, pitch_deg, zoom)\"}";
        } else {
            result = "{\"camera\":" + McpCameraJson() + "}";
        }
    } else if (strcmp(toolName_c, "orbit") == 0) {
        double az = 0.0, el = 0.0;
        bool hasAz = JsonGetNumber(a, "azimuth_deg", &az);
        bool hasEl = JsonGetNumber(a, "elevation_deg", &el);
        if (!hasAz && !hasEl) {
            ok = false;
            result = "{\"error\":\"need azimuth_deg and/or elevation_deg\"}";
        } else {
            float yawDeg, pitchDeg;
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_inputState.yaw += (float)(az * kMcpDeg2Rad);
                float p = g_inputState.pitch + (float)(el * kMcpDeg2Rad);
                if (p > kMcpMaxPitchRad) p = kMcpMaxPitchRad;
                if (p < -kMcpMaxPitchRad) p = -kMcpMaxPitchRad;
                g_inputState.pitch = p;
                g_inputState.transitioning = false;
                g_inputState.lastInputTimeSec = McpNowSec();
                g_inputState.animationActive = false;
                yawDeg = g_inputState.yaw * kMcpRad2Deg;
                pitchDeg = g_inputState.pitch * kMcpRad2Deg;
            }
            snprintf(buf, sizeof(buf), "{\"yaw_deg\":%.2f,\"pitch_deg\":%.2f}", yawDeg, pitchDeg);
            result = buf;
        }
    } else if (strcmp(toolName_c, "reset_camera") == 0) {
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.resetViewRequested = true;  // applied by the render loop next frame
        }
        bool fitValid = g_fitValid.load();
        snprintf(buf, sizeof(buf),
            "{\"framed\":%s,\"position\":[%.4f,%.4f,%.4f],\"yaw_deg\":%.2f,"
            "\"virtual_display_height_m\":%.4f}",
            fitValid ? "true" : "false",
            fitValid ? g_fitCenter[0] : 0.0f,
            fitValid ? g_fitCenter[1] : 0.0f,
            fitValid ? g_fitCenter[2] : 0.0f,
            (fitValid ? g_fitYaw : 0.0f) * kMcpRad2Deg,
            fitValid ? g_fitVHeight : kFallbackVirtualDisplayHeightM);
        result = buf;
    } else if (strcmp(toolName_c, "set_auto_orbit") == 0) {
        bool enabled = false;
        if (!JsonGetBool(a, "enabled", &enabled)) {
            ok = false;
            result = "{\"error\":\"missing required boolean arg 'enabled'\"}";
        } else {
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_inputState.animateEnabled = enabled;
                g_inputState.animationActive = false;
                g_inputState.lastInputTimeSec = McpNowSec(); // don't snap-start on enable
            }
            result = enabled ? "{\"auto_orbit\":true}" : "{\"auto_orbit\":false}";
        }
    } else if (strcmp(toolName_c, "set_transparent_background") == 0) {
        // Same code path as Ctrl+T: raise the shared input handler's request
        // flag and let the render loop stay the single place that flips
        // g_transparentBg, logs, and posts kBorderlessMsg. Omitting 'enabled'
        // toggles; passing it makes the call idempotent — the flag is raised
        // only when the target differs from the live state.
        const bool current = g_transparentBg.load();
        bool want = !current;                 // no 'enabled' -> toggle, like Ctrl+T
        JsonGetBool(a, "enabled", &want);     // absent/non-boolean keeps the toggle target
        const bool changing = (want != current);
        if (changing) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.transparentBgToggleRequested = true;
        }
        snprintf(buf, sizeof(buf), "{\"transparent_background\":%s,\"changed\":%s}",
                 want ? "true" : "false", changing ? "true" : "false");
        result = buf;
    } else {
        ok = false;
        result = std::string("{\"error\":\"unhandled tool '") + toolName + "'\"}";
    }

    success = ok;
    return result;
}

// Fullscreen state
static bool g_fullscreen = false;
static RECT g_savedWindowRect = {};
static DWORD g_savedWindowStyle = 0;

static void ToggleFullscreen(HWND hwnd) {
    // Same rule as kBorderlessMsg below: a style swap must never change whether
    // this window is on screen. Under the DisplayXR workspace the runtime hides
    // the HWND at xrCreateSession, so preserve the WS_VISIBLE / WS_MINIMIZE bits
    // and only touch the z-order while the window is actually on screen.
    const LONG cur = GetWindowLong(hwnd, GWL_STYLE);
    const LONG keep = cur & (WS_VISIBLE | WS_MINIMIZE);
    const bool onScreen = (cur & WS_VISIBLE) != 0 && (cur & WS_MINIMIZE) == 0;
    HWND zOrder = onScreen ? HWND_TOP : nullptr;
    UINT swpFlags = SWP_FRAMECHANGED | (onScreen ? 0u : SWP_NOZORDER);
    if (g_fullscreen) {
        SetWindowLong(hwnd, GWL_STYLE,
            (LONG)((g_savedWindowStyle & ~(DWORD)(WS_VISIBLE | WS_MINIMIZE)) | (DWORD)keep));
        SetWindowPos(hwnd, zOrder,
            g_savedWindowRect.left, g_savedWindowRect.top,
            g_savedWindowRect.right - g_savedWindowRect.left,
            g_savedWindowRect.bottom - g_savedWindowRect.top,
            swpFlags);
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen mode");
    } else {
        g_savedWindowStyle = (DWORD)cur;
        GetWindowRect(hwnd, &g_savedWindowRect);

        HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMonitor, &mi);

        SetWindowLong(hwnd, GWL_STYLE, (LONG)(WS_POPUP | (DWORD)keep));
        SetWindowPos(hwnd, zOrder,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            swpFlags);
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen mode");
    }
}

static bool PointInFractionRect(int mouseX, int mouseY, int windowW, int windowH,
                                float xf, float yf, float wf, float hf) {
    if (windowW <= 0 || windowH <= 0) return false;
    float fx = (float)mouseX / (float)windowW;
    float fy = (float)mouseY / (float)windowH;
    return (fx >= xf && fx <= xf + wf && fy >= yf && fy <= yf + hf);
}

static bool IsClickOnLoadButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        OPEN_BTN_X_FRACTION, OPEN_BTN_Y_FRACTION,
        OPEN_BTN_WIDTH_FRACTION, OPEN_BTN_HEIGHT_FRACTION);
}

static bool IsClickOnModeButton(int mouseX, int mouseY, int windowW, int windowH) {
    return PointInFractionRect(mouseX, mouseY, windowW, windowH,
        MODE_BTN_X_FRACTION, MODE_BTN_Y_FRACTION,
        MODE_BTN_WIDTH_FRACTION, MODE_BTN_HEIGHT_FRACTION);
}

// Atlas capture is runtime-owned via xrCaptureAtlasDXR (XR_DXR_atlas_capture).
// App-side helpers (filename numbering + flash overlay) live in
// common/atlas_capture* — see dxr_capture::MakeCaptureAtlasPrefix /
// TriggerCaptureFlash / PostFlashRequest.

// Load a scene on the CALLING thread. Only valid before the render thread
// starts (the startup path); every later load must go through QueueSceneLoad,
// because GsRenderer::loadScene submits on the graphics queue that the render
// thread otherwise owns exclusively.
static bool LoadSceneAtStartup(const std::string& path, const char* why) {
    if (!PathFileExistsA(path.c_str())) {
        LOG_WARN("%s: no file at %s", why, path.c_str());
        return false;
    }
    if (!ValidateSceneFile(path)) return false;
    LOG_INFO("%s: %s", why, path.c_str());
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    if (g_gsRenderer.loadScene(path.c_str())) {
        g_loadedFileName = GetPlyFilename(path);
        LOG_INFO("Loaded %s (%s)", g_loadedFileName.c_str(), GetPlyFileSize(path).c_str());
        ApplyAutoFitForLoadedScene_locked();
        return true;
    }
    LOG_WARN("%s: load failed for %s", why, path.c_str());
    return false;
}

// Attempt to auto-load butterfly.spz from next to the exe.
static void TryAutoLoadBundledScene() {
    // A launch that named its own asset (positional path, --src, or a protocol
    // URL) must not flash the bundled butterfly first — and for a URL the real
    // scene only arrives once the fetch thread lands, so the slot has to stay
    // empty rather than be raced.
    if (g_suppressBundledAutoLoad.load(std::memory_order_relaxed)) {
        LOG_INFO("Bundled auto-load skipped: the launch named its own asset");
        return;
    }
    char exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return;
    // Strip basename
    char *lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = strrchr(exePath, '/');
    if (!lastSlash) return;
    *(lastSlash + 1) = '\0';
    std::string path = std::string(exePath) + "butterfly.spz";
    if (!PathFileExistsA(path.c_str())) {
        LOG_INFO("No bundled scene at %s (skipping auto-load)", path.c_str());
        return;
    }
    LoadSceneAtStartup(path, "Auto-loading bundled scene");
}

// Hand a picked path off to the render thread for scene load. Validates the
// extension first; on failure pops a MessageBox and returns false. Used by
// both the Win32 GetOpenFileNameA path and the #228 spatial picker result
// drained in the main loop.
static bool QueueSceneLoad(HWND hwnd, const std::string& path) {
    if (!ValidateSceneFile(path)) {
        MessageBoxA(hwnd, "Invalid scene file. Supported formats: .ply, .spz", "Load Error", MB_OK | MB_ICONERROR);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
        g_pendingLoadPath = path;
    }
    g_loadRequested.store(true, std::memory_order_release);
    LOG_INFO("Queued 3DGS scene load: %s", path.c_str());
    return true;
}

// ── --src=<url>: fetch on a worker thread, load on the render thread ─────────

// Percent-encode everything outside the unreserved set, so the re-check below
// parses the FINAL url through exactly the same grammar the requested one went
// through (rather than a hand-rolled second opinion that could drift from it).
static std::string PercentEncodeAll(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

// Policy re-check for the URL a redirect chain actually landed on. A hostile
// page cannot get a `file:` or off-loopback `http:` src past launch_args.h, but
// nothing stops an https host it IS allowed to name from 302-ing somewhere
// else — so the final URL is re-parsed AS IF it had arrived over the protocol,
// which is the strictest of the two policies and needs no second implementation.
static bool LaunchPolicyAllowsFinalUrl(const std::string& finalUrl) {
    const std::string probe =
        "displayxr-view://open?src=" + PercentEncodeAll(finalUrl) + "&v=1";
    const dxr::LaunchArgs a = dxr::ParseLaunchArgs({probe});
    const bool allowed = a.ok() && a.srcKind == dxr::LaunchSrcKind::Url;
    if (!allowed) {
        LOG_WARN("Redirect target refused by launch policy: %s", finalUrl.c_str());
    }
    return allowed;
}

// Kick a detached download. Synchronous inside the thread (FetchUrlToCache
// blocks); progress lands on the toast, the finished file goes through the same
// cross-thread queue Ctrl+O uses. NEVER loads from this thread.
static void StartUrlFetch(HWND hwnd, const std::string& url, uint64_t maxBytes, bool noCache) {
    bool expected = false;
    if (!g_fetchInFlight.compare_exchange_strong(expected, true)) {
        ToastF("Busy - a download is already running");
        LOG_WARN("Download refused, one already in flight: %s", url.c_str());
        return;
    }
    LOG_INFO("Fetching --src URL: %s (maxBytes=%llu noCache=%d)",
             url.c_str(), (unsigned long long)maxBytes, noCache ? 1 : 0);
    ToastF("Downloading...");
    std::thread([hwnd, url, maxBytes, noCache]() {
        dxr::UrlFetchOptions opts;
        opts.cacheDir = dxr::DefaultCacheDir(L"GaussianSplat");
        opts.allowedExtensions = {".ply", ".spz"};  // .sog is not a loadable format here
        opts.maxBytes = maxBytes;
        opts.noCache = noCache;
        opts.urlAllowed = &LaunchPolicyAllowsFinalUrl;
        opts.progress = [](uint64_t done, uint64_t total) {
            // Throttled: the toast replaces its own message, and re-rasterizing
            // a chip per network chunk would be pure waste.
            static uint64_t s_lastTick = 0;
            const uint64_t now = GetTickCount64();
            if (now - s_lastTick < 300) return;
            s_lastTick = now;
            if (total > 0) {
                ToastF("Downloading  %llu%%  (%.1f / %.1f MB)",
                       (unsigned long long)(done * 100ull / total),
                       (double)done / (1024.0 * 1024.0),
                       (double)total / (1024.0 * 1024.0));
            } else {
                ToastF("Downloading  %.1f MB", (double)done / (1024.0 * 1024.0));
            }
        };
        const dxr::UrlFetchResult r = dxr::FetchUrlToCache(url, opts);
        g_fetchInFlight.store(false);
        if (!r.ok) {
            LOG_ERROR("Download failed for %s: %s", url.c_str(), r.error.c_str());
            ToastF("Download failed - %s", r.error.c_str());
            return;
        }
        const std::string path = dxr::NarrowPathForFopen(dxr::Utf8FromWide(r.path));
        LOG_INFO("Downloaded %llu bytes%s -> %s",
                 (unsigned long long)r.bytes, r.fromCache ? " (cache hit)" : "", path.c_str());
        if (!ValidateSceneFile(path)) {
            // Unreachable while allowedExtensions is {.ply,.spz}; toast rather
            // than MessageBox because this is not the UI thread.
            ToastF("Unsupported scene file");
            LOG_ERROR("Fetched file rejected by ValidateSceneFile: %s", path.c_str());
            return;
        }
        ToastF(r.fromCache ? "Loading (cached)" : "Loading");
        QueueSceneLoad(hwnd, path);
    }).detach();
}

// Nudge a requested --rect back INSIDE the panel monitor, and only that: a
// launcher that deliberately straddles two monitors keeps its geometry, we just
// refuse to place the window entirely off the panel. Never snaps to the panel
// origin, and does nothing at all when the runtime did not confirm the panel
// (sim_display, or a platform whose panel-origin plumbing is still open) or
// reported an all-zero rect — an unconfirmed rect is a valid monitor, but not
// evidence that the 3D panel is there.
static void ClampRectIntoPanel(int32_t& x, int32_t& y, int32_t w, int32_t h) {
    if (!g_displayPanelConfirmed) return;
    const XrRect2Di& p = g_displayDesktopRect;
    if (p.extent.width <= 0 || p.extent.height <= 0) return;
    const int32_t right = p.offset.x + p.extent.width;
    const int32_t bottom = p.offset.y + p.extent.height;
    if (x + w > right)  x = right - w;
    if (y + h > bottom) y = bottom - h;
    if (x < p.offset.x) x = p.offset.x;
    if (y < p.offset.y) y = p.offset.y;
}

// Apply a SECOND launch's arguments to this running instance (the WM_COPYDATA
// hand-off from view_protocol.h's single-instance path). Deliberately narrower
// than the startup path: geometry moves with SetWindowPos ONLY. A style change
// on a shaped window is the one thing transparency Rule 2 forbids, and the
// second launch has no way to know whether this window is currently shaped.
static void ApplyLaunchArgsLive(HWND hwnd, const dxr::LaunchArgs& a) {
    if (a.hasRect) {
        int32_t x = a.rectX, y = a.rectY;
        ClampRectIntoPanel(x, y, a.rectW, a.rectH);
        SetWindowPos(hwnd, nullptr, x, y, a.rectW, a.rectH, SWP_NOZORDER | SWP_NOACTIVATE);
        LOG_INFO("WM_COPYDATA rect: requested (%d,%d %dx%d) -> final (%d,%d %dx%d)",
                 a.rectX, a.rectY, a.rectW, a.rectH, x, y, a.rectW, a.rectH);
    }
    if (a.hasVh) {
        g_vhPinned.store(true, std::memory_order_relaxed);
        g_vhPinnedValue.store(a.vh, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_inputState.viewParams.virtualDisplayHeight = a.vh;
        LOG_INFO("WM_COPYDATA vh: %.3f m", a.vh);
    }
    if (a.srcKind == dxr::LaunchSrcKind::Url) {
        StartUrlFetch(hwnd, a.src, a.maxBytes, a.noCache);
    } else if (a.srcKind == dxr::LaunchSrcKind::LocalPath) {
        QueueSceneLoad(hwnd, dxr::NarrowPathForFopen(a.src));
    } else if (!a.positionalPath.empty()) {
        QueueSceneLoad(hwnd, dxr::NarrowPathForFopen(a.positionalPath));
    }
}

// Open a file dialog and load a .ply or .spz scene (called from main thread).
//
// Path A — workspace + Tier 1 picker available:
//     xrRequestFilePickerDXR fires async. The completion event is drained
//     by the shared PollEvents (displayxr-common) into xr.filePickerLast*;
//     the main loop dispatches to QueueSceneLoad on result arrival.
//
// Path B — workspace mode but no controller / no Tier 1 picker, OR running
// outside a workspace (standalone window), OR running on a non-DisplayXR
// OpenXR runtime: xrRequestFilePickerDXR either returns
// XR_FILE_PICKER_FALLBACK_TIER0_DXR (workspace fallback) or the PFN is
// null (extension absent). Either way fall through to GetOpenFileNameA
// and keep the existing standalone UX.
static void OpenLoadDialog(HWND hwnd) {
    // Already showing a spatial picker — second click on Open is a
    // no-op. Without this guard the prior "filePickerInFlight" check
    // would skip Path A and fall through to GetOpenFileNameA, opening
    // BOTH the spatial picker AND a flat Win32 dialog stacked on top.
    if (g_xr != nullptr && g_xr->filePickerInFlight) {
        LOG_INFO("[#228] OpenLoadDialog: spatial picker already in flight, ignoring");
        return;
    }

    // Path A: spatial picker, when available + not already in flight.
    if (g_xr != nullptr && g_xr->pfnRequestFilePickerEXT != nullptr &&
        !g_xr->filePickerInFlight) {
        XrFilePickerInfoDXR info = {XR_TYPE_FILE_PICKER_INFO_DXR};
        info.mode = XR_FILE_PICKER_MODE_OPEN_DXR;
        strncpy(info.title, "Load Gaussian Splatting Scene",
                sizeof(info.title) - 1);
        info.filterCount = 3;
        strncpy(info.filters[0].description, "3DGS Files",
                sizeof(info.filters[0].description) - 1);
        strncpy(info.filters[0].extensions, "*.ply;*.spz",
                sizeof(info.filters[0].extensions) - 1);
        strncpy(info.filters[1].description, "PLY Files",
                sizeof(info.filters[1].description) - 1);
        strncpy(info.filters[1].extensions, "*.ply",
                sizeof(info.filters[1].extensions) - 1);
        strncpy(info.filters[2].description, "SPZ Files",
                sizeof(info.filters[2].description) - 1);
        strncpy(info.filters[2].extensions, "*.spz",
                sizeof(info.filters[2].extensions) - 1);

        XrAsyncRequestIdDXR rid = 0;
        XrResult r = g_xr->pfnRequestFilePickerEXT(g_xr->session, &info, &rid);
        if (r == XR_SUCCESS) {
            g_xr->filePickerInFlight = true;
            g_xr->filePickerRequestId = rid;
            LOG_INFO("[#228] xrRequestFilePickerDXR -> rc=0x%x requestId=%llu",
                r, (unsigned long long)rid);
            return; // wait for completion event in the main loop
        }
        // r == XR_FILE_PICKER_FALLBACK_TIER0_DXR or an error → fall through.
        LOG_INFO("[#228] xrRequestFilePickerDXR -> rc=0x%x (falling back to Win32)", r);
    }

    // Path B: existing Win32 file dialog (unchanged behavior).
    OPENFILENAMEA ofn = {};
    char filePath[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "3DGS Files (*.ply;*.spz)\0*.ply;*.spz\0PLY Files (*.ply)\0*.ply\0SPZ Files (*.spz)\0*.spz\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Load Gaussian Splatting Scene";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        QueueSceneLoad(hwnd, std::string(filePath));
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // #833: RMB-drag moves the borderless punch-through window (Unity
    // desktop-avatar convention). Consumed BEFORE the input handler so a
    // window-drag doesn't double as camera input.
    if (g_windowDrag.handleMessage(hwnd, msg, wParam, lParam, g_borderless.load())) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        UpdateInputState(g_inputState, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_COPYDATA: {
        // A second launch of this viewer handed us its URL instead of opening a
        // rival window (view_protocol.h AcquireSingleInstanceOrForward). The
        // payload is attacker-influenced text, so it goes through the SAME
        // parser + policy as argv — never straight to a loader.
        const COPYDATASTRUCT* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!cds || cds->dwData != dxr::kViewProtocolCopyDataId || !cds->lpData ||
            cds->cbData == 0) {
            break;
        }
        // Length-bounded: trust cbData, not a NUL the sender may have omitted.
        const char* raw = static_cast<const char*>(cds->lpData);
        size_t n = strnlen(raw, cds->cbData);
        const std::string url(raw, n);
        LOG_INFO("WM_COPYDATA launch URL received (%zu bytes)", n);
        const dxr::LaunchArgs a = dxr::ParseLaunchArgs({url});
        for (const std::string& w : a.warnings) LOG_WARN("launch: %s", w.c_str());
        if (!a.ok()) {
            for (const std::string& e : a.errors) LOG_ERROR("launch: %s", e.c_str());
            ToastF("Refused: %s", a.errors.empty() ? "bad launch URL" : a.errors[0].c_str());
            return TRUE;
        }
        ApplyLaunchArgsLive(hwnd, a);
        return TRUE;
    }
    case WM_NCHITTEST:
        // Shaped borderless mode: the OS only delivers hits inside the
        // region; claim them for normal app input.
        if (g_borderless.load()) return HTCLIENT;
        break;

    case kBorderlessMsg: {
        // Ctrl+T style swap on the window-owning thread; client rect
        // preserved (the avatar recipe). Shaping follows on the render
        // thread once coverage lands.
        const bool borderless = wParam != 0;
        RECT client = {};
        GetClientRect(hwnd, &client);
        POINT tl = {0, 0};
        ClientToScreen(hwnd, &tl);
        // Ctrl+T must never change whether this window is on screen. Under the
        // DisplayXR workspace the runtime hides this HWND at xrCreateSession
        // (its pixels reach the panel through the composed atlas); writing
        // WS_VISIBLE here un-hid it and HWND_TOPMOST then put it on top of the
        // shell. Keep the WS_VISIBLE / WS_MINIMIZE bits exactly as they are,
        // and only touch the z-order while the window is actually on screen.
        const LONG cur = GetWindowLong(hwnd, GWL_STYLE);
        const LONG keep = cur & (WS_VISIBLE | WS_MINIMIZE);
        const bool onScreen = (cur & WS_VISIBLE) != 0 && (cur & WS_MINIMIZE) == 0;
        const DWORD style = (borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW) | (DWORD)keep;
        SetWindowLong(hwnd, GWL_STYLE, (LONG)style);
        RECT want = {tl.x, tl.y, tl.x + client.right, tl.y + client.bottom};
        AdjustWindowRect(&want, style, FALSE);
        // Transparent mode floats above other apps (the avatar behavior):
        // clicks punched through to a window behind activate it, but the
        // scene stays on top. Ctrl+T off returns to the normal z-band.
        HWND zOrder = onScreen ? (borderless ? HWND_TOPMOST : HWND_NOTOPMOST) : nullptr;
        UINT swpFlags = SWP_FRAMECHANGED | SWP_NOACTIVATE | (onScreen ? 0u : SWP_NOZORDER);
        SetWindowPos(hwnd, zOrder, want.left, want.top,
                     want.right - want.left, want.bottom - want.top,
                     swpFlags);
        g_borderless.store(borderless);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        // UpdateInputState above already set leftButton/dragging=true. For
        // button clicks (which post a message to run a modal dialog or change
        // mode), clear that drag state — otherwise the modal eats the
        // matching WM_LBUTTONUP and subsequent mouse motion is interpreted as
        // a scene drag.
        if (IsClickOnLoadButton(mx, my, g_windowWidth, g_windowHeight)) {
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                g_inputState.leftButton = false;
                g_inputState.dragging = false;
            }
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        if (IsClickOnModeButton(mx, my, g_windowWidth, g_windowHeight)) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.leftButton = false;
            g_inputState.dragging = false;
            // Mode button = cycle request (V-key equivalent). Main loop
            // reads runtime's current mode and computes the target.
            g_inputState.cycleRenderingModeRequested = true;
            return 0;
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_USER + 1:
        OpenLoadDialog(hwnd);
        return 0;

    case WM_DROPFILES: {
        // Drop a .ply / .spz scene onto the window. macOS has had this since the
        // Cocoa entry point was written; Windows never wired it. The path goes
        // through the existing QueueSceneLoad — the same entry Ctrl+O and the
        // #228 spatial picker use, with its ValidateSceneFile check and the
        // mutex-guarded handoff to the render thread — so a drop is
        // indistinguishable downstream.
        HDROP drop = (HDROP)wParam;
        const UINT count = DragQueryFileA(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            char path[MAX_PATH] = {};
            if (DragQueryFileA(drop, 0, path, MAX_PATH) > 0) {
                if (count > 1)
                    LOG_INFO("Drop: %u files, loading the first (%s)", count, path);
                QueueSceneLoad(hwnd, std::string(path));
            }
        }
        DragFinish(drop);
        return 0;
    }

    case dxr_capture::kFlashUserMsg:
        // Render thread requested a capture-flash; start it on this thread
        // (the message-pump thread that owns the HWND).
        dxr_capture::TriggerCaptureFlash(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == dxr_capture::kFlashTimerId) {
            dxr_capture::TickCaptureFlash(hwnd);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        // Ctrl+O = open a scene (uniform across all demos + platforms, incl. the
        // mediaplayer). Strict: Ctrl must be held. WM_USER+1 runs the picker on
        // the message loop. (Was bare L — retired for the uniform chord.)
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        // I key = capture multi-view atlas
        if (wParam == 'I' || wParam == 'i') {
            g_captureAtlasRequested.store(true);
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);
        }
        return 0;

    case WM_CLOSE:
        if (g_xr && g_xr->session != XR_NULL_HANDLE && g_xr->sessionRunning) {
            xrRequestExitSession(g_xr->session);
            return 0;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/*!
 * Create the app window.
 *
 * `borderless` is the --transparent launch: the window is born WS_POPUP +
 * WS_EX_TOPMOST and stays that way. This is transparency Rule 2 — a shaped
 * WS_EX_NOREDIRECTIONBITMAP window can never paint an OS frame, so the style
 * must be right from creation rather than swapped afterwards (the Ctrl+T
 * kBorderlessMsg path exists for the interactive toggle, and posting it at
 * startup would produce exactly the flip this contract promises not to do).
 * `titleSuffix` is APPENDED to WINDOW_TITLE — a launcher can label its window,
 * never impersonate a different app.
 */
static HWND CreateAppWindow(HINSTANCE hInstance, int width, int height, int32_t screenLeft, int32_t screenTop,
                            bool borderless, const std::wstring& titleSuffix) {
    LOG_INFO("Creating application window (%dx%d) at (%d, %d) style=%s", width, height,
             screenLeft, screenTop, borderless ? "WS_POPUP (transparent launch)" : "WS_OVERLAPPEDWINDOW");

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Null background brush + WS_EX_NOREDIRECTIONBITMAP (below) are required
    // by the runtime's transparent-window bridge (DComp + KMT shared texture).
    // Both must be set even when the demo defaults to opaque, because session
    // transparency is wired at xrCreateSession time and cannot be toggled later.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WINDOW_CLASS;

    if (!RegisterClassEx(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register window class, error: %lu", err);
            return nullptr;
        }
    }

    // Borderless has no non-client area, so AdjustWindowRect would inflate the
    // window past the rect the launcher asked for. Skip it: for WS_POPUP the
    // window rect IS the client rect.
    RECT rect = { 0, 0, width, height };
    if (!borderless) AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    std::wstring title = WINDOW_TITLE;
    if (!titleSuffix.empty()) title += L" - " + titleSuffix;

    // INV-1.3: open on the 3D panel (runtime#715 — handle apps otherwise open
    // on the primary monitor on multi-monitor boxes). (screenLeft, screenTop)
    // is the panel top-left in virtual-desktop pixels from
    // XrDisplayDesktopPositionDXR; (0,0) = primary/unknown, a safe default.
    // WS_EX_TOPMOST on the transparent launch matches the avatar: a click
    // punched through to a window behind activates it, but the scene stays on
    // top instead of disappearing under the app the user just clicked.
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | (borderless ? WS_EX_TOPMOST : 0u);
    const DWORD style = borderless ? (WS_POPUP | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowEx(exStyle, WINDOW_CLASS, title.c_str(),
        style,
        screenLeft, screenTop,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("Failed to create window, error: %lu", GetLastError());
        return nullptr;
    }

    // Accept dropped .ply / .spz scenes (WM_DROPFILES). This sets the
    // WS_EX_ACCEPTFILES *extended* style, so it survives any later GWL_STYLE
    // rewrite (borderless/fullscreen swaps touch the plain style only).
    DragAcceptFiles(hwnd, TRUE);

    LOG_INFO("Window created: 0x%p", hwnd);
    return hwnd;
}

struct PerformanceStats {
    std::chrono::high_resolution_clock::time_point lastTime;
    float deltaTime = 0.0f;
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    int frameCount = 0;
    float fpsAccumulator = 0.0f;
};

static void UpdatePerformanceStats(PerformanceStats& stats) {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - stats.lastTime);
    stats.deltaTime = duration.count() / 1000000.0f;
    stats.frameTimeMs = duration.count() / 1000.0f;
    stats.lastTime = now;
    stats.fpsAccumulator += stats.deltaTime;
    stats.frameCount++;
    if (stats.fpsAccumulator >= 1.0f) {
        stats.fps = stats.frameCount / stats.fpsAccumulator;
        stats.frameCount = 0;
        stats.fpsAccumulator = 0.0f;
    }
}

// Render a simple "no scene" placeholder by clearing to dark gray
static void RenderPlaceholder(VkDevice device, VkQueue queue, VkCommandPool cmdPool,
                               VkImage image, uint32_t width, uint32_t height) {
    VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = cmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue clearColor = {{0.1f, 0.1f, 0.12f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

    // Transition to COLOR_ATTACHMENT
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

// Standalone (own window) vs. running as a client of the workspace shell's
// external multi-compositor — signalled by the current rendering mode not being
// requestable (the shell owns the mode). Same predicate the foreground-clip
// path uses; kept in one place so the two can't drift.
static bool IsStandaloneSession(const XrSessionManager* xr) {
    return (xr->renderingModeCount == 0) ||
           (xr->currentModeIndex < xr->renderingModeCount &&
            xr->renderingModeIsRequestable[xr->currentModeIndex]);
}

// Idle auto-orbit gate. The turntable is a screensaver for a scene sitting in
// its own opaque window. In standalone transparent mode (Ctrl+T) the scene is
// punched through onto the user's desktop as a floating object, and a floating
// object that spins by itself reads as a glitch rather than as an idle
// screensaver. Under the workspace shell there is no punch-through illusion to
// break (the app is a framed tile in a composed scene), so the turntable keeps
// running there.
//
// This is a live gate, NOT a state change: 'M' (and the set_auto_orbit MCP
// tool) still own animateEnabled, and the turntable resumes — after a fresh
// 10 s idle countdown, since the caller holds lastInputTimeSec at "now" while
// engaged — as soon as the condition clears.
//
// The modelviewer's sibling gate has a second arm for a playing animation clip;
// splat scenes carry no clips, so there is nothing to hold against here.
static bool AutoOrbitSuppressed(const XrSessionManager* xr) {
    return g_transparentBg.load() && IsStandaloneSession(xr);
}

static void RenderThreadFunc(
    HWND hwnd,
    XrSessionManager* xr,
    VkDevice vkDevice,
    VkQueue graphicsQueue,
    uint32_t queueFamilyIndex,
    VkInstance vkInstance,
    VkPhysicalDevice physDevice,
    std::vector<VkImage>* swapchainVkImages,
    HudRenderer* hud,
    uint32_t hudWidth,
    uint32_t hudHeight,
    VkBuffer hudStagingBuffer,
    void* hudStagingMapped,
    VkCommandPool hudCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* hudSwapchainImages,
    VkCommandPool loadBtnCmdPool,
    std::vector<XrSwapchainImageVulkanKHR>* loadBtnSwapchainImages,
    uint32_t loadBtnWidth,
    uint32_t loadBtnHeight)
{
    LOG_INFO("[RenderThread] Started");

    PerformanceStats perfStats = {};
    perfStats.lastTime = std::chrono::high_resolution_clock::now();

    // Command pool for placeholder rendering
    VkCommandPool renderCmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &renderCmdPool);
    }

    while (g_running.load() && !xr->exitRequested) {
        InputState inputSnapshot;
        bool resetRequested = false;
        bool animateToggle = false;
        bool loadReq = false;
        uint32_t windowW, windowH;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot = g_inputState;
        }
        // No pitch sign flip: since W7 (#396) gs_renderer flips Vulkan-Y at
        // the RASTER stage (preprocess.comp), not by reflecting the view
        // matrix, so the shared input_handler's `-= dy` cube_handle
        // convention drives the camera directly — drag-down → look-down.
        float renderPitch = inputSnapshot.pitch;
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            resetRequested = g_inputState.resetViewRequested;
            animateToggle = g_inputState.animateToggleRequested;
            loadReq = g_inputState.loadRequested;
            g_inputState.resetViewRequested = false;
            g_inputState.teleportRequested = false;
            g_inputState.fullscreenToggleRequested = false;
            // ModeSwitch consumes the V/0-8 flags off inputSnapshot (captured
            // above); clear them on the shared state so they fire exactly once.
            g_inputState.cycleRenderingModeRequested = false;
            g_inputState.absoluteRenderingModeRequested = -1;
            g_inputState.eyeTrackingModeToggleRequested = false;
            if (g_inputState.transparentBgToggleRequested) {
                g_inputState.transparentBgToggleRequested = false;
                bool now = !g_transparentBg.load();
                g_transparentBg.store(now);
                LOG_INFO("Transparent background: %s (Ctrl+T)", now ? "ON" : "OFF");
                // #833: transparent mode = borderless + region punch-through.
                PostMessage(hwnd, kBorderlessMsg, now ? 1 : 0, 0);
            }
            g_inputState.animateToggleRequested = false;
            g_inputState.loadRequested = false;
            if (animateToggle) {
                g_inputState.animateEnabled = !g_inputState.animateEnabled;
                inputSnapshot.animateEnabled = g_inputState.animateEnabled;
            }
            windowW = g_windowWidth;
            windowH = g_windowHeight;
        }

        // Request main thread to open file dialog when Ctrl+O or Load button was pressed.
        if (loadReq) {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        }

        // Drain a queued scene load (set by OpenLoadDialog on the main
        // thread). We must run loadScene here because it submits Vulkan work
        // on the graphics queue, and that queue is exclusively driven by this
        // (render) thread for per-frame submissions — see g_pendingLoadPath.
        if (g_loadRequested.exchange(false, std::memory_order_acquire)) {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(g_pendingLoadPathMutex);
                path = std::move(g_pendingLoadPath);
                g_pendingLoadPath.clear();
            }
            if (!path.empty()) {
                LOG_INFO("Loading 3DGS scene: %s", path.c_str());
                std::lock_guard<std::mutex> lock(g_sceneMutex);
                if (g_gsRenderer.loadScene(path.c_str())) {
                    g_loadedFileName = GetPlyFilename(path);
                    LOG_INFO("Scene loaded: %s (%s)", g_loadedFileName.c_str(),
                        GetPlyFileSize(path).c_str());
                    ApplyAutoFitForLoadedScene_locked();
                } else {
                    LOG_ERROR("Failed to load scene: %s", path.c_str());
                    MessageBoxA(hwnd, "Failed to load scene file.\nThe file may be corrupt or unsupported.",
                        "Load Error", MB_OK | MB_ICONERROR);
                }
            }
        }

        // Rendering mode requests (V/mode-button=cycle, 0-8=absolute) through the
        // shared ModeSwitch sequencer: eases viewParams.ipdFactor around the switch
        // and fires xrRequestDisplayRenderingModeDXR on the right frame. Ramped ipd
        // lands on inputSnapshot.viewParams.ipdFactor (what the render path reads).
        // Runtime owns current mode via xr->currentModeIndex.
        XrSessionUpdateModeSwitch(*xr, inputSnapshot, perfStats.deltaTime);

        // Handle eye tracking mode toggle (T key)
        if (inputSnapshot.eyeTrackingModeToggleRequested) {
            if (xr->pfnRequestEyeTrackingModeEXT && xr->session != XR_NULL_HANDLE) {
                XrEyeTrackingModeDXR newMode = (xr->activeEyeTrackingMode == XR_EYE_TRACKING_MODE_MANAGED_DXR)
                    ? XR_EYE_TRACKING_MODE_MANUAL_DXR : XR_EYE_TRACKING_MODE_MANAGED_DXR;
                XrResult etResult = xr->pfnRequestEyeTrackingModeEXT(xr->session, newMode);
                LOG_INFO("Eye tracking mode -> %s (%s)",
                    newMode == XR_EYE_TRACKING_MODE_MANUAL_DXR ? "MANUAL" : "MANAGED",
                    XR_SUCCEEDED(etResult) ? "OK" : "unsupported");
            }
        }

        UpdatePerformanceStats(perfStats);
        g_currentFps.store(perfStats.fps, std::memory_order_relaxed);

        // Auto-orbit gate (see AutoOrbitSuppressed). Drop the turntable for this
        // frame without touching the user's 'M' state, and hold the idle clock
        // at "now" so the 10 s countdown restarts when the gate clears — leaving
        // it stale would snap the orbit on the instant the user exits
        // transparent mode.
        const bool orbitEnabledByUser = inputSnapshot.animateEnabled;
        const bool orbitSuppressed = orbitEnabledByUser && AutoOrbitSuppressed(xr);
        if (orbitSuppressed) {
            inputSnapshot.animateEnabled = false;
            inputSnapshot.lastInputTimeSec = McpNowSec();
        }
        UpdateCameraMovement(inputSnapshot, perfStats.deltaTime, xr->displayHeightM);

        // Re-derive the base against the viewport the runtime actually resolved
        // (published from the previous frame's locate) and advance the move.
        // Runs before the reset block so a Space in the same frame wins.
        RefitForViewport(perfStats.deltaTime);
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            inputSnapshot.viewParams.virtualDisplayHeight =
                g_inputState.viewParams.virtualDisplayHeight;
        }

        // On Space-reset: shared UpdateCameraMovement returns to (0,0,0) + default
        // vHeight. For the splat demo, restore the per-scene auto-fit pose instead.
        if (resetRequested && g_fitValid.load()) {
            inputSnapshot.cameraPosX = g_fitCenter[0];
            inputSnapshot.cameraPosY = g_fitCenter[1];
            inputSnapshot.cameraPosZ = g_fitCenter[2];
            inputSnapshot.yaw = g_fitYaw;
            inputSnapshot.viewParams.virtualDisplayHeight = g_fitVHeight;
            // Land any in-flight refit on the reset target so the animation
            // cannot drag the base back off it over the next frames.
            g_fitTransition.start(g_fitVHeight, g_fitVHeight, 0.0f);
        }

        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_inputState.cameraPosX = inputSnapshot.cameraPosX;
            g_inputState.cameraPosY = inputSnapshot.cameraPosY;
            g_inputState.cameraPosZ = inputSnapshot.cameraPosZ;
            // Pose slerp and auto-orbit mutate yaw/pitch each frame — copy back.
            g_inputState.yaw = inputSnapshot.yaw;
            g_inputState.pitch = inputSnapshot.pitch;
            g_inputState.transitioning = inputSnapshot.transitioning;
            g_inputState.transitionT = inputSnapshot.transitionT;
            g_inputState.animationActive = inputSnapshot.animationActive;
            // Held idle clock (gate above) — persist it, otherwise the shared
            // state keeps the stale timestamp and the countdown never restarts.
            if (orbitSuppressed) g_inputState.lastInputTimeSec = inputSnapshot.lastInputTimeSec;
            if (resetRequested) {
                g_inputState.viewParams = inputSnapshot.viewParams;
                // Auto-orbit always on; reset only clears the in-flight
                // transition. The shared UpdateCameraMovement may set
                // animateEnabled=false on Space — re-assert true here.
                g_inputState.animateEnabled = true;
                g_inputState.transitioning = false;
            }
        }

        // Shared event pump. XR_DXR_mcp_tools calls are dispatched to this
        // app's HandleAgentToolCall via xr.mcpToolHandler (installed in
        // RegisterAgentTools; displayxr-common v2.1.0 / #18, #66).
        PollEvents(*xr);

        // #228 Tier 1: drain a spatial-picker result if one arrived this
        // tick. PollEvents wrote the path + result code onto the session
        // manager; we route it through the same QueueSceneLoad path the
        // Win32 GetOpenFileNameA branch uses. The render thread picks the
        // queued path up via g_pendingLoadPath.
        if (xr->filePickerHasResult) {
            xr->filePickerHasResult = false;
            if (xr->filePickerLastResult == XR_FILE_PICKER_RESULT_SUCCESS_DXR &&
                xr->filePickerLastPath[0] != '\0') {
                QueueSceneLoad(hwnd, std::string(xr->filePickerLastPath));
            } else if (xr->filePickerLastResult == XR_FILE_PICKER_RESULT_CANCELLED_DXR) {
                LOG_INFO("[#228] User cancelled spatial picker — no scene load");
            } else {
                // PICKER_FAILED / INVALID_PATH — log and silently drop.
                // Don't auto-fall-back to Win32: the user already cancelled
                // out of the spatial flow, surfacing another dialog would
                // feel like a bug. They can click Load again if needed.
                LOG_WARN("[#228] Spatial picker delivered result=%d (no load)",
                    (int)xr->filePickerLastResult);
            }
        }

        if (xr->sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(*xr, frameState)) {
                // Sized to runtime's max possible view count (sim_display Quad mode = 4).
                // Active mode's view count drives how many slots are actually filled and submitted.
                XrCompositionLayerProjectionView projectionViews[8] = {};
                bool rendered = false;
                bool hudSubmitted = false;
                bool loadBtnSubmitted = false;

                // Aspect-preserving HUD layer footprint (fixes demo-gs#8).
                // The HUD swapchain has a fixed pixel aspect (hudWidth × hudHeight,
                // sized once at session create). When the workspace tile is
                // resized to a different aspect, the runtime stretches the
                // swapchain per-axis to fit the layer rect — which distorts
                // glyphs and button shapes. Fix: pick layer-rect fractions
                // (layerFracW × layerFracH, in HWND fractions) that match the
                // swapchain aspect so both axes stretch by the same factor
                // (uniform scaling, no distortion). Same pattern as the runtime
                // test apps (test_apps/cube_handle_d3d11_win/main.cpp ~L800).
                // Prefer layerFracH = 1.0 (full window height, keeps the info
                // panel anchored to the window bottom); on extremely tall tiles
                // where that would push layerFracW past 1.0, clamp width and
                // shrink height instead.
                const float hudAR = (hudHeight > 0)
                    ? (float)hudWidth / (float)hudHeight : 1.0f;
                const float windowAR = (windowW > 0 && windowH > 0)
                    ? (float)windowW / (float)windowH : 1.0f;
                float layerFracH = 1.0f;
                float layerFracW = hudAR / windowAR;
                if (layerFracW > 1.0f) {
                    layerFracW = 1.0f;
                    layerFracH = windowAR / hudAR;
                }

                if (frameState.shouldRender) {
                    if (LocateViews(*xr, frameState.predictedDisplayTime,
                        inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                        inputSnapshot.yaw, renderPitch,
                        inputSnapshot.viewParams)) {

                        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                        locateInfo.viewConfigurationType = xr->viewConfigType;
                        locateInfo.displayTime = frameState.predictedDisplayTime;
                        locateInfo.space = xr->localSpace;

                        XrViewState viewState = {XR_TYPE_VIEW_STATE};

                        // Clean +Y-up world camera pose (no Y-mirror — the GsRenderer owns
                        // the Vulkan Y-down flip at the raster stage; see preprocess.comp).
                        XrPosef cameraPose;
                        quat_from_yaw_pitch(inputSnapshot.yaw, inputSnapshot.pitch,
                                            &cameraPose.orientation);
                        cameraPose.position = {inputSnapshot.cameraPosX,
                                               inputSnapshot.cameraPosY,
                                               inputSnapshot.cameraPosZ};
                        const float rigVH = inputSnapshot.viewParams.virtualDisplayHeight /
                                            inputSnapshot.viewParams.scaleFactor;

                        // XR_DXR_view_rig (#396 W7): chain the display rig so the runtime
                        // owns the window resolve + off-axis Kooima and returns render-ready
                        // XrView{pose, fov}. The raw channel carries display-space eyes for
                        // the HUD readout.
                        const bool useRig =
                            XrViewRigExtAvailable() && xr->displayWidthM > 0 && xr->displayHeightM > 0;
                        XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
                        XrViewDisplayRawDXR viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
                        if (useRig) {
                            displayRig.pose = cameraPose;
                            displayRig.virtualDisplayHeight = rigVH;
                            displayRig.ipdFactor = inputSnapshot.viewParams.ipdFactor;
                            displayRig.parallaxFactor = inputSnapshot.viewParams.parallaxFactor;
                            displayRig.perspectiveFactor = inputSnapshot.viewParams.perspectiveFactor;
                            locateInfo.next = &displayRig;
                            viewState.next = &viewRigRaw;
                        }

                        // Over-allocate to the runtime's max possible view_count (sim_display
                        // reports 4 for Quad mode; LeiaSR reports 2). Hardcoding 2 here used
                        // to fail with XR_ERROR_SIZE_INSUFFICIENT under sim_display.
                        uint32_t viewCount = 8;
                        XrView rawViews[8];
                        for (uint32_t i = 0; i < 8; i++) rawViews[i] = {XR_TYPE_VIEW};
                        xrLocateViews(xr->session, &locateInfo, &viewState, 8, &viewCount, rawViews);

                        // HUD eye readout. Under the rig, rawViews[] carries render-ready
                        // WORLD eyes, so the display-space eyes come from the raw channel
                        // (XrViewDisplayRawDXR); without the rig, LocateViews() already
                        // stored them from its own (rig-free) locate.
                        if (useRig && viewRigRaw.eyeCountOutput > 0) {
                            for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                                xr->eyePositions[v][0] = viewRigRaw.rawEyes[v].x;
                                xr->eyePositions[v][1] = viewRigRaw.rawEyes[v].y;
                                xr->eyePositions[v][2] = viewRigRaw.rawEyes[v].z;
                            }
                        }

                        // The same raw channel carries the canvas the runtime
                        // RESOLVED for this locate — the shell's 3D window tile
                        // under a workspace, this window's client rect
                        // standalone. That, not our own (hidden, creation-size)
                        // window, is the viewport the auto-fit must frame
                        // against; RefitForViewport picks it up next tick.
                        if (useRig) {
                            g_autoFitCanvas.PublishFromRaw(
                                viewRigRaw.canvasSizeMeters.width,
                                viewRigRaw.canvasSizeMeters.height,
                                viewRigRaw.canvasRectPx.extent.width,
                                viewRigRaw.canvasRectPx.extent.height);
                        }

                        bool monoMode = (xr->renderingModeCount > 0 && !xr->renderingModeDisplay3D[xr->currentModeIndex]);

                        // View count for the active rendering mode (1=mono, 2=stereo, 4=quad).
                        // Sized off the runtime's per-mode advertisement so the eye-loop and
                        // per-view buffers (stereoViews / viewMat / projectionViews)
                        // all line up with what xrEndFrame expects.
                        uint32_t activeViewCount = (xr->renderingModeCount > 0)
                            ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2u;
                        if (activeViewCount == 0) activeViewCount = 1u;
                        if (activeViewCount > 8) activeViewCount = 8u;
                        const int eyeCount = monoMode ? 1 : (int)activeViewCount;

                        // Per-view extent driven entirely by the current rendering
                        // mode's view_scale and the live window size. Atlas dims
                        // (cols × renderW, rows × renderH) are what gets written to
                        // the swapchain and snapshotted by the 'I' key. Swapchain
                        // creation already sized for the largest atlas, so no clamp.
                        // Falls back to the global recommendedViewScale (and 1.0 for
                        // mono) if the runtime didn't advertise per-mode info.
                        float scaleX, scaleY;
                        uint32_t cols, rows;
                        if (xr->renderingModeCount > 0) {
                            uint32_t mode = xr->currentModeIndex;
                            scaleX = xr->renderingModeScaleX[mode];
                            scaleY = xr->renderingModeScaleY[mode];
                            cols   = xr->renderingModeTileColumns[mode] ? xr->renderingModeTileColumns[mode] : 1u;
                            rows   = xr->renderingModeTileRows[mode]    ? xr->renderingModeTileRows[mode]    : 1u;
                        } else if (monoMode) {
                            scaleX = 1.0f; scaleY = 1.0f; cols = 1u; rows = 1u;
                        } else {
                            scaleX = xr->recommendedViewScaleX;
                            scaleY = xr->recommendedViewScaleY;
                            cols = 2u; rows = 1u;  // legacy SBS default
                        }
                        uint32_t renderW = (uint32_t)((double)windowW * scaleX);
                        uint32_t renderH = (uint32_t)((double)windowH * scaleY);
                        if (renderW == 0) renderW = 1;
                        if (renderH == 0) renderH = 1;

                        // --- Consume the runtime's render-ready XrView{pose, fov} (#396 W7) ---
                        // The runtime owns the off-axis Kooima (window resolve included); the
                        // app keeps only the clip policy (fov is clip-independent). near =
                        // ez - vH, far = ez + far_offset, where ez = rig-local eye Z
                        // (RigLocalEyeZ == the display-space eye Z display3d used to resolve).
                        // Transparent-bg (Ctrl+T) clamps far to the ZDP (foreground-only);
                        // opaque pushes it to ~infinity (1000·vH). The view matrix is the
                        // plain clean-frame mat4_view_from_xr_pose — GsRenderer owns the
                        // Vulkan Y-down flip at the raster stage. stereoViews[] is just a
                        // per-view container so the render loops below stay unchanged.
                        Display3DView stereoViews[8];
                        bool useAppProjection = useRig;
                        if (useRig) {
                            // Mono: collapse the active views to their centroid (pose + fov).
                            uint32_t srcCount = activeViewCount;
                            if (srcCount > viewCount) srcCount = viewCount;
                            if (srcCount < 1) srcCount = 1;
                            XrView srcViews[8];
                            if (monoMode) {
                                XrView cv = rawViews[0];
                                XrVector3f c = {0, 0, 0};
                                XrFovf f = {0, 0, 0, 0};
                                for (uint32_t v = 0; v < srcCount; v++) {
                                    c.x += rawViews[v].pose.position.x;
                                    c.y += rawViews[v].pose.position.y;
                                    c.z += rawViews[v].pose.position.z;
                                    f.angleLeft  += rawViews[v].fov.angleLeft;
                                    f.angleRight += rawViews[v].fov.angleRight;
                                    f.angleUp    += rawViews[v].fov.angleUp;
                                    f.angleDown  += rawViews[v].fov.angleDown;
                                }
                                float inv = 1.0f / (float)srcCount;
                                cv.pose.position = {c.x * inv, c.y * inv, c.z * inv};
                                cv.fov = {f.angleLeft * inv, f.angleRight * inv,
                                          f.angleUp * inv, f.angleDown * inv};
                                srcViews[0] = cv;
                            } else {
                                for (int e = 0; e < eyeCount; e++)
                                    srcViews[e] = rawViews[e < (int)viewCount ? e : 0];
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                const XrView& sv = srcViews[eye];
                                float ez = RigLocalEyeZ(cameraPose, sv.pose.position);
                                float near_z = (ez - rigVH > 1.0e-4f) ? (ez - rigVH) : 1.0e-4f;
                                float far_z  = g_transparentBg.load() ? ez : (ez + 1000.0f * rigVH);
                                if (far_z < near_z + 1.0e-4f) far_z = near_z + 1.0e-4f;
                                mat4_view_from_xr_pose(stereoViews[eye].view_matrix, sv.pose);
                                mat4_from_xr_fov(stereoViews[eye].projection_matrix, sv.fov, near_z, far_z);
                                stereoViews[eye].fov = sv.fov;
                                stereoViews[eye].eye_world = sv.pose.position;
                                stereoViews[eye].orientation = sv.pose.orientation;
                                stereoViews[eye].eye_display = {0.0f, 0.0f, ez};  // ZDP depth (pick/transparent)
                                stereoViews[eye].near_z = near_z;
                                stereoViews[eye].far_z = far_z;
                            }
                        }

                        // Double-click focus: center-eye ray through mouse, pick splat,
                        // smoothly re-pose the virtual display to face back along the ray.
                        if (inputSnapshot.teleportRequested && useRig) {
                            float ndcX = 2.0f * inputSnapshot.teleportMouseX / (float)windowW - 1.0f;
                            float ndcY = -(2.0f * inputSnapshot.teleportMouseY / (float)windowH - 1.0f);

                            // Center-eye pick view reconstructed from the render-ready rig
                            // views: average the active eye poses + fovs into a symmetric
                            // center frustum in the clean +Y-up world frame the splats live
                            // in (no Y flip — the pick ray must match the world, not the
                            // Vulkan raster). A well-conditioned near/far (the ray is a full
                            // line). pickClipFar in transparent mode is the ZDP = rig-local
                            // center eye Z, matching the old centerView.eye_display.z. NDC Y
                            // was already adjusted above for Win32's top-left mouse origin.
                            XrVector3f cpos = {0, 0, 0};
                            XrFovf cfov = {0, 0, 0, 0};
                            for (int e = 0; e < eyeCount; e++) {
                                cpos.x += stereoViews[e].eye_world.x;
                                cpos.y += stereoViews[e].eye_world.y;
                                cpos.z += stereoViews[e].eye_world.z;
                                cfov.angleLeft  += stereoViews[e].fov.angleLeft;
                                cfov.angleRight += stereoViews[e].fov.angleRight;
                                cfov.angleUp    += stereoViews[e].fov.angleUp;
                                cfov.angleDown  += stereoViews[e].fov.angleDown;
                            }
                            float invE = 1.0f / (float)eyeCount;
                            XrPosef cpose;
                            cpose.position = {cpos.x * invE, cpos.y * invE, cpos.z * invE};
                            cpose.orientation = cameraPose.orientation;
                            cfov = {cfov.angleLeft * invE, cfov.angleRight * invE,
                                    cfov.angleUp * invE, cfov.angleDown * invE};
                            float ez = RigLocalEyeZ(cameraPose, cpose.position);
                            float pickNear = (ez - rigVH > 1.0e-4f) ? (ez - rigVH) : 1.0e-4f;
                            float pickFar = ez + 1000.0f * rigVH;
                            float pickView[16], pickProj[16];
                            mat4_view_from_xr_pose(pickView, cpose);
                            mat4_from_xr_fov(pickProj, cfov, pickNear, pickFar);

                            XrVector3f rayOriginV, rayDirV;
                            display3d_unproject_ndc_to_ray(ndcX, ndcY,
                                pickView, pickProj, &rayOriginV, &rayDirV);

                            float rayOrigin[3] = {rayOriginV.x, rayOriginV.y, rayOriginV.z};
                            float rayDir[3]    = {rayDirV.x,    rayDirV.y,    rayDirV.z};
                            float hitPos[3];
                            // Only recenter on splats that are actually visible: reject any
                            // in front of the near plane (pickNear), and — in transparent/
                            // foreground mode — behind the ZDP (ez). Opaque mode shows
                            // everything behind the display, so no far reject there. A full
                            // miss returns false -> no recenter, the existing behavior.
                            float pickClipFar = g_transparentBg.load() ? ez : 0.0f;
                            std::lock_guard<std::mutex> sceneLock(g_sceneMutex);
                            if (g_gsRenderer.pickGaussian(rayOrigin, rayDir, hitPos, 100.0f,
                                                          pickView,
                                                          pickNear, pickClipFar)) {
                                // Both endpoints stored in the clean +Y-up WORLD frame (the
                                // same frame as inputSnapshot.cameraPosX/Y/Z and the splats)
                                // so the slerp interpolates consistently. cameraPose is that
                                // world orientation — no pitch-sign rebuild needed since the
                                // raster-stage flip removed the render-frame negation.
                                XrPosef fromWorld;
                                fromWorld.orientation = cameraPose.orientation;
                                fromWorld.position = {inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ};
                                XrPosef target;
                                target.position = {hitPos[0], hitPos[1], hitPos[2]};
                                target.orientation = cameraPose.orientation;  // preserve current orientation — translate-only
                                std::lock_guard<std::mutex> inputLock(g_inputMutex);
                                g_inputState.transitionFrom = fromWorld;
                                g_inputState.transitionTo = target;
                                g_inputState.transitionT = 0.0f;
                                g_inputState.transitioning = true;
                                LOG_INFO("Focus on splat (%.3f, %.3f, %.3f)",
                                    hitPos[0], hitPos[1], hitPos[2]);
                            }
                        }

                        rendered = true;
                        // eyeCount already computed above from active mode's view count

                        // Mono center eye
                        XMMATRIX monoViewMatrix, monoProjMatrix;
                        XrPosef monoPose = rawViews[0].pose;
                        if (monoMode) {
                            if (useRig) {
                                // Centroid view already built into stereoViews[0]; submit
                                // its world eye with the camera orientation.
                                monoPose.position = stereoViews[0].eye_world;
                                monoPose.orientation = cameraPose.orientation;
                            } else {
                                monoPose.position.x = (rawViews[0].pose.position.x + rawViews[1].pose.position.x) * 0.5f;
                                monoPose.position.y = (rawViews[0].pose.position.y + rawViews[1].pose.position.y) * 0.5f;
                                monoPose.position.z = (rawViews[0].pose.position.z + rawViews[1].pose.position.z) * 0.5f;
                            }

                            if (!useAppProjection) {
                                monoProjMatrix = xr->projMatrices[0];
                                XMVECTOR centerLocalPos = XMVectorSet(
                                    monoPose.position.x, monoPose.position.y, monoPose.position.z, 0.0f);
                                XMVECTOR localOri = XMVectorSet(
                                    rawViews[0].pose.orientation.x, rawViews[0].pose.orientation.y,
                                    rawViews[0].pose.orientation.z, rawViews[0].pose.orientation.w);
                                float monoM2vView = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    monoM2vView = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                float eyeScale = inputSnapshot.viewParams.perspectiveFactor * monoM2vView / inputSnapshot.viewParams.scaleFactor;
                                XMVECTOR playerOri = XMQuaternionRotationRollPitchYaw(
                                    renderPitch, inputSnapshot.yaw, 0);
                                XMVECTOR playerPos = XMVectorSet(
                                    inputSnapshot.cameraPosX, -inputSnapshot.cameraPosY,
                                    inputSnapshot.cameraPosZ, 0.0f);
                                XMVECTOR worldPos = XMVector3Rotate(centerLocalPos * eyeScale, playerOri) + playerPos;
                                XMVECTOR worldOri = XMQuaternionMultiply(localOri, playerOri);
                                XMMATRIX rot = XMMatrixTranspose(XMMatrixRotationQuaternion(worldOri));
                                XMFLOAT3 wp;
                                XMStoreFloat3(&wp, worldPos);
                                monoViewMatrix = XMMatrixTranslation(-wp.x, -wp.y, -wp.z) * rot;
                            }
                        }

                        // Foreground-only clip: in transparent mode, cull splats
                        // behind the virtual display plane so only popping-out
                        // content shows. Suppressed under the shell's external
                        // multi-compositor (non-controller workspace session,
                        // where the per-app transparent bridge is bypassed) —
                        // signalled by renderingModeIsRequestable being false.
                        bool foregroundClip = g_transparentBg.load() && IsStandaloneSession(xr);

                        // Soft foreground clip: fade splat opacity over the last
                        // clipFadeFrac of the eye->ZDP distance instead of a hard
                        // discard at the plane, so splat centers crossing the ZDP
                        // roll off rather than pop. 0 reverts to the legacy hard cut.
                        const float clipFadeFrac = 0.15f;

                        // Build per-eye view/projection matrices (column-major float[16]).
                        // Sized to the runtime's max view count so Quad mode (4 views) fits.
                        float viewMat[8][16], projMat[8][16];
                        float clipNear[8] = {0}; // per-eye view-space near cull (0 = off)
                        float clipFar[8] = {0};  // per-eye view-space far cull (0 = off)
                        for (int eye = 0; eye < eyeCount; eye++) {
                            if (useAppProjection) {
                                int srcEye = monoMode ? 0 : eye;
                                memcpy(viewMat[eye], stereoViews[srcEye].view_matrix, sizeof(float) * 16);
                                memcpy(projMat[eye], stereoViews[srcEye].projection_matrix, sizeof(float) * 16);
                                // near_z/far_z are the resolved per-eye view-space planes
                                // (ez - near_offset / ez + far_offset), in the same units as
                                // the shader's p_view.z. They drive the explicit geometric
                                // culls — the projection-matrix planes do NOT clip splats.
                                clipNear[eye] = stereoViews[srcEye].near_z;
                                // Far cull stays gated on foreground (transparent + standalone)
                                // mode; in opaque mode far_z = ez + 1000*vH is effectively
                                // infinite anyway, so this just keeps the shell path untouched.
                                if (foregroundClip) {
                                    clipFar[eye] = stereoViews[srcEye].far_z;
                                }
                            } else {
                                // Fallback: use DirectXMath mono matrices, store as column-major
                                XMMATRIX v = monoMode ? monoViewMatrix :
                                    XMMatrixLookAtRH(XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position),
                                        XMLoadFloat3((XMFLOAT3*)&rawViews[eye].pose.position) + XMVectorSet(0,0,-1,0),
                                        XMVectorSet(0,1,0,0));
                                XMMATRIX p = monoMode ? monoProjMatrix : xr->projMatrices[0];
                                // XMMatrix is row-major; transpose to get column-major for shader
                                XMMATRIX vT = XMMatrixTranspose(v);
                                XMMATRIX pT = XMMatrixTranspose(p);
                                XMStoreFloat4x4((XMFLOAT4X4*)viewMat[eye], vT);
                                XMStoreFloat4x4((XMFLOAT4X4*)projMat[eye], pT);
                            }
                        }

                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(*xr, imageIndex)) {
                            VkFormat colorFormat = (VkFormat)xr->swapchain.format;

                            bool hasGsScene;
                            {
                                std::lock_guard<std::mutex> lock(g_sceneMutex);
                                hasGsScene = g_gsRenderer.hasScene();
                            }

                            if (hasGsScene) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    // Row-major eye placement in the atlas; for 2×1 SBS
                                    // this is (0, renderW) at row 0; for mono (cols=1)
                                    // it collapses to (0, 0).
                                    uint32_t col = (uint32_t)eye % cols;
                                    uint32_t row = (uint32_t)eye / cols;
                                    uint32_t vpX = col * renderW;
                                    uint32_t vpY = row * renderH;
                                    g_gsRenderer.renderEye(
                                        (*swapchainVkImages)[imageIndex], colorFormat,
                                        xr->swapchain.width, xr->swapchain.height,
                                        vpX, vpY, renderW, renderH,
                                        viewMat[eye], projMat[eye],
                                        g_transparentBg.load(), clipNear[eye], clipFar[eye], clipFadeFrac);
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, renderCmdPool,
                                    (*swapchainVkImages)[imageIndex], xr->swapchain.width, xr->swapchain.height);
                            }

                            // 'I' key: snapshot the multi-view atlas the runtime
                            // composes for this session via xrCaptureAtlasDXR
                            // (XR_DXR_atlas_capture, W6 of #396). The runtime owns
                            // the readback — no app-side staging texture. Works for
                            // any multi-view layout the runtime advertises; skipped
                            // for mono (1×1). Filename auto-increments. The prefix
                            // has no ".png"; the runtime appends "_atlas.png".
                            if (g_captureAtlasRequested.exchange(false)) {
                                if (!hasGsScene) {
                                    LOG_WARN("Capture skipped: no scene loaded");
                                } else if (cols <= 1 && rows <= 1) {
                                    LOG_WARN("Capture skipped: mono (1×1) layout");
                                } else if (xr->pfnCaptureAtlasEXT &&
                                           xr->session != XR_NULL_HANDLE) {
                                    std::string sceneName;
                                    {
                                        std::lock_guard<std::mutex> lock(g_sceneMutex);
                                        sceneName = g_loadedFileName;
                                    }
                                    // Strip extension from scene filename
                                    // (e.g. "butterfly.spz" → "butterfly").
                                    auto dot = sceneName.find_last_of('.');
                                    std::string stem = (dot == std::string::npos)
                                        ? sceneName : sceneName.substr(0, dot);
                                    if (stem.empty()) stem = "scene";
                                    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix(
                                        stem, cols, rows);
                                    XrAtlasCaptureInfoDXR info = {XR_TYPE_ATLAS_CAPTURE_INFO_DXR};
                                    info.next = nullptr;
                                    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_DXR;
                                    strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);
                                    XrResult cr = xr->pfnCaptureAtlasEXT(xr->session, &info, nullptr);
                                    if (XR_SUCCEEDED(cr)) {
                                        LOG_INFO("Atlas capture requested -> %s_atlas.png",
                                                 prefix.c_str());
                                        dxr_capture::PostFlashRequest(hwnd);
                                    } else {
                                        LOG_WARN("xrCaptureAtlasDXR failed: 0x%x", (unsigned)cr);
                                    }
                                } else {
                                    LOG_WARN("Capture skipped: XR_DXR_atlas_capture not available");
                                }
                            }

                            for (int eye = 0; eye < eyeCount; eye++) {
                                uint32_t col = (uint32_t)eye % cols;
                                uint32_t row = (uint32_t)eye / cols;
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr->swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {
                                    (int32_t)(col * renderW), (int32_t)(row * renderH)};
                                projectionViews[eye].subImage.imageRect.extent = {
                                    (int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                if (useRig) {
                                    // Submit the consumed per-view world eye with the camera
                                    // orientation (mirrors the macOS W7 leg).
                                    projectionViews[eye].pose.position =
                                        stereoViews[monoMode ? 0 : eye].eye_world;
                                    projectionViews[eye].pose.orientation = cameraPose.orientation;
                                } else {
                                    projectionViews[eye].pose = monoMode ? monoPose : rawViews[eye].pose;
                                }
                                projectionViews[eye].fov = useAppProjection ?
                                    stereoViews[monoMode ? 0 : eye].fov :
                                    (monoMode ? rawViews[0].fov : rawViews[eye].fov);
                            }

                            // #833 punch-through: while Ctrl+T transparent AND
                            // borderless, shape the window from view 0's alpha
                            // (fence-pipelined, ~2-frame lag — no waits). Chrome
                            // is hidden in transparent mode (product call), so
                            // no chrome rects are kept.
                            if (g_transparentBg.load() && g_borderless.load()) {
                                static bool s_punchInit = false;
                                if (!s_punchInit) {
                                    s_punchInit = g_punch.init(vkDevice, physDevice, queueFamilyIndex);
                                }
                                if (s_punchInit) {
                                    // The toast is the ONE piece of chrome that must
                                    // survive shaping. Everything else is hidden in
                                    // transparent mode, so a --src download on an
                                    // empty window would otherwise be carved away
                                    // entirely — a blank silhouette reporting nothing.
                                    RECT chrome[1];
                                    uint32_t chromeCount = 0;
                                    if (g_toast.Active() && windowW > 0 && windowH > 0) {
                                        const dxr::ToastLayerRect tr = dxr::ComputeToastLayerRect(
                                            windowW, windowH,
                                            (float)TOAST_TEX_W / (float)TOAST_TEX_H,
                                            TOAST_SIZE_FRACTION, TOAST_Y_FRACTION);
                                        chrome[0].left   = (LONG)(tr.x * (float)windowW);
                                        chrome[0].top    = (LONG)(tr.y * (float)windowH);
                                        chrome[0].right  = (LONG)((tr.x + tr.width) * (float)windowW);
                                        chrome[0].bottom = (LONG)((tr.y + tr.height) * (float)windowH);
                                        chromeCount = 1;
                                    }
                                    // Union the LAST view's tile too — a view-0-only
                                    // region clips the other views' parallax edges
                                    // once content moves off ZDP (butterfly bug).
                                    const uint32_t lastV = (uint32_t)(eyeCount - 1);
                                    g_punch.update(graphicsQueue, (*swapchainVkImages)[imageIndex],
                                                   renderW, renderH, hwnd, windowW, windowH,
                                                   chromeCount ? chrome : nullptr, chromeCount,
                                                   (lastV % cols) * renderW, (lastV / cols) * renderH,
                                                   eyeCount > 1);
                                }
                            } else if (g_punch.shaped()) {
                                g_punch.disable(hwnd);
                            }
                            ReleaseSwapchainImage(*xr);
                        } else {
                            rendered = false;
                        }

                        // Render HUD to window-space layer swapchain. Submitted
                        // every frame so the chrome buttons (Open / Mode) stay
                        // visible — the TAB toggle only hides the body backdrop
                        // and text via the `drawBody` flag below.
                        // #833: chrome (the HUD layer incl. buttons) is HIDDEN in
                        // transparent/punch-through mode by design — the splats
                        // float clean over the live desktop.
                        if (rendered && hud && xr->hasHudSwapchain && hudSwapchainImages &&
                            !g_transparentBg.load()) {
                            {
                                std::wstring sessionText(xr->systemName, xr->systemName + strlen(xr->systemName));
                                sessionText += L"\nSession: ";
                                sessionText += FormatSessionState((int)xr->sessionState);
                                std::wstring modeText = xr->hasWin32WindowBindingExt ?
                                    L"XR_DXR_win32_window_binding: ACTIVE (Vulkan + 3DGS)" :
                                    L"XR_DXR_win32_window_binding: NOT AVAILABLE";

                                // Scene info
                                std::wstring sceneText = L"\n--- 3DGS Scene ---";
                                {
                                    std::lock_guard<std::mutex> lock(g_sceneMutex);
                                    if (g_gsRenderer.hasScene()) {
                                        std::wstring fname(g_loadedFileName.begin(), g_loadedFileName.end());
                                        sceneText += L"\nLoaded: " + fname;
                                    } else {
                                        sceneText += L"\nNo scene loaded (Ctrl+O or click Load)";
                                    }
                                }
                                modeText += sceneText;

                                // Per-view extent for HUD display — same formula as the
                                // render path (window × view_scale of the current mode).
                                float dispScaleX, dispScaleY;
                                if (xr->renderingModeCount > 0) {
                                    uint32_t mode = xr->currentModeIndex;
                                    dispScaleX = xr->renderingModeScaleX[mode];
                                    dispScaleY = xr->renderingModeScaleY[mode];
                                } else {
                                    dispScaleX = xr->recommendedViewScaleX;
                                    dispScaleY = xr->recommendedViewScaleY;
                                }
                                uint32_t dispRenderW = (uint32_t)((double)windowW * dispScaleX);
                                uint32_t dispRenderH = (uint32_t)((double)windowH * dispScaleY);
                                if (dispRenderW == 0) dispRenderW = 1;
                                if (dispRenderH == 0) dispRenderH = 1;
                                std::wstring perfText = FormatPerformanceInfo(perfStats.fps, perfStats.frameTimeMs,
                                    dispRenderW, dispRenderH, windowW, windowH);
                                std::wstring dispText = FormatDisplayInfo(xr->displayWidthM, xr->displayHeightM,
                                    xr->nominalViewerX, xr->nominalViewerY, xr->nominalViewerZ);
                                dispText += L"\n" + FormatScaleInfo(xr->recommendedViewScaleX, xr->recommendedViewScaleY);
                                dispText += L"\n" + FormatMode(xr->currentModeIndex, xr->pfnRequestDisplayRenderingModeEXT != nullptr,
                                    (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeNames[xr->currentModeIndex] : nullptr,
                                    xr->renderingModeCount,
                                    xr->renderingModeCount > 0 ? xr->renderingModeDisplay3D[xr->currentModeIndex] : true,
                                    xr->renderingModeCount > 0 ? xr->renderingModeIsRequestable[xr->currentModeIndex] : true);
                                std::wstring eyeText = FormatEyeTrackingInfo(
                                    xr->eyePositions, (uint32_t)eyeCount,
                                    xr->eyeTrackingActive, xr->isEyeTracking,
                                    xr->activeEyeTrackingMode, xr->supportedEyeTrackingModes);

                                float fwdX = -sinf(inputSnapshot.yaw) * cosf(renderPitch);
                                float fwdY =  sinf(renderPitch);
                                float fwdZ = -cosf(inputSnapshot.yaw) * cosf(renderPitch);
                                std::wstring cameraText = FormatCameraInfo(
                                    inputSnapshot.cameraPosX, inputSnapshot.cameraPosY, inputSnapshot.cameraPosZ,
                                    fwdX, fwdY, fwdZ);
                                float hudM2v = 1.0f;
                                if (inputSnapshot.viewParams.virtualDisplayHeight > 0.0f && xr->displayHeightM > 0.0f)
                                    hudM2v = inputSnapshot.viewParams.virtualDisplayHeight / xr->displayHeightM;
                                std::wstring stereoText = FormatViewParams(
                                    inputSnapshot.viewParams.ipdFactor, inputSnapshot.viewParams.parallaxFactor,
                                    inputSnapshot.viewParams.perspectiveFactor, inputSnapshot.viewParams.scaleFactor);
                                {
                                    wchar_t vhBuf[128];  // fits the longest "held:" label
                                    int depthPct = (int)(inputSnapshot.viewParams.ipdFactor * 100.0f + 0.5f);
                                    // orbitEnabledByUser, not the snapshot flag —
                                    // the gate clears the latter for the frame, and
                                    // reporting that as "OFF" would blame the M key
                                    // for a hold the display mode asked for.
                                    const wchar_t* orbitLbl =
                                        !orbitEnabledByUser ? L"OFF"
                                        : orbitSuppressed ? L"ON (held: transparent)"
                                        : (inputSnapshot.animationActive ? L"ON (running)"
                                                                         : L"ON (idle countdown)");
                                    swprintf(vhBuf, 128, L"\nvHeight: %.3f  m2v: %.3f\nDepth/IPD: %d%%  Auto-Orbit: %s",
                                        inputSnapshot.viewParams.virtualDisplayHeight, hudM2v, depthPct, orbitLbl);
                                    stereoText += vhBuf;
                                }
                                std::wstring helpText = L"[WASDEQ] Move | [LMB-drag] Rotate | [Scroll] Zoom\n"
                                    L"[DblClick] Focus | [-/=] Depth | [Space] Reset\n"
                                    L"[M] Auto-Orbit | [V] Mode | [Ctrl+O] Load | [Tab] HUD | [ESC] Quit";

                                // Top-bar buttons. Translate window-fraction click
                                // regions into HUD-pixel coords scaled by the
                                // per-frame layer footprint (layerFracW/H, computed
                                // below the HUD block).
                                std::vector<HudButton> buttons;
                                {
                                    // Hover tracking: compare current cursor (in HWND
                                    // pixel space, captured by WM_MOUSEMOVE into
                                    // inputSnapshot.mouseX/Y) against the button's
                                    // HWND-fraction rect. Hovered buttons get a
                                    // visual highlight in the HUD renderer.
                                    const float mx_frac = (g_windowWidth > 0)
                                        ? (float)inputSnapshot.mouseX / (float)g_windowWidth : 0.0f;
                                    const float my_frac = (g_windowHeight > 0)
                                        ? (float)inputSnapshot.mouseY / (float)g_windowHeight : 0.0f;
                                    // HWND-fraction → HUD-pixel mapping must divide by
                                    // the current layer footprint (layerFracW/H), not the
                                    // legacy constants, otherwise on-screen position
                                    // drifts as the tile aspect changes.
                                    auto toHudPx = [&](float xf, float yf, float wf, float hf, const std::wstring& label) {
                                        HudButton b;
                                        b.label = label;
                                        b.x = (xf / layerFracW) * (float)hudWidth;
                                        b.y = (yf / layerFracH) * (float)hudHeight;
                                        b.width  = (wf / layerFracW) * (float)hudWidth;
                                        b.height = (hf / layerFracH) * (float)hudHeight;
                                        b.hovered = (mx_frac >= xf && mx_frac <= xf + wf &&
                                                     my_frac >= yf && my_frac <= yf + hf);
                                        return b;
                                    };
                                    buttons.push_back(toHudPx(
                                        OPEN_BTN_X_FRACTION, OPEN_BTN_Y_FRACTION,
                                        OPEN_BTN_WIDTH_FRACTION, OPEN_BTN_HEIGHT_FRACTION,
                                        L"Open…"));
                                    std::wstring modeLabel = L"Mode";
                                    if (xr->renderingModeCount > 0 &&
                                        xr->currentModeIndex < xr->renderingModeCount &&
                                        xr->renderingModeNames[xr->currentModeIndex]) {
                                        const char* nm = xr->renderingModeNames[xr->currentModeIndex];
                                        modeLabel = L"Mode: " + std::wstring(nm, nm + strlen(nm));
                                    }
                                    // v13: surface workspace mode-lock so user
                                    // knows clicking the Mode button is a no-op.
                                    if (xr->renderingModeCount > 0 &&
                                        xr->currentModeIndex < xr->renderingModeCount &&
                                        !xr->renderingModeIsRequestable[xr->currentModeIndex]) {
                                        modeLabel += L" [locked]";
                                    }
                                    buttons.push_back(toHudPx(
                                        MODE_BTN_X_FRACTION, MODE_BTN_Y_FRACTION,
                                        MODE_BTN_WIDTH_FRACTION, MODE_BTN_HEIGHT_FRACTION,
                                        modeLabel));
                                }

                                // #837 overlay kit: rasterize + upload ONLY when the HUD
                                // content actually changed. Volatile readouts (fps, eye
                                // positions, camera) refresh on a 250 ms tick folded into
                                // the hash while the body is visible; body hidden =
                                // buttons-only, re-uploaded on hover/label changes. The
                                // upload submits with a fence and never waits — the old
                                // per-frame vkQueueWaitIdle here drained the whole frame's
                                // queued GPU work on the single iGPU queue.
                                static dxr::CachedLayerUploader s_hudUp;
                                static bool s_hudUpInit = false;
                                static bool s_hudUploadedOnce = false;
                                if (!s_hudUpInit) {
                                    s_hudUpInit = s_hudUp.init(vkDevice, physDevice,
                                        queueFamilyIndex, hudWidth, hudHeight);
                                }

                                uint64_t hh = dxr::HashBytes(&inputSnapshot.hudVisible,
                                    sizeof(inputSnapshot.hudVisible));
                                auto hashW = [&hh](const std::wstring& s) {
                                    hh = dxr::HashBytes(s.data(), s.size() * sizeof(wchar_t), hh);
                                };
                                hashW(sessionText); hashW(modeText); hashW(dispText); hashW(helpText);
                                for (const HudButton& b : buttons) {
                                    hashW(b.label);
                                    hh = dxr::HashBytes(&b.hovered, sizeof(b.hovered), hh);
                                }
                                if (inputSnapshot.hudVisible) {
                                    // Body visible: fold the volatile text in on a 250 ms
                                    // bucket so the numbers refresh at 4 Hz.
                                    const uint64_t bucket = GetTickCount64() / 250;
                                    hh = dxr::HashBytes(&bucket, sizeof(bucket), hh);
                                }

                                if (s_hudUpInit && s_hudUp.needsUpload(hh)) {
                                    uint32_t hudImageIndex;
                                    if (AcquireHudSwapchainImage(*xr, hudImageIndex)) {
                                        uint32_t srcRowPitch = 0;
                                        const void* pixels = RenderHudAndMap(*hud, &srcRowPitch, sessionText, modeText, perfText, dispText, eyeText,
                                            cameraText, stereoText, helpText, buttons,
                                            /*drawBody=*/inputSnapshot.hudVisible,
                                            /*bodyAtBottom=*/true);
                                        if (pixels) {
                                            VkImage hudImg = (*hudSwapchainImages)[hudImageIndex].image;
                                            if (s_hudUp.upload(graphicsQueue, hudImg, pixels,
                                                    srcRowPitch, hudWidth * 4, hudHeight, hh)) {
                                                s_hudUploadedOnce = true;
                                            }
                                            UnmapHud(*hud);
                                        }
                                        ReleaseHudSwapchainImage(*xr);
                                    }
                                }
                                hudSubmitted = s_hudUploadedOnce;
                            }
                        }

                    }
                }

                // ── Toast layer ─────────────────────────────────────────────
                // Built and submitted only on the frames dxr::ToastState says a
                // message is live; once it expires the layer is simply absent
                // (a true toggle, not a transparent layer). Its own swapchain,
                // so it survives the transparent-mode chrome blackout above.
                XrCompositionLayerWindowSpaceDXR toastLayer = {};
                bool toastLayerReady = false;
                if (rendered && g_toastReady && g_hasToastSwapchain) {
                    std::wstring toastText;
                    float toastAlpha = 1.0f;
                    if (g_toast.Snapshot(toastText, toastAlpha)) {
                        static dxr::CachedLayerUploader s_toastUp;
                        static bool s_toastUpInit = false;
                        static bool s_toastUploadedOnce = false;
                        if (!s_toastUpInit) {
                            s_toastUpInit = s_toastUp.init(vkDevice, physDevice, queueFamilyIndex,
                                                           TOAST_TEX_W, TOAST_TEX_H);
                        }
                        // Alpha is quantised into the hash so the fade-out
                        // re-rasterizes, but a steady message does not.
                        const uint32_t alphaQ = (uint32_t)(toastAlpha * 32.0f);
                        uint64_t th = dxr::HashBytes(toastText.data(),
                                                     toastText.size() * sizeof(wchar_t));
                        th = dxr::HashBytes(&alphaQ, sizeof(alphaQ), th);
                        if (s_toastUpInit && s_toastUp.needsUpload(th)) {
                            uint32_t pitch = 0;
                            const void* px = RenderToastStandalone(g_toastHud, &pitch,
                                                                   toastText, toastAlpha);
                            uint32_t idx = 0;
                            if (px && AcquireWindowSpaceImage(g_toastSwapchain, idx)) {
                                if (s_toastUp.upload(graphicsQueue, g_toastSwapImages[idx].image,
                                                     px, pitch, TOAST_TEX_W * 4, TOAST_TEX_H, th)) {
                                    s_toastUploadedOnce = true;
                                }
                                ReleaseWindowSpaceImage(g_toastSwapchain);
                            }
                            if (px) UnmapHud(g_toastHud);
                        }
                        if (s_toastUploadedOnce) {
                            const dxr::ToastLayerRect tr = dxr::ComputeToastLayerRect(
                                windowW, windowH, (float)TOAST_TEX_W / (float)TOAST_TEX_H,
                                TOAST_SIZE_FRACTION, TOAST_Y_FRACTION);
                            toastLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
                            toastLayer.next = nullptr;
                            toastLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                            toastLayer.subImage.swapchain = g_toastSwapchain.swapchain;
                            toastLayer.subImage.imageRect.offset = {0, 0};
                            toastLayer.subImage.imageRect.extent = {(int32_t)TOAST_TEX_W,
                                                                    (int32_t)TOAST_TEX_H};
                            toastLayer.subImage.imageArrayIndex = 0;
                            toastLayer.x = tr.x;
                            toastLayer.y = tr.y;
                            toastLayer.width = tr.width;
                            toastLayer.height = tr.height;
                            toastLayer.disparity = 0.0f;
                            toastLayerReady = true;
                            // One-shot (never per-frame): proves the layer
                            // actually reached xrEndFrame, which is otherwise
                            // only checkable by eye on the panel.
                            static bool s_toastLoggedOnce = false;
                            if (!s_toastLoggedOnce) {
                                s_toastLoggedOnce = true;
                                LOG_INFO("Toast layer submitted: rect=(%.3f,%.3f %.3fx%.3f) "
                                         "of a %ux%u window",
                                         tr.x, tr.y, tr.width, tr.height, windowW, windowH);
                            }
                        }
                    }
                }

                // Submit frame
                uint32_t submitViewCount = (xr->renderingModeCount > 0 && xr->currentModeIndex < xr->renderingModeCount) ? xr->renderingModeViewCounts[xr->currentModeIndex] : 2;
                if (submitViewCount == 0) submitViewCount = 1;
                if (submitViewCount > 8) submitViewCount = 8;  // matches projectionViews[8] sizing
                if (rendered && (hudSubmitted || toastLayerReady)) {
                    // Layer footprint sized per-frame to match the HUD
                    // swapchain's aspect (computed above as layerFracW ×
                    // layerFracH), so the runtime's swapchain→layer rect
                    // mapping is uniform across both axes — glyphs and
                    // buttons keep their proportions on any tile aspect.
                    // Empty regions stay alpha=0 (compositor honors source
                    // alpha for window-space layers).
                    //
                    // SOURCE_ALPHA on the projection layer: displayxr::common's
                    // EndFrame family defaults projectionLayerFlags to 0, so
                    // this demo passes the bit explicitly (its vendored copy
                    // used to hardcode it) — required for the Ctrl+T
                    // transparent-background path; a no-op when opaque.
                    //
                    // The toast rides as an extra window-space layer with its
                    // own swapchain, so it composites on top of the HUD and
                    // shows even on the frames where `submitHud` is false
                    // (transparent mode hides all other chrome).
                    EndFrameWithWindowSpaceLayers(*xr, frameState.predictedDisplayTime, projectionViews,
                        0.0f, 0.0f, layerFracW, layerFracH, 0.0f, submitViewCount,
                        toastLayerReady ? &toastLayer : nullptr, toastLayerReady ? 1u : 0u,
                        0, 0, -1, -1,
                        /*submitHud=*/hudSubmitted,
                        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                } else if (rendered) {
                    EndFrame(*xr, frameState.predictedDisplayTime, projectionViews, submitViewCount,
                        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
                } else {
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr->session, &endInfo);
                }
            }
        } else {
            Sleep(100);
        }
    }

    if (renderCmdPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(vkDevice, renderCmdPool, nullptr);

    if (xr->exitRequested && g_running.load()) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }

    LOG_INFO("[RenderThread] Exiting");
}

// Global crash handler
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exInfo) {
    const char* excName = "UNKNOWN";
    switch (exInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:      excName = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        excName = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    excName = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   excName = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_IN_PAGE_ERROR:         excName = "IN_PAGE_ERROR"; break;
        case EXCEPTION_GUARD_PAGE:            excName = "GUARD_PAGE"; break;
    }
    LOG_ERROR("!!! UNHANDLED EXCEPTION: %s (0x%08X) at address 0x%p !!!",
        excName, exInfo->ExceptionRecord->ExceptionCode,
        exInfo->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    SetUnhandledExceptionFilter(CrashHandler);

    if (!InitializeLogging(APP_NAME)) {
        MessageBox(nullptr, L"Failed to initialize logging", L"Warning", MB_OK | MB_ICONWARNING);
    }

    LOG_INFO("=== SR 3DGS OpenXR Ext Vulkan Application ===");

    // ── The undock launch contract ──────────────────────────────────────────
    // GetCommandLineW, not WinMain's ANSI lpCmdLine, so a non-ASCII path
    // survives. One parse, one policy — see displayxr-common/common/launch_args.h.
    g_launch = dxr::ParseLaunchArgsFromCommandLine();
    LOG_INFO("Launch: transparent=%d rect=%d(%d,%d %dx%d) src=%s(%s) positional='%s' "
             "vh=%d(%.3f) dpr=%d(%.2f) type='%s' title='%s' protocol=%d maxBytes=%llu noCache=%d",
             g_launch.transparent ? 1 : 0, g_launch.hasRect ? 1 : 0,
             g_launch.rectX, g_launch.rectY, g_launch.rectW, g_launch.rectH,
             g_launch.src.c_str(),
             g_launch.srcKind == dxr::LaunchSrcKind::Url ? "url"
                 : g_launch.srcKind == dxr::LaunchSrcKind::LocalPath ? "path" : "none",
             g_launch.positionalPath.c_str(),
             g_launch.hasVh ? 1 : 0, g_launch.vh, g_launch.hasDpr ? 1 : 0, g_launch.dpr,
             g_launch.type.c_str(), g_launch.title.c_str(), g_launch.fromProtocol ? 1 : 0,
             (unsigned long long)g_launch.maxBytes, g_launch.noCache ? 1 : 0);
    for (const std::string& w : g_launch.warnings) LOG_WARN("launch: %s", w.c_str());
    // An undocked viewer must run its OWN in-process compositor. A protocol handler
    // inherits the browser's environment (XRT_FORCE_MODE=ipc), which would make this
    // process an IPC client that is not the panel owner: 2D whenever the browser holds
    // the panel, no drag phase-snap. Must run before xrCreateInstance loads the runtime.
    // Inherited IPC routing cannot be overridden in place (the runtime DLL's dynamic CRT
    // snapshots the environment at process start), so re-launch once with a scrubbed
    // block and let the child run. The in-place override below stays as the fallback.
    if (dxr::ReexecWithCleanRuntimeEnvIfNeeded(g_launch)) {
        LOG_WARN("launch: inherited IPC routing -> re-launched with a clean environment; this instance exits");
        ShutdownLogging();
        return 0;
    }
    if (dxr::ForceInProcessRuntimeForUndock(g_launch))
        LOG_WARN("launch: inherited IPC routing overridden -> XRT_FORCE_MODE=native (undock is standalone)");
    if (!g_launch.ok()) {
        for (const std::string& e : g_launch.errors) LOG_ERROR("launch: %s", e.c_str());
        // A protocol launch has no console to read the error from, and the
        // browser already asked the user to open this app — say why nothing
        // appeared. A CLI launch gets the log and a non-zero exit code.
        if (g_launch.fromProtocol) {
            LaunchMessageBox(dxr::WideFromUtf8(g_launch.errors[0]), MB_ICONERROR);
        }
        ShutdownLogging();
        return 2;
    }

    // Register HKCU\Software\Classes\displayxr-view -> this exe. Done by the
    // VIEWER, not the installer: the NSIS installers run elevated, so their HKCU
    // writes land in the elevating admin's hive. Idempotent; a LIVE sibling that
    // already owns the scheme is left alone (type forwarding covers that).
    {
        const bool reg = dxr::EnsureViewProtocolRegistered(
            dxr::ThisExePath(), L"DisplayXR Gaussian Splat Viewer");
        LOG_INFO("displayxr-view protocol association: %s", reg ? "usable" : "FAILED");
    }

    if (g_launch.fromProtocol) {
        // (a) Wrong viewer for this asset. One scheme serves every viewer so the
        // browser only asks once; whoever the OS launched forwards by type.
        if (!g_launch.type.empty() && g_launch.type != "splat") {
            const std::wstring sibling =
                dxr::FindSiblingViewer(L"ModelViewer", L"model_viewer_handle_vk_win.exe");
            if (sibling.empty()) {
                LOG_ERROR("type='%s' is not ours and the model viewer is not installed",
                          g_launch.type.c_str());
                LaunchMessageBox(
                    L"This link opens a 3D model, which needs the DisplayXR 3D Model Viewer.\n\n"
                    L"Please install the DisplayXR 3D Model Viewer and try again.",
                    MB_ICONINFORMATION);
                ShutdownLogging();
                return 3;
            }
            const bool launched = dxr::LaunchViewerWithUrl(
                sibling, dxr::WideFromUtf8(g_launch.protocolUrl));
            LOG_INFO("Forwarded type='%s' to %ls: %s", g_launch.type.c_str(), sibling.c_str(),
                     launched ? "ok" : "FAILED");
            ShutdownLogging();
            return launched ? 0 : 3;
        }
        // (b) Already running? Hand the URL to that instance and exit, so a page
        // that undocks twice retargets one window instead of stacking viewers.
        g_singleInstanceMutex = dxr::AcquireSingleInstanceOrForward(
            L"Local\\DisplayXR.GaussianSplat.Undock", WINDOW_CLASS, g_launch.protocolUrl);
        if (g_singleInstanceMutex == nullptr) {
            LOG_INFO("Forwarded the URL to the running instance; exiting");
            ShutdownLogging();
            return 0;
        }
    }

    // Mirror the launch into the render-thread-visible flags before anything
    // that reads them (the bundled auto-load runs during init, below).
    if (g_launch.hasVh) {
        g_vhPinned.store(true, std::memory_order_relaxed);
        g_vhPinnedValue.store(g_launch.vh, std::memory_order_relaxed);
    }
    if (g_launch.srcKind != dxr::LaunchSrcKind::None || !g_launch.positionalPath.empty()) {
        g_suppressBundledAutoLoad.store(true, std::memory_order_relaxed);
    }
    g_transparentBg.store(g_launch.transparent);
    g_borderless.store(g_launch.transparent);

    // Drift guard: assert the view/unproject/orientation conventions are
    // self-consistent (round-trip + +NDC->+world). A non-zero result means the
    // render-vs-pick frame math has drifted — fail loud rather than ship a
    // silently-inverted picker (the head->feet bug class). Mirrors macOS.
    {
        int stFails = display3d_selftest();
        if (stFails == 0)
            LOG_INFO("display3d self-test passed (view/unproject/orientation consistent)");
        else
            LOG_ERROR("display3d self-test FAILED with %d check(s) — pick ray math has drifted", stFails);
    }

    // Add DisplayXR to DLL search path
    {
        HKEY hKey;
        char installPath[MAX_PATH] = {0};
        DWORD pathSize = sizeof(installPath);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\DisplayXR\\Runtime", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)installPath, &pathSize) == ERROR_SUCCESS) {
                LOG_INFO("Adding DisplayXR install path to DLL search: %s", installPath);
                SetDllDirectoryA(installPath);
            }
            RegCloseKey(hKey);
        }
    }

    // Initialize OpenXR FIRST — xrGetSystemProperties needs only instance +
    // system id, and returns the 3D panel desktop position the window below
    // is created at (INV-1.3 ordering: instance → system → properties →
    // window → session; runtime#715).
    XrSessionManager xr = {};
    g_xr = &xr;
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }

    // Create the app window on the 3D panel
    int32_t panelLeft = 0, panelTop = 0;
    GetDisplayDesktopPosition(panelLeft, panelTop);
    // --rect=X,Y,W,H replaces the default placement. Physical virtual-screen
    // pixels (this exe declares PerMonitorV2, so no DPI virtualisation).
    if (g_launch.hasRect) {
        int32_t rx = g_launch.rectX, ry = g_launch.rectY;
        ClampRectIntoPanel(rx, ry, g_launch.rectW, g_launch.rectH);
        LOG_INFO("Launch rect: requested (%d,%d %dx%d) -> final (%d,%d %dx%d) "
                 "panel=(%d,%d %dx%d) confirmed=%d dpr=%.2f",
                 g_launch.rectX, g_launch.rectY, g_launch.rectW, g_launch.rectH,
                 rx, ry, g_launch.rectW, g_launch.rectH,
                 g_displayDesktopRect.offset.x, g_displayDesktopRect.offset.y,
                 g_displayDesktopRect.extent.width, g_displayDesktopRect.extent.height,
                 g_displayPanelConfirmed ? 1 : 0, g_launch.hasDpr ? g_launch.dpr : 0.0f);
        panelLeft = rx;
        panelTop = ry;
        g_windowWidth = (UINT)g_launch.rectW;
        g_windowHeight = (UINT)g_launch.rectH;
    }
    HWND hwnd = CreateAppWindow(hInstance, (int)g_windowWidth, (int)g_windowHeight,
                                panelLeft, panelTop, g_launch.transparent,
                                dxr::WideFromUtf8(g_launch.title));
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        CleanupOpenXR(xr);
        g_xr = nullptr;
        ShutdownLogging();
        return 1;
    }
    // Publish for GetAutoFitViewport (the bundled auto-load fits before ShowWindow).
    g_appWindow = hwnd;

    // Try to load sim_display_set_output_mode
    {
        HMODULE rtModule = GetModuleHandleA("openxr_displayxr.dll");
        if (!rtModule) rtModule = GetModuleHandleA("openxr_displayxr");
        if (rtModule) {
            g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)GetProcAddress(rtModule, "sim_display_set_output_mode");
        }
        LOG_INFO("sim_display output mode: %s", g_pfnSetOutputMode ? "available" : "not available");
    }

    // Get Vulkan graphics requirements
    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Find graphics queue family
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        LOG_ERROR("No graphics queue family found");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(xr, physDevice, queueFamilyIndex, vkDevice, graphicsQueue)) {
        LOG_ERROR("Vulkan device creation failed");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex, 0, hwnd)) {
        LOG_ERROR("OpenXR session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Reference space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        ShutdownLogging();
        return 1;
    }

    // Enumerate Vulkan swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    {
        uint32_t count = xr.swapchain.imageCount;
        swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
            (XrSwapchainImageBaseHeader*)swapchainImages.data());
        LOG_INFO("Enumerated %u Vulkan swapchain images", count);

        // Extract VkImage handles for render thread access
    }
    std::vector<VkImage> swapchainVkImages(swapchainImages.size());
    for (uint32_t i = 0; i < (uint32_t)swapchainImages.size(); i++) {
        swapchainVkImages[i] = swapchainImages[i].image;
    }

    // Initialize 3DGS renderer with the OpenXR Vulkan device
    {
        uint32_t renderW = xr.swapchain.width;   // Full width — mono uses entire swapchain
        uint32_t renderH = xr.swapchain.height;
        if (!g_gsRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue,
                               queueFamilyIndex, renderW, renderH)) {
            LOG_WARN("3DGS renderer init failed - scene rendering will not be available");
        } else {
            // Startup scene selection. A local asset named on the command line
            // loads here, synchronously, exactly where the bundled sample would
            // have — the render thread does not exist yet, so this is the one
            // moment loadScene may run off it. A URL cannot: it is kicked after
            // the render thread is up, so the toast can report it.
            if (g_launch.srcKind == dxr::LaunchSrcKind::LocalPath) {
                LoadSceneAtStartup(dxr::NarrowPathForFopen(g_launch.src), "Loading --src");
            } else if (!g_launch.positionalPath.empty()) {
                LoadSceneAtStartup(dxr::NarrowPathForFopen(g_launch.positionalPath),
                                   "Loading command-line scene");
            }
            TryAutoLoadBundledScene();
        }
    }

    // Initialize HUD renderer
    uint32_t hudWidth = (uint32_t)(xr.swapchain.width * HUD_WIDTH_FRACTION);
    uint32_t hudHeight = (uint32_t)(xr.swapchain.height * HUD_HEIGHT_FRACTION);

    HudRenderer hudRenderer = {};
    uint32_t hudFontBaseHeight = (uint32_t)(xr.swapchain.height * HUD_FONT_BASE_FRACTION);
    bool hudOk = InitializeHudRenderer(hudRenderer, hudWidth, hudHeight, hudFontBaseHeight);
    if (!hudOk) {
        LOG_WARN("HUD renderer init failed - HUD will not be displayed");
    }

    // Create HUD swapchain
    std::vector<XrSwapchainImageVulkanKHR> hudSwapImages;
    if (hudOk) {
        if (CreateHudSwapchain(xr, hudWidth, hudHeight)) {
            uint32_t count = xr.hudSwapchain.imageCount;
            hudSwapImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(xr.hudSwapchain.swapchain, count, &count,
                (XrSwapchainImageBaseHeader*)hudSwapImages.data());
            LOG_INFO("HUD swapchain: enumerated %u Vulkan images", count);
        } else {
            LOG_WARN("HUD swapchain creation failed - HUD will not be displayed");
            hudOk = false;
        }
    }

    // Create HUD staging buffer
    VkBuffer hudStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory hudStagingMemory = VK_NULL_HANDLE;
    void* hudStagingMapped = nullptr;
    VkCommandPool hudCmdPool = VK_NULL_HANDLE;

    if (hudOk) {
        VkBufferCreateInfo bufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = (VkDeviceSize)hudWidth * hudHeight * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(vkDevice, &bufInfo, nullptr, &hudStagingBuffer) != VK_SUCCESS) {
            LOG_WARN("Failed to create HUD staging buffer");
            hudOk = false;
        }

        if (hudOk) {
            VkMemoryRequirements memReqs;
            vkGetBufferMemoryRequirements(vkDevice, hudStagingBuffer, &memReqs);

            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

            uint32_t memTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                if ((memReqs.memoryTypeBits & (1 << i)) &&
                    (memProps.memoryTypes[i].propertyFlags &
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memTypeIndex = i;
                    break;
                }
            }

            if (memTypeIndex == UINT32_MAX) {
                LOG_WARN("No suitable memory type for HUD staging buffer");
                hudOk = false;
            } else {
                VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = memTypeIndex;
                vkAllocateMemory(vkDevice, &allocInfo, nullptr, &hudStagingMemory);
                vkBindBufferMemory(vkDevice, hudStagingBuffer, hudStagingMemory, 0);
                vkMapMemory(vkDevice, hudStagingMemory, 0, bufInfo.size, 0, &hudStagingMapped);
            }
        }

        if (hudOk) {
            VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndex;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &hudCmdPool) != VK_SUCCESS) {
                LOG_WARN("Failed to create HUD command pool");
                hudOk = false;
            }
        }

        if (hudOk) {
            LOG_INFO("HUD Vulkan resources created (%ux%u)", hudWidth, hudHeight);
        }
    }

    // ── Toast window-space layer ────────────────────────────────────────────
    // Its own small swapchain rather than a corner of the HUD, because the HUD
    // (and every other piece of chrome) is HIDDEN in transparent mode by design
    // — which is precisely when a download most needs to say something. Failure
    // is non-fatal: the log still carries the progress.
    {
        if (InitializeHudRenderer(g_toastHud, TOAST_TEX_W, TOAST_TEX_H, TOAST_FONT_BASE) &&
            CreateWindowSpaceSwapchain(xr, g_toastSwapchain, TOAST_TEX_W, TOAST_TEX_H)) {
            uint32_t c = g_toastSwapchain.imageCount;
            g_toastSwapImages.resize(c, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(g_toastSwapchain.swapchain, c, &c,
                (XrSwapchainImageBaseHeader*)g_toastSwapImages.data());
            g_hasToastSwapchain = true;
            g_toastReady = true;
            LOG_INFO("Toast swapchain: %ux%u, %u images", TOAST_TEX_W, TOAST_TEX_H, c);
        } else {
            LOG_WARN("Toast layer unavailable - progress will only reach the log");
        }
    }

    // A --transparent launch must not steal focus: it is a floating overlay a
    // page spawned, not a window the user asked to switch to.
    ShowWindow(hwnd, g_launch.transparent ? SW_SHOWNOACTIVATE : nCmdShow);
    UpdateWindow(hwnd);

    LOG_INFO("");
    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: WASDEQ=Move  LMB-drag=Rotate  Scroll=Zoom  DblClick=Focus");
    LOG_INFO("          -/= Depth  Space=Reset  M=Auto-Orbit  V=Mode");
    LOG_INFO("          L=Load  Tab=HUD  F11=Fullscreen  ESC=Quit");
    LOG_INFO("");

    // --vh seeds the fallback: with a URL src there is no scene yet, so this is
    // the value the first frames actually render at.
    g_inputState.viewParams.virtualDisplayHeight =
        g_vhPinned.load(std::memory_order_relaxed) ? g_vhPinnedValue.load(std::memory_order_relaxed)
                                                   : kFallbackVirtualDisplayHeightM;
    g_inputState.renderingModeCount = xr.renderingModeCount;
    // Align runtime active rendering mode with app's default (mode 1 = first 3D mode).
    // The main loop's dispatch picks this up on the first frame and calls
    // xrRequestDisplayRenderingModeDXR(1); the runtime event drives xr.currentModeIndex.
    g_inputState.absoluteRenderingModeRequested = 1;
    g_inputState.hudVisible = false;     // hidden by default; toggle with Tab
    g_inputState.animateEnabled = true;  // auto-orbit always on after 10 s idle
    {
        using namespace std::chrono;
        g_inputState.lastInputTimeSec = (double)duration_cast<microseconds>(
            high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
    }

    std::thread renderThread(RenderThreadFunc, hwnd, &xr, vkDevice, graphicsQueue,
        queueFamilyIndex, vkInstance, physDevice,
        &swapchainVkImages,
        hudOk ? &hudRenderer : nullptr, hudWidth, hudHeight,
        hudStagingBuffer, hudStagingMapped, hudCmdPool,
        hudOk ? &hudSwapImages : nullptr,
        (VkCommandPool)VK_NULL_HANDLE, (std::vector<XrSwapchainImageVulkanKHR>*)nullptr,
        (uint32_t)0, (uint32_t)0);

    // --src=<url>: start the download now that the render thread (and with it
    // the toast) is alive, so progress is visible from the first byte.
    if (g_launch.srcKind == dxr::LaunchSrcKind::Url) {
        StartUrlFetch(hwnd, g_launch.src, g_launch.maxBytes, g_launch.noCache);
    }

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running.store(false);
    LOG_INFO("Main thread: waiting for render thread...");
    renderThread.join();
    LOG_INFO("Main thread: render thread joined");

    LOG_INFO("");
    LOG_INFO("=== Shutting down ===");

    g_gsRenderer.cleanup();

    if (hudCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, hudCmdPool, nullptr);
    if (hudStagingBuffer != VK_NULL_HANDLE) {
        vkUnmapMemory(vkDevice, hudStagingMemory);
        vkDestroyBuffer(vkDevice, hudStagingBuffer, nullptr);
    }
    if (hudStagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, hudStagingMemory, nullptr);
    if (hudOk) CleanupHudRenderer(hudRenderer);

    // Toast layer — destroy the swapchain before CleanupOpenXR tears the
    // session down under it.
    if (g_toastReady) CleanupHudRenderer(g_toastHud);
    if (g_toastSwapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_toastSwapchain.swapchain);
        g_toastSwapchain.swapchain = XR_NULL_HANDLE;
        g_hasToastSwapchain = false;
    }

    g_xr = nullptr;
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    DestroyWindow(hwnd);
    UnregisterClass(WINDOW_CLASS, hInstance);

    // Released last: while this handle is open we are THE instance, and a
    // second launch forwards to us instead of opening a rival window.
    if (g_singleInstanceMutex) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }

    LOG_INFO("Application shutdown complete");
    ShutdownLogging();

    return 0;
}

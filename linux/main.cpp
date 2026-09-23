// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux Vulkan OpenXR 3D Gaussian Splatting viewer (handle app, X11 or Wayland).
 *
 * Linux leg of displayxr-demo-gaussiansplat (#60 build-green harness, upgraded
 * to a real handle app by #76). Mirrors the macOS/Windows legs' OpenXR +
 * Vulkan + gs_renderer flow.
 *
 * WINDOWING — HANDLE app, ONE binary for X11 and native Wayland: the window is
 * displayxr-common's displayxr::linux_window (dxr_linux_window.h), the one
 * Linux window implementation shared with the runtime's test apps and the
 * other demos. The platform is chosen by capability at startup
 * (--platform=x11|wayland|auto; auto = native Wayland when the compositor is
 * ready, else X11 — never from session env vars), and the helper passes
 * XR_DXR_xlib_window_binding or XR_DXR_wayland_surface_binding, so the
 * runtime weaves window-relative and the app receives input. The window
 * defaults to 1920x1080 centered on the panel (XR_DXR_display_info desktop
 * rect); GAUSS_WINDOW="WxH+X+Y" overrides (X,Y absolute virtual-desktop px,
 * X11 only). It carries the shared client-side header bar: LMB on the bar
 * moves the window (phase-snapped on X11), LMB below it orbits the scene.
 * When no window system answers the app falls back to hosted-NULL — which
 * also keeps it startable on the build-green CI runner.
 *
 * INPUT — Ctrl+O opens a zenity file-selection dialog (async fork/exec, no
 * hard dependency: absent zenity logs and no-ops) and loads the chosen
 * .spz/.ply scene. The camera is otherwise the auto-orbit the harness always
 * had; the runtime supplies per-view rig poses via XR_DXR_view_rig.
 *
 * Renderer: selected by linux/CMakeLists.txt (GS_RENDERER, default GRAPHICS —
 * the graphics-pipeline splat path; COMPUTE = the legacy generic path).
 */

#include <vulkan/vulkan.h>

// The Linux window (displayxr::linux_window) — before the OpenXR platform
// header so the window-binding structs see the real Display/Window/wl_* types.
// Keys arrive as X11 keysyms on both backends.
#include "dxr_linux_window.h"
#include "dxr_weave_snap.h"
#include <X11/keysym.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>
#include <openxr/XR_DXR_weave.h>                  // xrWeaveSnapWindowRectDXR — drag-time phase snap
#include "dxr_view_config.h"   // DxrSelectViewConfigType + DxrAliasInactiveViews (displayxr::rules; runtime#1486, ADR-041)

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ── The app headers, fenced against X11's `None` ────────────────────────────
//
// <X11/X.h> defines `None` as the object-ID macro `0L`. Two shared headers
// declare an enumerator of that name — `dxr::LaunchSrcKind::None` in
// launch_args.h and `GsBoundsSource::None` in gs_scene_fit.h — and with Xlib
// included first each expands to a numeric constant and stops the header
// parsing ("expected identifier before numeric constant"). `None` is a
// perfectly ordinary C++ enumerator; X11 is the one misbehaving.
//
// The macro cannot simply be dropped: Xlib code included above relies on it.
// So it is pushed aside for the whole app-header block and
// restored afterwards — one fence rather than one per header, because this has
// now happened twice and the next shared header to spell a word X11 claimed
// should not need a third. macOS and Windows never see any of it, which is why
// the Linux lane, and only the Linux lane, goes red on it.
#pragma push_macro("None")
#undef None
#include "gs_renderer_select.h"   // GsActiveRenderer (GS_RENDERER in linux/CMakeLists.txt)
#include "gs_camera_rig.h"        // GsCameraRig / GsRigFlags — the photo-lifted rig
#include "gs_scene_fit.h"         // the shared, depth-aware display-rig fit
#include "launch_args.h"          // dxr::ParseLaunchArgs — the shared --key=value grammar
#include "mode_switch.h"          // dxr::ModeSwitch — the shared 2D<->3D ramp (V / 0-8 keys)
#include "clip_policy.h"          // dxr::ResolveClipPlanes — the transparent-mode ZDP clip
#include "color_policy.h"         // dxr::DisplayReferredToSceneLinear (placeholder clear)
#include "gs_vulkan_utils.h"      // gsIsSrgbFormat
#include "clickthrough.h"         // input-region punch-through (Ctrl+T transparent mode)
#pragma pop_macro("None")

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static GsActiveRenderer g_gsRenderer;

// Tile basis for a handle app: the app window (window × viewScale, #729-style).
// Falls back to the display size on the hosted-NULL path.
static uint32_t g_windowW = 1920, g_windowH = 1080;

// ── The two rigs (see 3dgs_common/gs_camera_rig.h) ──────────────────────────
// Display rig (everything this leg had before): a fixed 1.5 m virtual display
// with the auto-orbit turntable. Camera rig: frame a photo-lifted scene through
// the camera it was predicted from. The rig is chosen per SCENE — a `camera`
// block in a SOG meta.json means camera rig, its absence means display rig —
// and `--rig=` overrides. g_cameraRigActive is the single live flag; when it is
// false every path below behaves exactly as it did before.
static dxr::LaunchArgs g_launch;        //!< shared flags (--src/--vh/--pose/...)
static GsRigFlags      g_rigFlags;      //!< rig flags (--rig/--fx/--size/...)
static GsCameraRig     g_camRig;        //!< resolved camera rig, valid while active
static bool            g_cameraRigActive = false;
//! Eye spread the runtime last reported for this display, metres (the raw
//! channel's rawEyes[]). The camera rig's ipdFactor scales THAT, so measuring
//! it is what makes the rendered pair exactly the capture baseline instead of
//! whatever a nominal face happens to be. 0 until the first locate lands.
static float           g_camRigMeasuredIpdM = 0.0f;

// ── Anchoring the camera rig's REST view to the capture camera ───────────────
//
// XrCameraRigDXR is defined against the DISPLAY: its eye displacement is the
// tracked eye measured from the panel's axis, and this panel's nominal viewer
// does not sit on that axis. The rig faithfully renders that: at rest, with
// nothing tracked, it puts the eye above the declared camera and shears the
// window to match. For a head-tracked scene that is exactly right; for a PHOTO
// it is not — the rest view has to BE the photograph.
//
// So the app samples the untracked eye centroid and the untracked frustum
// centre once, then cancels both. Head motion is unaffected — it is a DELTA
// from that rest, and subtracting a constant leaves every delta intact.
// Sampled rather than assumed, because the offset is a property of the panel.
static bool  g_camRigRestSampled = false;
static float g_camRigRestEyeXY[2] = {0.0f, 0.0f};  //!< display-space x,y of the untracked centroid
static float g_camRigRestTan[2]   = {0.0f, 0.0f};  //!< tangent centre of the untracked frustum

// Scene orbit (camera rig only), radians. A drag turns the SCENE about the
// pivot — never the camera, whose eyes are the tracker's. Zero at rest, and
// zero for the whole life of a display-rig session.
static float g_camOrbitYaw = 0.0f, g_camOrbitPitch = 0.0f;
static bool  g_camDragging = false;
static int   g_camDragLastX = 0, g_camDragLastY = 0;

// ── Display-rig interaction (the parity gap this leg had) ───────────────────
//
// The Windows leg drives the display rig from InputState.{yaw,pitch} and
// viewParams.{scaleFactor,ipdFactor}; the render path there computes
// `rigVH = virtualDisplayHeight / scaleFactor` (windows/main.cpp:2339), so a
// bigger scaleFactor is a bigger subject. input_handler.h is <windows.h>-only,
// so the STATE is mirrored here and fed from X11 events with the same
// semantics, signs and clamps rather than a Linux-only invention.
//
// The turntable used to be unconditional on this leg. Windows gates it on the
// 'M' toggle and stops it the moment the user touches the scene, which is the
// behaviour a drag needs — an auto-advancing yaw fights the drag otherwise.
static float g_dispOrbitYaw = 0.0f;     //!< display-rig yaw, radians (drag + turntable)
static float g_dispOrbitPitch = 0.0f;   //!< display-rig pitch, radians (drag only)
static bool  g_dispDragging = false;
static int   g_dispDragLastX = 0, g_dispDragLastY = 0;
static bool  g_animateEnabled = true;   //!< 'M': turntable on/off (on = the old behaviour)
static bool  g_animationActive = false; //!< derived: animateEnabled && idle > 10 s
//! Wall-clock of the last user input, seconds. The turntable's 10 s idle gate
//! reads it; MarkUserInput() resets it. Mirrors InputState::lastInputTimeSec.
static double g_lastInputTimeSec = 0.0;
static float g_scaleFactor = 1.0f;      //!< wheel zoom, [0.1, 10] (Windows: viewParams.scaleFactor)
static float g_steadyIpd = 1.0f;        //!< '+/-' and shift+wheel 3D strength, [0.1, 1]
static float g_ipdFactor = 1.0f;        //!< live ipdFactor, driven by the ModeSwitch ramp

// ── 2D/3D mode switching ('V', '0'-'8') ─────────────────────────────────────
// The shared, platform-neutral sequencer displayxr-common already ships and
// the Windows leg uses (XrSessionUpdateModeSwitch wraps it there). It owns the
// asymmetry: 3D->2D ramps the disparity to zero BEFORE switching, 2D->3D
// switches first and eases up after.
static dxr::ModeSwitch g_modeSwitch;
static bool    g_cycleModeRequested = false;
static int32_t g_absoluteModeRequested = -1;

// ── The window (INV-1.3, runtime #1588) ────────────────────────────────────
//
// X11 or native Wayland, the header bar, the drag, F11 — displayxr-common's
// displayxr::linux_window. The interlace phase is a function of the window's
// absolute position in physical panel pixels, so the drag is the helper's: on
// X11 it owns the move and routes every step through xrWeaveSnapWindowRectDXR
// (DxrWeaveSnap); on Wayland the compositor runs it constrained to the drag
// lattice. The header bar replaces the invisible top strip this leg used to
// have: LMB on the bar moves the window, LMB anywhere below it ORBITS the scene
// — the Windows split (title-bar drag moves, client drag orbits).
// DXR_X11_WM_DECORATIONS=1 restores a decorated, WM-dragged X11 window (and
// forfeits the snap). Destroyed LAST (the runtime's VkSurfaceKHR borrows its
// connection).
static DxrLinuxWindow g_window;
static DxrWeaveSnap g_weaveSnap;

// Ctrl+T: opaque <-> transparent background — the Windows leg's
// g_transparentBg. The session is created transparent-capable (an ARGB window
// + transparentBackgroundEnabled, both fixed at creation, as Windows'
// xr_session.cpp sets it unconditionally); this flag only flips what the
// renderer writes: alpha 1 (opaque) or 1 - T, so the pixels no splat covers
// show the desktop. Starts OPAQUE (Windows parity) unless --transparent.
// g_transparentCapable drops to false when the window system has no ARGB
// visual or there is no app window (hosted-NULL), and Ctrl+T then refuses.
static bool g_transparentCapable = true;
static bool g_transparentBg = false;

// The Windows leg's RenderPlaceholder: clear the whole atlas to the viewer's
// slate while no scene is loaded, and hand it back in COLOR_ATTACHMENT_OPTIMAL
// (the layout renderEye leaves and the click-through expects). Opaque in both
// modes, as on Windows, so an empty transparent window stays visible and
// clickable instead of vanishing.
static VkCommandPool g_placeholderPool = VK_NULL_HANDLE;

static void RenderPlaceholder(VkDevice device, VkQueue queue, uint32_t queueFamily,
                              VkImage image, VkFormat format) {
    if (g_placeholderPool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(device, &pci, nullptr, &g_placeholderPool) != VK_SUCCESS) return;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_placeholderPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    // Authored display-referred (sRGB-encoded), like every colour picked by eye.
    static const float kPlaceholderRgb[3] = {0.1f, 0.1f, 0.12f};
    const bool srgbTarget = gsIsSrgbFormat(format);
    auto ch = [&](int i) {
        return srgbTarget ? dxr::DisplayReferredToSceneLinear(kPlaceholderRgb[i]) : kPlaceholderRgb[i];
    };
    VkClearColorValue color = {{ch(0), ch(1), ch(2), 1.0f}};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, g_placeholderPool, 1, &cmd);
}

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// ============================================================================
// Inline math — column-major float[16] matrices (mirrors the macOS leg)
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];
    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye Z for the ZDP-anchored clip (mirrors macOS RigLocalEyeZ).
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// ============================================================================
// Executable-relative path (for the bundled scene)
// ============================================================================

static std::string ExecutableDir() {
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char* slash = strrchr(buf, '/');
        if (slash) *slash = '\0';
        return std::string(buf);
    }
    return ".";
}

// ============================================================================
// Rig selection — run after every successful load
// ============================================================================

// Frame the freshly-loaded scene with whichever rig it asked for.
//
// This is the ONE place the choice is made, so every load path (startup, the
// zenity picker) agrees. Camera rig when the scene declared a capture camera
// (or --rig=camera supplied one), display rig otherwise — and a camera rig that
// cannot be resolved falls back to the display rig with a WARN rather than
// inventing intrinsics.
//
// The display rig's counterpart is ApplyAutoFitForLoadedScene (below the fit
// helpers): frame the bounds against the live content rect.
// ── Display-rig auto-fit (parity with the macOS / Windows legs) ─────────────
//
// This leg used to have NO auto-fit: a constant 1.5 m virtual display height
// with the rig at the origin, whatever the scene and whatever the window. The
// butterfly then overflowed the top of the content (both views clipped flat at
// the top edge, black margin below) — the header bar only made the content
// short enough to show it. Now the scene is framed exactly as the other legs
// frame it: the shared GsFitFrameEx (3dgs_common/gs_scene_fit.h) on the bounds
// measured at load, against the LIVE CONTENT rect (the bound surface, header
// bar excluded), with the same comfort-compensated 80% fill; and re-run when
// the content's aspect changes (a resize, the Wayland scale correction, F11),
// so the framing never goes stale. Only the BASE vHeight moves on a refit:
// the render path divides it by g_scaleFactor, so the user's zoom and orbit
// are untouched.
static constexpr float kFallbackVirtualDisplayHeightM = 1.5f;
// The bounds carry a 1.10x comfort margin on every axis, so the fill handed to
// the fit is 0.80 x 1.10 to net an 80% cap on the TRUE object (macOS / Windows
// kAutoFitFill, same derivation).
static constexpr float kAutoFitFill = 0.80f * 1.10f;
static float g_fitCenter[3] = {0.0f, 0.0f, 0.0f};
static float g_fitVHeight = kFallbackVirtualDisplayHeightM;
static float g_fitAspect = 0.0f;  //!< content aspect the current fit was derived for
static bool  g_fitValid = false;

static GsFitComfort ActiveFitComfort() {
    GsFitComfort c;
    if (g_rigFlags.hasFitDisparity) c.maxDisparityVH = g_rigFlags.fitDisparityVH;
    return c;
}

//! The display-rig fit for a viewport, in the selected --fit mode. `flood` (and
//! `legacy`, which on this leg frames the same bounds — the Linux renderers
//! cache one set) keep the flat x/y rule; `depth` lets the disparity budget
//! bound it. Same shape as the macOS / Windows RunFit.
static GsFitFrameResult RunFit(float viewportW, float viewportH, float yaw, float pitch) {
    GsFitFrameResult r = GsFitFrameEx(g_gsRenderer.fitBounds(), viewportW, viewportH, kAutoFitFill,
                                      yaw, pitch, ActiveFitComfort());
    if (g_rigFlags.fitMode != GsFitMode::Depth && r.valid) {
        r.vHeight = r.vHeightFlat;
        r.boundBy = (r.screenH > 0.0f && viewportH > 0.0f &&
                     (r.screenW * viewportH / viewportW) > r.screenH) ? "width" : "height";
    }
    return r;
}

//! Frame the loaded scene against the content rect (g_windowW x g_windowH —
//! the bound surface's pixels, header bar excluded).
static void ApplyAutoFitForLoadedScene() {
    const GsFitBounds& bounds = g_gsRenderer.fitBounds();
    const float vpW = (float)g_windowW, vpH = (float)g_windowH;
    if (!bounds.valid || !(vpW > 0.0f) || !(vpH > 0.0f)) {
        g_fitValid = false;
        g_fitVHeight = kFallbackVirtualDisplayHeightM;
        LOG_WARN("Fit: no bounds to frame — falling back to vHeight %.3f", kFallbackVirtualDisplayHeightM);
        return;
    }
    g_fitCenter[0] = bounds.center[0];
    g_fitCenter[1] = bounds.center[1];
    g_fitCenter[2] = bounds.center[2];
    const GsFitFrameResult fr = RunFit(vpW, vpH, 0.0f, 0.0f);
    float vh = fr.valid ? fr.vHeight : 0.0f;
    if (!(vh > 1e-3f)) vh = kFallbackVirtualDisplayHeightM;
    g_fitVHeight = vh;
    g_fitAspect = vpW / vpH;
    g_fitValid = true;
    LOG_INFO("Fit: center=(%.3f, %.3f, %.3f) screen=(%.3f, %.3f) content=%.0fx%.0f px aspect=%.3f "
             "bound=%s vHeight=%.3f (flat %.3f, depth asks %.3f)",
             bounds.center[0], bounds.center[1], bounds.center[2], fr.screenW, fr.screenH, vpW, vpH,
             g_fitAspect, fr.boundBy, vh, fr.vHeightFlat, fr.vHeightDepth);
}

//! Re-derive the base vHeight when the CONTENT aspect changes (resize, the
//! Wayland scale correction, F11). Zoom and orbit are preserved.
static void RefitForContent() {
    if (!g_fitValid || g_cameraRigActive || g_windowW == 0 || g_windowH == 0) return;
    const float aspect = (float)g_windowW / (float)g_windowH;
    if (g_fitAspect > 0.0f && std::fabs(aspect - g_fitAspect) < 1e-3f * g_fitAspect) return;
    const GsFitFrameResult fr = RunFit((float)g_windowW, (float)g_windowH, g_dispOrbitYaw, g_dispOrbitPitch);
    const float vh = fr.valid ? fr.vHeight : 0.0f;
    if (!(vh > 1e-3f)) return;
    LOG_INFO("Fit refit: content %ux%u aspect %.3f -> %.3f, bound=%s, base vHeight %.3f -> %.3f (zoom kept)",
             g_windowW, g_windowH, g_fitAspect, aspect, fr.boundBy, g_fitVHeight, vh);
    g_fitAspect = aspect;
    g_fitVHeight = vh;
}

static void ApplyRigForLoadedScene() {
    const GsSceneCamera& cam = g_gsRenderer.sceneCamera();
    // The last step of the waterfall: when nothing has declared a rig, ask the
    // cloud whether it looks like a photograph. A `.spz` or `.ply` conversion
    // of a lift carries no metadata at all, and framing one as an object gives
    // it a crop at the wrong field of view.
    const GsPhotoLiftSignature sig =
        GsDetectPhotoLift(g_gsRenderer.sceneMeasurements(), g_gsRenderer.fitBounds());
    LOG_INFO("Photo-lift signature: front=%.4f%% (need %.1f%%) focal=%.1fmm-eq [%s] "
             "origin-outside=%.3f (need %.3f) -> %s%s%s",
             sig.forwardFraction * 100.0f, kGsPhotoLiftMinForwardFrac * 100.0f,
             sig.focal35mm, sig.focalGatePassed ? "lens" : "not a lens",
             sig.originOutsideRatio, kGsPhotoLiftMinOriginOutside,
             sig.isPhotoLift ? "PHOTO LIFT" : "object scan",
             sig.failedTerm ? " (" : "",
             sig.failedTerm ? (std::string(sig.failedTerm) + ")").c_str() : "");

    if (GsSelectRigKind(cam, g_rigFlags, nullptr, &sig) == GsRigKind::Camera) {
        // Coarse fallback pivot for a --rig=camera override on a scene that
        // carries no camera: the forward depth of the main object's centre.
        float boundsDepth = 0.0f;
        // Only the main object's forward depth is wanted here, as a coarse
        // focus fallback; the fit bounds were measured at load by the shared
        // module (gs_scene_fit.h).
        {
            const GsFitBounds& fb = g_gsRenderer.fitBounds();
            if (fb.valid && -fb.center[2] > 0.0f) boundsDepth = -fb.center[2];
        }

        // The v2 waterfall takes everything the cloud says in one struct,
        // measured at load: the vertices are long gone by now.
        GsRigResolveInput rigIn;
        rigIn.measurements = &g_gsRenderer.sceneMeasurements();
        rigIn.boundsForwardDepthM = boundsDepth;
        std::string why;
        if (GsResolveCameraRig(cam, g_rigFlags, rigIn, g_camRig, &why)) {
            g_cameraRigActive = true;
            g_camOrbitYaw = 0.0f;       // the orbit is the SCENE's, and rest is 0
            g_camOrbitPitch = 0.0f;
            g_camRigRestSampled = false;  // re-anchor: a new scene, a new camera
            float du = 0.0f, dv = 0.0f;
            g_camRig.PrincipalShiftTan(du, dv);
            LOG_INFO("Camera rig: fx=%.3f fy=%.3f cx=%.1f cy=%.1f %dx%d baseline=%.4fm "
                     "pivot=%.3fm (%s) vFOV=%.1fdeg principal-shift=(%.5f, %.5f)",
                     g_camRig.fx, g_camRig.fy, g_camRig.cx, g_camRig.cy,
                     g_camRig.width, g_camRig.height, g_camRig.baselineM,
                     g_camRig.pivotM, g_camRig.focusSource.c_str(),
                     g_camRig.VerticalFovRad((float)g_camRig.width / (float)g_camRig.height) *
                         57.2957795f, du, dv);
            return;
        }
        LOG_WARN("Camera rig requested but %s — framing with the display rig instead",
                 why.c_str());
    }
    g_cameraRigActive = false;
    ApplyAutoFitForLoadedScene();
}

// ============================================================================
// OpenXR session (hosted-NULL — no window binding on Linux build-green)
// ============================================================================

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    char systemName[256] = {};

    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    bool hasDisplayInfoExt = false;
    bool hasViewRigExt = false;
    bool hasDepthBudgetExt = false;      //!< XR_DXR_depth_budget (the transparent-mode rear clip)
    bool hasXlibBindingExt = false;
    bool hasWaylandBindingExt = false;
    //! Window platform resolved before xrCreateInstance; Auto = hosted-NULL.
    DxrWindowBackend windowBackend = DxrWindowBackend::Auto;
    bool hasWeaveExt = false;   //!< XR_DXR_weave — drag-time phase snap only
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerZ = 0.5f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;
    int32_t displayScreenLeft = 0;     // 3D-panel top-left in virtual-desktop px (INV-1.3)
    int32_t displayScreenTop = 0;

    // App-owned window (g_window). False = hosted-NULL fallback. xWinW/H is
    // the CONTENT size (the bound window/surface, header bar excluded).
    bool hasAppWindow = false;
    unsigned int xWinW = 0, xWinH = 0;

    PFN_xrRequestDisplayRenderingModeDXR pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateDisplayRenderingModesEXT = nullptr;

    uint32_t renderingModeCount = 0;
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    // Index of the 3D mode the runtime reports ACTIVE at session create, or -1.
    // Only a 3D mode is a candidate: the display commonly reports its 2D mode
    // active at startup (it stays 2D until something asks for 3D), and adopting
    // that would open this 3D demo in mono.
    int32_t activeRenderingMode = -1;
    uint32_t currentRenderingMode = 1;   // default: first 3D mode

    // Views reported by xrEnumerateViewConfigurationViews under viewConfigType.
    // runtime#1486: the DEVICE MAX across rendering modes when the session opts
    // into PRIMARY_MULTIVIEW_DXR; exactly 2 under the PRIMARY_STEREO fallback.
    uint32_t maxViewCount = 2;
};

static bool InitializeOpenXR(AppXrSession& xr, DxrWindowBackend requestedBackend) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& ext : exts) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) xr.hasViewRigExt = true;
        if (strcmp(ext.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasXlibBindingExt = true;
        if (strcmp(ext.extensionName, XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME) == 0) xr.hasWaylandBindingExt = true;
        if (strcmp(ext.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0) xr.hasWeaveExt = true;
        if (strcmp(ext.extensionName, XR_DXR_DEPTH_BUDGET_EXTENSION_NAME) == 0) xr.hasDepthBudgetExt = true;
    }
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasViewRigExt) enabled.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    if (xr.hasDepthBudgetExt) enabled.push_back(XR_DXR_DEPTH_BUDGET_EXTENSION_NAME);
    // Window platform, resolved BEFORE xrCreateInstance so only the binding the
    // session will chain is enabled. A capability probe (connection attempt +
    // what the compositor advertises), never session env vars; an explicit
    // --platform wins. Nothing usable -> hosted-NULL.
    {
        std::string why;
        xr.windowBackend = DxrLinuxWindow::select(requestedBackend, xr.hasXlibBindingExt,
                                                  xr.hasWaylandBindingExt, &why);
        if (xr.windowBackend == DxrWindowBackend::Auto) {
            LOG_WARN("No usable window platform (%s) — hosted-NULL windowing", why.c_str());
        } else {
            LOG_INFO("Window platform: %s (requested %s) — %s", DxrLinuxWindow::backend_name(xr.windowBackend),
                     DxrLinuxWindow::backend_name(requestedBackend), why.c_str());
        }
    }
    if (xr.windowBackend == DxrWindowBackend::X11) enabled.push_back(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.windowBackend == DxrWindowBackend::Wayland)
        enabled.push_back(XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME);
    // XR_DXR_weave: this leg wants ONE entry point out of it —
    // xrWeaveSnapWindowRectDXR, the GPU-free query that says where a dragged
    // window may land on the lens lattice. Detect-then-push; never required.
    if (xr.hasWeaveExt) {
        enabled.push_back(XR_DXR_WEAVE_EXTENSION_NAME);
        LOG_INFO("XR_DXR_weave: AVAILABLE (enabled for the drag-time phase snap)");
    } else {
        LOG_INFO("XR_DXR_weave: NOT FOUND — window drags will not be phase-snapped");
    }

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "SR3DGSOpenXRExtLinux",
            sizeof(ci.applicationInfo.applicationName) - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName) - 1);
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &xr.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

    // runtime#1486 / #1500 — PRIMARY_MULTIVIEW_DXR opt-in. Must run before the
    // first xrEnumerateViewConfigurationViews (CreateSwapchains below), and the
    // SAME xr.viewConfigType then feeds xrBeginSession's
    // primaryViewConfigurationType and every XrViewLocateInfo. This leg's view
    // count comes from the active DXR rendering mode, so under the conformant
    // PRIMARY_STEREO (exactly 2) it could not reach a 4-view Quad mode at all.
    // Degrades to PRIMARY_STEREO on any runtime that does not advertise the new
    // type, so it is safe to call unconditionally.
    xr.viewConfigType = DxrSelectViewConfigType(xr.instance, xr.systemId);
    LOG_INFO("View configuration: %s", DxrViewConfigTypeName(xr.viewConfigType));

    { XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
      xrGetSystemProperties(xr.instance, xr.systemId, &sp);
      memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName)); }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        di.next = &desktopPos;
        sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
            xr.displayScreenLeft = desktopPos.left;
            xr.displayScreenTop = desktopPos.top;
        }
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    LOG_INFO("OpenXR initialized: %s", xr.systemName);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static std::vector<std::string> SplitSpaceSeparated(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(' ', i);
        if (e == std::string::npos) e = s.size();
        std::string n = s.substr(i, e - i);
        if (!n.empty() && n[0] != '\0') out.push_back(n);
        i = e + 1;
    }
    return out;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames = SplitSpaceSeparated(extStr);
    std::vector<const char*> extPtrs;
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "SR3DGSOpenXRExtLinux";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)extPtrs.size();
    ci.ppEnabledExtensionNames = extPtrs.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& pd) {
    PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    LOG_INFO("GPU: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    storage = SplitSpaceSeparated(extStr);
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++)
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
    const std::vector<const char*>& exts, VkDevice& dev, VkQueue& queue) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = qfi; qi.queueCount = 1; qi.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures features = {};
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
    VkDevice dev, uint32_t qfi) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    // Handle app: the window helper hands back the binding for its platform
    // (xlib, or Wayland + its surface-geometry struct), bound to the CONTENT
    // (never the header bar), with transparentBackgroundEnabled set when the
    // window is transparent-capable — the runtime fixes the swapchain's alpha
    // mode here, so Ctrl+T can only work if the session starts this way. Opaque
    // frames write alpha 1 throughout, which composites exactly like an opaque
    // session. Falls back to hosted-NULL (no window binding — the runtime self-creates a
    // window at native resolution) when there is no app window.
    const bool useAppWindow = xr.hasAppWindow;

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = useAppWindow ? g_window.session_binding_chain(&vkBinding) : (const void*)&vkBinding;
    si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));
    if (useAppWindow) g_window.attach_session(xr.instance, xr.session);   // Wayland geometry feed
    LOG_INFO("Session created (%s%s)", useAppWindow ? g_window.describe().c_str() : "hosted-NULL",
             (useAppWindow && g_transparentCapable)
                 ? (g_transparentBg ? ", transparent-capable — starting TRANSPARENT"
                                    : ", transparent-capable — starting opaque (Ctrl+T toggles)")
                 : ", opaque only (no Ctrl+T)");

    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        if (XR_SUCCEEDED(xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr))
            && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (auto& m : modes) { m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR; m.next = nullptr; }
            if (XR_SUCCEEDED(xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data()))) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
                    // Only a 3D mode is a candidate for the app's default —
                    // see AppXrSession::activeRenderingMode.
                    if (modes[i].isActive == XR_TRUE && modes[i].hardwareDisplay3D == XR_TRUE)
                        xr.activeRenderingMode = (int32_t)i;
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, tiles=%ux%u, 3D=%d, active=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY,
                        xr.renderingModeTileColumns[i], xr.renderingModeTileRows[i],
                        modes[i].hardwareDisplay3D, modes[i].isActive == XR_TRUE);
                }
            }
        }
    }
    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));
    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, views.data());
    xr.maxViewCount = viewCount;
    LOG_INFO("View config: %u views reported by runtime", viewCount);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());

    int64_t selectedFmt = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0];
    for (auto f : fmts) {
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
    }

    // Worst-case-size across advertised modes (see swapchain-model.md).
    uint32_t w = views[0].recommendedImageRectWidth * 2;
    uint32_t h = views[0].recommendedImageRectHeight;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        w = xr.displayPixelWidth;
        h = xr.displayPixelHeight;
        if (xr.renderingModeCount > 0) {
            uint32_t maxW = 0, maxH = 0;
            for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] * xr.renderingModeScaleX[i] * (double)xr.displayPixelWidth);
                uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] * xr.renderingModeScaleY[i] * (double)xr.displayPixelHeight);
                if (aw > maxW) maxW = aw;
                if (ah > maxH) maxH = ah;
            }
            if (maxW > w) w = maxW;
            if (maxH > h) h = maxH;
        }
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;

    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, (long long)selectedFmt);
    return true;
}

static void PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = ssc->state;
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = xr.viewConfigType;
                xrBeginSession(xr.session, &bi);
                xr.sessionRunning = true;
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING ||
                       ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
                xr.exitRequested = true;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(xr.session, nullptr, &fs))) return false;
    return XR_SUCCEEDED(xrBeginFrame(xr.session, nullptr));
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 1000000000;
    return XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &wi));
}

static void ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
}

static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = (viewCount > 0) ? 1 : 0;
    ei.layers = layers;
    xrEndFrame(xr.session, &ei);
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.localSpace) xrDestroySpace(xr.localSpace);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
    // The app window is destroyed separately, LAST — after the Vulkan
    // instance, since the runtime's VkSurfaceKHR borrows its connection.
}

// ============================================================================
// App-owned window (handle app) — displayxr::linux_window, X11 or Wayland
// ============================================================================

static const unsigned int kDefaultWindowW = 1920;
static const unsigned int kDefaultWindowH = 1080;

// Create the window: 1920x1080 content centred on the 3D panel (a panel-sized
// GAUSS_WINDOW goes fullscreen on it, INV-1.3), with the shared header bar.
// Returns false when no window could be made -> hosted-NULL.
static bool CreateAppWindow(AppXrSession& xr) {
    if (xr.windowBackend == DxrWindowBackend::Auto) return false;

    unsigned int w = kDefaultWindowW, h = kDefaultWindowH;
    const bool panelKnown = xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0;
    const int prx = xr.displayScreenLeft, pry = xr.displayScreenTop;
    const int prw = (int)xr.displayPixelWidth, prh = (int)xr.displayPixelHeight;
    bool explicitPos = false;
    int px = 0, py = 0;

    // GAUSS_WINDOW="WxH+X+Y" override (X,Y absolute virtual-desktop px, X11
    // only); WxH alone re-centres on the panel.
    if (const char* wenv = getenv("GAUSS_WINDOW")) {
        unsigned int ow = 0, oh = 0; int ox = 0, oy = 0;
        int n = sscanf(wenv, "%ux%u+%d+%d", &ow, &oh, &ox, &oy);
        if (n >= 2 && ow > 0 && oh > 0) {
            w = ow; h = oh;
            if (n >= 4) { explicitPos = true; px = ox; py = oy; }
            LOG_INFO("GAUSS_WINDOW override: %ux%u%s", w, h, n >= 4 ? " at an absolute position" : "");
        }
    }
    if (!explicitPos && panelKnown) {
        px = prx + (prw - (int)w) / 2;
        py = pry + (prh - (int)h) / 2;
        LOG_INFO("3D panel (display_info) %dx%d at (%d,%d) — centering %ux%u window at (%d,%d)",
                 prw, prh, prx, pry, w, h, px, py);
    }

    DxrLinuxWindowDesc desc;
    desc.width = w;
    desc.height = h;
    desc.panel_left = prx;
    desc.panel_top = pry;
    desc.panel_width = (uint32_t)prw;
    desc.panel_height = (uint32_t)prh;
    desc.title = "DisplayXR Gaussian Splat Viewer";
    desc.app_id = "com.displayxr.gaussiansplat";
    desc.transparent = g_transparentCapable;   // ARGB visual / alpha surface: Ctrl+T can work
    desc.keep_above = g_transparentCapable && g_transparentBg;             // --transparent floats
    desc.transparent_background = g_transparentCapable && g_transparentBg; // no header bar while transparent
    desc.x11_header_bar = true;     // LMB on the bar moves the window (snapped)
    desc.x11_drag_button = 0;       // LMB below the bar orbits; no drag-anywhere button
    desc.wayland_drag_button = 0;
    desc.has_position = explicitPos || panelKnown;
    desc.x = px;
    desc.y = py;
    desc.fullscreen_on_wayland = panelKnown && (int)w == prw && (int)h == prh;

    if (!g_window.create(xr.windowBackend, desc)) {
        LOG_WARN("%s window creation failed — using hosted-NULL windowing",
                 DxrLinuxWindow::backend_name(xr.windowBackend));
        return false;
    }
    xr.hasAppWindow = true;
    if (g_transparentCapable && !g_window.is_transparent()) {
        // X11 screen without an ARGB visual: the handle path stays (it is what
        // weaves window-relative); only transparency is lost.
        g_transparentCapable = false;
        g_transparentBg = false;
    }
    uint32_t cw = w, ch = h;
    g_window.current_size(&cw, &ch);
    xr.xWinW = cw;
    xr.xWinH = ch;
    LOG_INFO("Created %ux%u %s window on %s", cw, ch,
             g_window.is_transparent() ? "transparent-capable" : "opaque",
             g_window.connection_description().c_str());
    return true;
}

// ============================================================================
// File-open dialog (Ctrl+O) — async zenity picker + X11 event pump
// ============================================================================

// Non-blocking zenity file-selection: fork/exec with a pipe, polled every
// frame so the XR frame loop never stalls. zenity is NOT a hard dependency —
// when absent the child exits 127 and we log a hint. Windows-parity semantics:
// pick a .spz/.ply → g_gsRenderer.loadScene.
static pid_t g_pickerPid = -1;
static int g_pickerFd = -1;
static std::string g_pickerBuf;

static void StartFilePicker() {
    if (g_pickerPid > 0) return; // dialog already open
    int fds[2];
    if (pipe(fds) != 0) { LOG_WARN("file picker: pipe() failed"); return; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); LOG_WARN("file picker: fork() failed"); return; }
    if (pid == 0) {
        // Child: zenity prints the chosen path on stdout.
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        std::string startDir = ExecutableDir(); // bundled butterfly.spz lives here
        if (!startDir.empty() && startDir.back() != '/') startDir += '/';
        std::string filenameArg = "--filename=" + startDir;
        const char* argv[] = {"zenity", "--file-selection", "--title=Open 3DGS scene",
                              filenameArg.c_str(),
                              "--file-filter=3DGS scenes | *.spz *.ply *.sog *.SPZ *.PLY *.SOG",
                              "--file-filter=All files | *", nullptr};
        execvp("zenity", const_cast<char* const*>(argv));
        _exit(127); // zenity not installed
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    g_pickerPid = pid;
    g_pickerFd = fds[0];
    g_pickerBuf.clear();
    LOG_INFO("file picker: zenity dialog opened");
}

static void PollFilePicker() {
    if (g_pickerPid <= 0) return;
    // Drain whatever the child has written so far.
    char buf[512];
    ssize_t n;
    while ((n = read(g_pickerFd, buf, sizeof(buf))) > 0) g_pickerBuf.append(buf, (size_t)n);
    int status = 0;
    pid_t r = waitpid(g_pickerPid, &status, WNOHANG);
    if (r != g_pickerPid) return; // still open
    while ((n = read(g_pickerFd, buf, sizeof(buf))) > 0) g_pickerBuf.append(buf, (size_t)n);
    close(g_pickerFd);
    g_pickerFd = -1; g_pickerPid = -1;

    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 127) { LOG_WARN("file picker: zenity not installed (apt install zenity)"); return; }
    if (code != 0) { LOG_INFO("file picker: cancelled"); return; }
    // Trim the trailing newline zenity appends.
    while (!g_pickerBuf.empty() && (g_pickerBuf.back() == '\n' || g_pickerBuf.back() == '\r'))
        g_pickerBuf.pop_back();
    if (g_pickerBuf.empty()) return;
    LOG_INFO("Loading scene: %s", g_pickerBuf.c_str());
    if (g_gsRenderer.loadScene(g_pickerBuf.c_str())) {
        LOG_INFO("Loaded %s (%u gaussians)", g_pickerBuf.c_str(), g_gsRenderer.gaussianCount());
        ApplyRigForLoadedScene();
    } else {
        LOG_WARN("file picker: load failed for %s", g_pickerBuf.c_str());
    }
}

//! Monotonic seconds. The turntable's idle gate is a duration, so a steady
//! clock is what it needs — not wall time, which can step.
static double NowSec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

//! Every input resets the turntable's idle countdown (input_handler.cpp's
//! MarkUserInput). Note WHICH events count on Windows: a key press, a wheel
//! notch, a button press, and a mouse MOVE ONLY WHILE DRAGGING — a bare
//! hover does not. Transcribed rather than broadened, because a hover-resets
//! version would mean the turntable never starts on a machine whose pointer
//! sits over the window.
static void MarkUserInput() { g_lastInputTimeSec = NowSec(); }

// Ctrl+T — Windows' transparentBgToggleRequested + kBorderlessMsg. The header
// bar hides while transparent (Windows goes borderless), the window floats
// above other apps, and the click-through region is re-applied (or dropped)
// by ClickthroughUpdate on the next frame, its only owner.
static void ToggleTransparentBackground() {
    if (!g_transparentCapable) {
        LOG_WARN("Ctrl+T ignored — this session is not transparent-capable "
                 "(no 32-bit ARGB visual, or hosted-NULL)");
        return;
    }
    g_transparentBg = !g_transparentBg;
    LOG_INFO("Transparent background: %s (Ctrl+T)", g_transparentBg ? "ON" : "OFF");
    g_window.set_transparent_background(g_transparentBg);
    if (!g_window.is_fullscreen()) g_window.set_keep_above(g_transparentBg);
}

// Handle the app window's input events (displayxr::linux_window — the same
// stream on X11 and Wayland: X11 keysyms, content-relative pixels).
//
// PARITY NOTE. windows/main.cpp routes every message through displayxr-common's
// UpdateInputState (input_handler.cpp), which is <windows.h>-only and cannot be
// linked here. The bindings below are therefore transcribed from it rather than
// invented, so a key or a drag means the same thing on both legs:
//
//   LMB drag (client area) orbit the scene          input_handler.cpp:98/72
//   LMB drag (header bar)   move the window          WM_NCHITTEST -> HTCAPTION
//   wheel                   zoom, x1.1 per notch     input_handler.cpp:130
//   shift+wheel, +/-        3D strength (ipd)        input_handler.cpp:136, :210
//   V                       cycle rendering mode     input_handler.cpp:252
//   0-8                     jump to that mode        input_handler.cpp:268
//   SPACE                   reset the view           input_handler.cpp:230
//   M                       turntable on/off         input_handler.cpp:201
//   F11                     fullscreen               input_handler.cpp:237
//   ESC / Q                 quit                     windows/main.cpp:1732
//   Ctrl+O                  open a scene             windows/main.cpp:1743
//   Ctrl+T                  transparent background   input_handler.cpp (Ctrl+T)
//
// The header bar, the window drag and F11 are the helper's; the orbit drag is
// incremental and accumulates every Motion event.
static void HandleWindowEvent(AppXrSession& xr, const DxrWindowEvent& ev) {
    switch (ev.type) {
    case DxrWindowEvent::Type::KeyDown: {
        MarkUserInput();
        KeySym sym = (KeySym)ev.keysym;
        const bool ctrl = (ev.mods & DxrModCtrl) != 0;
            switch (sym) {
            case XK_o: case XK_O:
                // Ctrl+O = open a scene (uniform across demos + platforms, #74).
                // Strict: Ctrl must be held (bare O does nothing).
                if (ctrl) StartFilePicker();
                break;
            case XK_Escape:
            case XK_q: case XK_Q:
                // Graceful, like the Windows WM_CLOSE path: ask the runtime to
                // end the session and let the state machine drain, rather than
                // dropping the loop from under an in-flight frame. The hard
                // stop is the fallback when there is no running session.
                LOG_INFO("ESC/Q — exiting");
                if (xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
                    xrRequestExitSession(xr.session);
                } else {
                    g_running = false;
                }
                break;
            case XK_F11:
                break; // toggled inside the window helper's pump
            case XK_v: case XK_V:
                // Cycle to the next rendering mode. The main loop turns this
                // into a ModeSwitch request and the sequencer fires
                // xrRequestDisplayRenderingModeDXR on the right frame.
                g_cycleModeRequested = true;
                break;
            case XK_t: case XK_T:
                // Ctrl+T = transparent background. Strict: bare T does nothing.
                if (ctrl) ToggleTransparentBackground();
                break;
            case XK_m: case XK_M:
                g_animateEnabled = !g_animateEnabled;
                LOG_INFO("M: turntable %s", g_animateEnabled ? "ON" : "OFF");
                break;
            case XK_space:
                // Reset the view: orbit back to rest, zoom and 3D strength back
                // to their defaults. Windows' resetViewRequested does the same.
                g_dispOrbitYaw = 0.0f; g_dispOrbitPitch = 0.0f;
                g_camOrbitYaw = 0.0f;  g_camOrbitPitch = 0.0f;
                g_scaleFactor = 1.0f;
                g_steadyIpd = 1.0f;
                MarkUserInput();
                LOG_INFO("SPACE: view reset");
                break;
            case XK_minus: case XK_KP_Subtract: {
                float v = g_steadyIpd - 0.1f;
                if (v < 0.1f) v = 0.1f;
                g_steadyIpd = v;
                LOG_INFO("-: 3D strength %.2f", g_steadyIpd);
                break;
            }
            case XK_equal: case XK_plus: case XK_KP_Add: {
                float v = g_steadyIpd + 0.1f;
                if (v > 1.0f) v = 1.0f;
                g_steadyIpd = v;
                LOG_INFO("+: 3D strength %.2f", g_steadyIpd);
                break;
            }
            case XK_0: case XK_1: case XK_2: case XK_3: case XK_4:
            case XK_5: case XK_6: case XK_7: case XK_8: {
                const int32_t want = (int32_t)(sym - XK_0);
                // Mode 0 is always legal (2D); the rest only if enumerated.
                if (want == 0 || (uint32_t)want < xr.renderingModeCount) {
                    g_absoluteModeRequested = want;
                } else {
                    LOG_WARN("mode %d not advertised by this display (%u modes)",
                             want, xr.renderingModeCount);
                }
                break;
            }
            default: break;
            }
        break;
    }

    case DxrWindowEvent::Type::Scroll: {
        MarkUserInput();
        // x1.1 per notch, multiplicative, matching WM_MOUSEWHEEL's `factor`.
        const int n = ev.scroll_steps;
        for (int i = 0; i < (n > 0 ? n : -n); i++) {
            const float factor = (n > 0) ? 1.1f : (1.0f / 1.1f);
            if ((ev.mods & DxrModShift) != 0) {
                // Shift+wheel drives the 3D-effect strength, in lockstep with
                // the +/- keys (input_handler.cpp:136).
                float v = g_steadyIpd * factor;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                g_steadyIpd = v;
            } else {
                float z = g_scaleFactor * factor;
                if (z < 0.1f) z = 0.1f;
                if (z > 10.0f) z = 10.0f;
                g_scaleFactor = z;
            }
        }
        break;
    }

    case DxrWindowEvent::Type::ButtonDown:
        MarkUserInput();
        if (ev.button != 1) break;
        // Scene orbit (a press on the header bar never reaches here — it is
        // the window drag). Both rigs: the camera rig turns the SCENE about
        // the pivot, the display rig turns the display about the subject, but
        // a left-drag rotates the thing you are looking at on either.
        g_camDragging = true;
        g_dispDragging = true;
        g_camDragLastX = ev.x;
        g_camDragLastY = ev.y;
        g_dispDragLastX = ev.x;
        g_dispDragLastY = ev.y;
        break;

    case DxrWindowEvent::Type::ButtonUp:
        if (ev.button != 1) break;
        g_camDragging = false;
        g_dispDragging = false;
        break;

    case DxrWindowEvent::Type::Motion:
            if (g_camDragging && g_cameraRigActive && g_camRig.valid) {
                MarkUserInput();
                // Gain is per canvas FRACTION (GsOrbitFromDrag's contract), so a
                // full-width drag reaches the comfort cone and no further,
                // whatever the window size. Turntable sign and the cone are the
                // shared helper's — not re-derived here.
                const float vw = (xr.xWinW > 1u) ? (float)xr.xWinW : 1.0f;
                const float vh = (xr.xWinH > 1u) ? (float)xr.xWinH : 1.0f;
                const float dx = (float)(ev.x - g_camDragLastX) / vw;
                // X11 y grows DOWN; macOS deltaY grows up. Negate so a drag in
                // the same physical direction orbits the same way on both.
                const float dy = -(float)(ev.y - g_camDragLastY) / vh;
                g_camDragLastX = ev.x;
                g_camDragLastY = ev.y;
                const GsOrbit d = GsOrbitFromDrag(dx, dy);
                g_camOrbitYaw += d.yaw;
                g_camOrbitPitch += d.pitch;
                g_camRig.ClampOrbit(g_camOrbitYaw, g_camOrbitPitch);
            } else if (g_dispDragging) {
                MarkUserInput();
                // Display rig — transcribed verbatim from input_handler.cpp:72-89,
                // including the things that look arbitrary:
                //   * 0.005 rad PER CLIENT PIXEL on both axes, with no window-size
                //     or DPI normalisation (unlike the camera rig, whose gain is
                //     per canvas fraction because its cone is absolute).
                //   * BOTH signs negative — camera-orbit convention, the scene
                //     appears to move opposite the finger. Note this is the
                //     opposite sense from the camera rig's turntable above, which
                //     is deliberate on Windows too.
                //   * pitch clamped to +-1.4 rad (80.21 deg), yaw unclamped and
                //     never wrapped.
                // Incremental, with the anchor rewritten every motion event.
                const int dx = ev.x - g_dispDragLastX;
                const int dy = ev.y - g_dispDragLastY;
                g_dispDragLastX = ev.x;
                g_dispDragLastY = ev.y;
                g_dispOrbitYaw -= (float)dx * 0.005f;
                g_dispOrbitPitch -= (float)dy * 0.005f;
                if (g_dispOrbitPitch > 1.4f) g_dispOrbitPitch = 1.4f;
                if (g_dispOrbitPitch < -1.4f) g_dispOrbitPitch = -1.4f;
            }
            break;

    case DxrWindowEvent::Type::Resize:
        // The CONTENT size (the bound window/surface, header bar excluded).
        xr.xWinW = ev.width;
        xr.xWinH = ev.height;
        g_windowW = xr.xWinW;
        g_windowH = xr.xWinH;
        g_weaveSnap.set_extent(ev.width, ev.height);
        break;

    default: break;
    }
}

//! Drain the window once per frame (both backends).
static void PumpWindow(AppXrSession& xr) {
    if (!xr.hasAppWindow) return;
    bool running = true;
    g_window.pump_events([&xr](const DxrWindowEvent& ev) { HandleWindowEvent(xr, ev); }, &running);
    if (!running) {
        LOG_INFO("Window closed — exiting");
        g_running = false;
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== DisplayXR Gaussian Splat Viewer (Vulkan, Linux) ===");

    // Command line. The shared grammar is displayxr-common's launch-args parser
    // — the same one the Windows and macOS arms use, so `--vh=`, `--src=` and a
    // positional path mean exactly what they mean there rather than growing a
    // third per-platform implementation. The rig flags are a second pass over
    // the same argv (GsParseRigFlags leaves tokens it does not own alone, and
    // the launch parser only WARNS about tokens it does not know), so neither
    // parser has to learn the other's vocabulary.
    std::string cliScenePath;
    // Window platform: --platform=x11|wayland|auto (default auto, a capability
    // probe). Taken out of the argument list before the launch parser, which
    // would otherwise warn about it or take `--platform x11`'s value for a
    // scene path.
    DxrWindowBackend requestedBackend = DxrWindowBackend::Auto;
    {
        std::vector<std::string> args;
        for (int i = 1; i < argc; i++)
            if (argv[i]) args.emplace_back(argv[i]);
        {
            std::string err;
            if (!DxrLinuxWindow::take_platform_args(&args, &requestedBackend, &err)) {
                LOG_ERROR("%s", err.c_str());
                return 1;
            }
        }
        g_launch = dxr::ParseLaunchArgs(args);
        // The shared parser meets this viewer's own flags and says so; that is
        // not a problem worth a WARN, because they ARE handled — just by the
        // second pass below. See GsIsOwnFlagWarning.
        for (const std::string& w : g_launch.warnings)
            if (!GsIsOwnFlagWarning(w)) LOG_WARN("launch: %s", w.c_str());
        for (const std::string& e : g_launch.errors)   LOG_ERROR("launch: %s", e.c_str());
        // A local `--src=` and the legacy positional both name a scene; the
        // explicit flag wins. A URL src is a Windows-only capability (no
        // download cache on this arm) — name it rather than silently ignoring it.
        if (g_launch.srcKind == dxr::LaunchSrcKind::LocalPath && !g_launch.src.empty())
            cliScenePath = g_launch.src;
        else if (!g_launch.positionalPath.empty())
            cliScenePath = g_launch.positionalPath;
        // --transparent: start in transparent mode (Windows parity:
        // g_transparentBg = g_launch.transparent).
        if (g_launch.transparent) {
            g_transparentBg = true;
            LOG_INFO("launch: --transparent");
        }
        if (g_launch.srcKind == dxr::LaunchSrcKind::Url)
            LOG_WARN("launch: --src URL is not fetched on Linux; pass a local path");

        std::vector<std::string> rigWarn;
        GsParseRigFlags(argc, argv, g_rigFlags, &rigWarn);
        for (const std::string& w : rigWarn) LOG_WARN("rig: %s", w.c_str());
    }

    AppXrSession xr = {};
    // Pre-session SIM_DISPLAY_OUTPUT pin. This is the only way an agent can
    // select a rendering mode headlessly (V and the number keys need a human),
    // so it takes PRECEDENCE over the adoption below rather than being a mere
    // fallback. sim_display's enumerated mode table is
    // [0]=2D [1]=Anaglyph [2]=Cropped SBS [3]=Squeezed SBS [4]=Quad; `quad`,
    // `2d`/`passthrough` and `squeezed` were all missing from this map, so the
    // 4-view and 1-view atlases — the two counts that most need exercising —
    // could not be selected at all: the app stomped the display back to
    // Anaglyph a frame after it booted into them.
    int32_t envPinnedMode = -1;
    { const char* mode = getenv("SIM_DISPLAY_OUTPUT");
      if (mode) {
          if (strcmp(mode, "2d") == 0 ||
              strcmp(mode, "passthrough") == 0) envPinnedMode = 0;
          else if (strcmp(mode, "anaglyph") == 0) envPinnedMode = 1;
          else if (strcmp(mode, "sbs") == 0) envPinnedMode = 2;
          else if (strcmp(mode, "squeezed") == 0 ||
                   strcmp(mode, "squeezed_sbs") == 0) envPinnedMode = 3;
          else if (strcmp(mode, "quad") == 0) envPinnedMode = 4;
          // "blend" is a sim_display DP pipeline, not an enumerated rendering
          // mode; left on the historical 3 as there is no index that names it.
          else if (strcmp(mode, "blend") == 0) envPinnedMode = 3;
          else envPinnedMode = 1; // unrecognised -> the first 3D mode
          xr.currentRenderingMode = (uint32_t)envPinnedMode;
      } }

    // `--mode=` wins over the env var, and — like it — over ADOPTION: it is the
    // most explicit statement of intent there is, a per-launch request typed by
    // whoever started the process, and it is what makes a rest view
    // reproducible from a script (mode 0 is the 2D passthrough, so a capture of
    // it is one view rather than a composite the DP has already mixed).
    //
    // It therefore has to set `envPinnedMode`, not just currentRenderingMode:
    // that variable is what gates the adoption below, so writing only the mode
    // would let the runtime's active mode overwrite the flag a few lines later.
    if (g_rigFlags.hasMode) {
        envPinnedMode = g_rigFlags.mode;
        xr.currentRenderingMode = (uint32_t)g_rigFlags.mode;
        LOG_INFO("Initial rendering mode from --mode=%d", g_rigFlags.mode);
    }

    if (!InitializeOpenXR(xr, requestedBackend)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // Handle app: own a window on the 3D panel — X11 or Wayland, whichever the
    // probe chose (display_info queried in InitializeOpenXR gives the panel
    // rect). Falls back to hosted-NULL when no window system answers (also
    // the CI-safe path — CI never runs this).
    CreateAppWindow(xr);
    if (!xr.hasAppWindow) {
        // hosted-NULL: the runtime's own window is opaque — no Ctrl+T.
        g_transparentCapable = false;
        g_transparentBg = false;
    }

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    // Resolve the drag-time phase snap. Optional by construction: the entry
    // point is young (runtime #1588) and a display processor with no lattice
    // snap answers with the target unchanged anyway, so an absent pointer is
    // logged once and the drag is merely unsnapped. It is resolved AFTER
    // xrCreateSession because it is a per-session query.
    if (xr.hasAppWindow) {
        g_weaveSnap.attach(xr.hasWeaveExt ? xr.instance : XR_NULL_HANDLE, xr.session, xr.xWinW, xr.xWinH);
        g_window.set_snap_provider(&DxrWeaveSnap::callback, &g_weaveSnap);
        LOG_INFO("xrWeaveSnapWindowRectDXR: %s — a window drag %s",
                 g_weaveSnap.available() ? "RESOLVED" : "unavailable on this runtime",
                 g_weaveSnap.available() ? "will be phase-snapped by the display processor"
                                         : "lands on the raw pointer position (identity snap)");
    }

    // Settle the STARTUP rendering mode here. Same precedence as the macOS leg:
    // an explicit SIM_DISPLAY_OUTPUT / --mode pin wins, else the runtime's
    // active 3D mode, else the app default (mode 1). Everything downstream —
    // tile grid, view count, view scale — keys off xr.currentRenderingMode, and
    // CreateSwapchains below sizes the atlas worst-case across all modes, so a
    // 4-view Quad fits.
    //
    // Unlike before, this is the STARTUP mode rather than the only one: 'V' and
    // '0'-'8' now drive xrRequestDisplayRenderingModeDXR through the shared
    // ModeSwitch sequencer (see the render loop). The old per-frame re-assert
    // that made an external mode change impossible is still gone — the app
    // issues a request only when the user asks for one.
    if (envPinnedMode >= 0) {
        if (xr.renderingModeCount > 0 && (uint32_t)envPinnedMode >= xr.renderingModeCount) {
            LOG_WARN("%s selects mode %d but the display has only %u mode(s) "
                     "— falling back to mode 0",
                     g_rigFlags.hasMode ? "--mode" : "SIM_DISPLAY_OUTPUT",
                     envPinnedMode, xr.renderingModeCount);
            xr.currentRenderingMode = 0;
        } else {
            xr.currentRenderingMode = (uint32_t)envPinnedMode;
        }
    } else if (xr.activeRenderingMode >= 0) {
        xr.currentRenderingMode = (uint32_t)xr.activeRenderingMode;
    }
    LOG_INFO("Startup rendering mode: %u (%s)", xr.currentRenderingMode,
             g_rigFlags.hasMode ? "--mode"
                                : envPinnedMode >= 0 ? "SIM_DISPLAY_OUTPUT"
                                : (xr.activeRenderingMode >= 0 ? "adopted from runtime"
                                                               : "app default"));

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    { uint32_t count = xr.swapchain.imageCount;
      swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    if (!g_gsRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue, queueFamilyIndex,
                           xr.swapchain.width, xr.swapchain.height))
        LOG_WARN("3DGS renderer init failed");

    // Tile basis: the app window when we own one (window × scaleXY, #729-style);
    // else the full panel (hosted-NULL renders display-sized).
    if (xr.xWinW > 0 && xr.xWinH > 0) {
        g_windowW = xr.xWinW;
        g_windowH = xr.xWinH;
    } else {
        if (xr.displayPixelWidth > 0) g_windowW = xr.displayPixelWidth;
        if (xr.displayPixelHeight > 0) g_windowH = xr.displayPixelHeight;
    }

    // Auto-load bundled butterfly.spz (copied next to the exe by CMake).
    { std::string scene = ExecutableDir() + "/butterfly.spz";
      if (!cliScenePath.empty()) scene = cliScenePath;
      if (g_gsRenderer.loadScene(scene.c_str())) {
          LOG_INFO("Loaded scene: %s (%u gaussians)", scene.c_str(), g_gsRenderer.gaussianCount());
          ApplyRigForLoadedScene();
      } else {
#if defined(GS_RENDERER_COMPUTE)
          // Debug-scene fallback exists only on the compute renderer (GsRenderer).
          LOG_WARN("No scene at %s — loading debug splat", scene.c_str());
          g_gsRenderer.loadDebugScene(0.0f, 0.0f, 0.0f, 0.1f);
#else
          // Graphics renderer (GsAdrenoRenderer, the Linux default) has no
          // loadDebugScene — it renders from a loaded .spz/.ply only.
          LOG_ERROR("No scene at %s and the graphics renderer has no debug-scene "
                    "fallback — pass a .spz/.ply path or bundle butterfly.spz", scene.c_str());
#endif
      } }

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("  mouse: drag = orbit | wheel = zoom | shift+wheel = 3D strength");
    LOG_INFO("  keys:  V = cycle 2D/3D | 0-8 = pick mode | M = turntable | SPACE = reset");
    LOG_INFO("         +/- = 3D strength | F11 = fullscreen | Ctrl+O = open | ESC/Q = quit");

    auto lastTime = std::chrono::high_resolution_clock::now();
    // Seed the idle clock so the turntable starts 10 s after launch rather
    // than on frame 1 — the same shape as Windows, where lastInputTimeSec is
    // stamped by the startup pose application.
    MarkUserInput();

    while (g_running && !xr.exitRequested) {
        PollEvents(xr);
        PumpWindow(xr);    // input; the helper runs the header bar, the drag and F11
        PollFilePicker();  // async zenity result → loadScene
        RefitForContent(); // the content aspect changed -> re-derive the framed vHeight
        // This frame's base virtual display height: the scene's fit (the
        // render path divides it by g_scaleFactor for the wheel zoom).
        const float virtualDisplayHeight = g_fitValid ? g_fitVHeight : kFallbackVirtualDisplayHeightM;

        // NOTE: this used to re-assert the app's own rendering mode HERE, every
        // frame, for as long as the session ran — so nothing outside the process
        // could ever change the mode and SIM_DISPLAY_OUTPUT=quad was undone a
        // frame after it took effect. The mode is now settled once after
        // xrCreateSession (adopted from the runtime, or pinned by
        // SIM_DISPLAY_OUTPUT) and this leg issues no request at all.

        if (!xr.sessionRunning) {
            struct timespec ts = {0, 10 * 1000 * 1000}; nanosleep(&ts, nullptr);
            continue;
        }

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        // Never on the camera rig: a photo-lifted cloud has no support more than
        // ~15 deg off the capture axis, so a turntable shows floaters rather
        // than parallax — and the rest view has to stay the photograph.
        //
        // Also never WHILE THE USER IS DRAGGING, and only while the 'M' toggle
        // is on. An auto-advancing yaw fights a drag: the scene would keep
        // creeping out from under the pointer. Windows solves the same problem
        // with an idle timer on InputState.lastInputTimeSec; the toggle is the
        // same contract in a form this leg can honour without a HUD.
        // input_handler.cpp:460-470: the turntable only runs after the user has
        // been idle for more than 10 s, and every input resets the clock. That
        // is what lets an auto-orbit coexist with a drag instead of fighting
        // it. Rate and sign are the shared handler's: +18 deg/s, which is the
        // OPPOSITE sense from a rightward display-rig drag (also true on
        // Windows).
        g_animationActive = false;
        if (!g_cameraRigActive && g_animateEnabled && g_lastInputTimeSec > 0.0) {
            const double idleFor = NowSec() - g_lastInputTimeSec;
            g_animationActive = (idleFor > 10.0);
            if (g_animationActive)
                g_dispOrbitYaw += (6.2831853f / 20.0f) * dt;   // one revolution / 20 s
        }

        // ── 2D/3D mode switching ('V' / '0'-'8') ────────────────────────────
        // Turn a key press into a ModeSwitch request, then advance the ramp.
        // The sequencer owns the asymmetry (3D->2D flattens BEFORE switching,
        // 2D->3D switches first and eases up) and hands back the ipdFactor to
        // submit this frame plus the frame on which to fire the runtime call.
        if (xr.renderingModeCount > 0 && xr.pfnRequestDisplayRenderingModeEXT != nullptr) {
            const uint32_t cur = xr.currentRenderingMode < xr.renderingModeCount
                                     ? xr.currentRenderingMode : 0u;
            // Cycle first, then let an absolute request OVERRIDE it — the
            // order in xr_session_common.cpp:394-451. It matters when both
            // land in one frame.
            int32_t want = -1;
            if (g_cycleModeRequested) want = (int32_t)((cur + 1u) % xr.renderingModeCount);
            if (g_absoluteModeRequested >= 0) want = g_absoluteModeRequested;
            g_cycleModeRequested = false;
            g_absoluteModeRequested = -1;

            if (want >= 0 && (uint32_t)want < xr.renderingModeCount) {
                const uint32_t targetVC = xr.renderingModeViewCounts[want];
                const uint32_t currentVC = xr.renderingModeViewCounts[cur];
                LOG_INFO("mode switch requested: %u (%u view%s) -> %d (%u view%s)",
                         cur, currentVC, currentVC == 1 ? "" : "s",
                         want, targetVC, targetVC == 1 ? "" : "s");
                // From the ramp's own value while one is in flight, else the
                // steady value the frame is actually rendering with.
                g_modeSwitch.request((uint32_t)want, targetVC, cur, currentVC,
                                     g_modeSwitch.active() ? g_modeSwitch.ipd() : g_steadyIpd,
                                     g_steadyIpd);
            }

            // The ramp's output ONLY while a switch is in flight. Idle,
            // ModeSwitch::update() reports its last ramp value — 0 before the
            // first switch — and taking it then gave every frame ipdFactor 0:
            // both eyes at one position, zero parallax, a woven image that
            // reads as flat 2D. Idle renders the steady strength (which also
            // tracks +/- at once), as modelviewer's UpdateModeSwitch does.
            float rampIpd = g_steadyIpd;
            bool fire = false;
            uint32_t fireMode = cur;
            if (g_modeSwitch.active()) {
                g_modeSwitch.update(dt, &rampIpd, &fire, &fireMode);
            }
            g_ipdFactor = rampIpd;
            if (fire && fireMode != xr.currentRenderingMode) {
                const XrResult mr = xr.pfnRequestDisplayRenderingModeEXT(xr.session, fireMode);
                if (XR_SUCCEEDED(mr)) {
                    xr.currentRenderingMode = fireMode;
                    LOG_INFO("rendering mode -> %u", fireMode);
                } else {
                    LOG_WARN("xrRequestDisplayRenderingModeDXR(%u) failed: %d", fireMode, (int)mr);
                }
            }
        } else {
            // No mode-switch entry point (or no enumerated modes): the ramp has
            // nothing to sequence, so the strength knob applies directly.
            g_ipdFactor = g_steadyIpd;
        }

        XrFrameState fs;
        if (!BeginFrame(xr, fs)) continue;

        std::vector<XrCompositionLayerProjectionView> projectionViews;
        if (fs.shouldRender) {
            // On the CAMERA rig the declared pose is the capture camera's REST
            // pose and never the orbit: a drag turns the scene (see
            // PumpXEvents), so feeding the turntable yaw here as well would
            // double-apply the rotation.
            const bool camRig = g_cameraRigActive && g_camRig.valid;
            // Does the ACTIVE mode render one view or a pair? Known before the
            // locate (it is a property of the mode, not of the result), and the
            // camera rig's pose depends on it: a pair straddles the head centre,
            // a single view IS the photograph.
            const bool camMonoMode =
                (xr.renderingModeCount > 0 && xr.currentRenderingMode < xr.renderingModeCount)
                    ? !xr.renderingModeDisplay3D[xr.currentRenderingMode]
                    : false;

            XrPosef cameraPose;
            if (camRig) {
                // The HEAD CENTRE, half a baseline right of the block's `rest`
                // (which is the LEFT capture camera) — so at rest the two
                // rendered views land on the two capture cameras; in a mono mode
                // it stays on `rest`. See GsCameraRig::RigPose.
                float rigPos[3], rigRot[4];
                g_camRig.RigPose(camMonoMode, rigPos, rigRot);
                cameraPose.orientation = {rigRot[0], rigRot[1], rigRot[2], rigRot[3]};
                // Move the declared camera by MINUS the rest eye offset, in the
                // camera's own frame, so the rest eye lands back on the capture
                // camera. See g_camRigRestEyeXY.
                if (g_camRigRestSampled) {
                    float ox, oy, oz;
                    quat_rotate_vec3(cameraPose.orientation,
                                     -g_camRigRestEyeXY[0], -g_camRigRestEyeXY[1], 0.0f,
                                     &ox, &oy, &oz);
                    rigPos[0] += ox; rigPos[1] += oy; rigPos[2] += oz;
                }
                cameraPose.position = {rigPos[0], rigPos[1], rigPos[2]};
            } else {
                // Display rig: the turntable and the left-drag orbit are the
                // same two angles, so a drag simply moves where the turntable
                // resumes from rather than fighting it.
                quat_from_yaw_pitch(g_dispOrbitYaw, g_dispOrbitPitch, &cameraPose.orientation);
                // The rig orbits the FRAMED centre (the fit), as on macOS /
                // Windows — not the world origin.
                if (g_fitValid) {
                    cameraPose.position = {g_fitCenter[0], g_fitCenter[1], g_fitCenter[2]};
                } else {
                    cameraPose.position = {0, 0, 0};
                }
            }

            const bool useRig = xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;

            // Canvas aspect the runtime will derive the horizontal FOV from.
            const float canvasAspect =
                (g_windowH > 0) ? ((float)g_windowW / (float)g_windowH) : 1.0f;

            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = fs.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            XrViewState viewState = {XR_TYPE_VIEW_STATE};

            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
            XrCameraRigDXR  camRigDesc = {XR_TYPE_CAMERA_RIG_DXR};
            // The raw channel is the only place the UNSCALED eye spread and the
            // tracker-lock flag appear, and the camera rig needs both (the
            // absolute ipdFactor is a multiplier on that spread, and the rest
            // anchor may only be sampled on an untracked frame). This leg chained
            // nothing on XrViewState::next before; the struct is inert when the
            // runtime does not fill it.
            XrViewDisplayRawDXR viewRigRaw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
            if (useRig && camRig) {
                // DECLARE, never compute. The photo's intrinsics become an
                // XR_DXR_view_rig CAMERA descriptor and the runtime returns
                // render-ready XrView{pose, fov}; the app does no Kooima.
                //
                // ipdFactor / parallaxFactor on this rig are ABSOLUTE scales, not
                // the display rig's [0,1] factors, and they stay absolute: the eye
                // separation is the capture baseline in world metres, never
                // normalised against the convergence distance. IpdScale() divides
                // the baseline by the MEASURED eye spread because the runtime
                // scales the tracked eyes; metersToVirtual is 1 because a lifted
                // scene is already metric.
                camRigDesc.pose = cameraPose;
                // The ramp is a [0,1] STRENGTH multiplier here, not the
                // absolute scale: IpdScale() already carries the capture
                // baseline, and 'V' must still be able to flatten it to zero.
                camRigDesc.ipdFactor = g_camRig.IpdScale(g_camRigMeasuredIpdM) * g_ipdFactor;
                camRigDesc.parallaxFactor = 1.0f;
                camRigDesc.convergenceDiopters = g_camRig.ConvergenceDiopters();
                camRigDesc.verticalFov = g_camRig.VerticalFovRad(canvasAspect);
                camRigDesc.metersToVirtual = 1.0f;
                locateInfo.next = &camRigDesc;
                viewState.next = &viewRigRaw;
            } else if (useRig) {
                displayRig.pose = cameraPose;
                // rigVH = virtualDisplayHeight / scaleFactor — the same
                // arithmetic the Windows leg uses (windows/main.cpp:2339), so
                // a wheel notch means the same thing on both: a bigger
                // scaleFactor is a SMALLER virtual display, i.e. a bigger
                // subject.
                displayRig.virtualDisplayHeight = virtualDisplayHeight / g_scaleFactor;
                displayRig.ipdFactor = g_ipdFactor;
                displayRig.parallaxFactor = 1.0f;
                displayRig.perspectiveFactor = 1.0f;
                locateInfo.next = &displayRig;
            } else if (camRig) {
                // No XR_DXR_view_rig on this runtime: the rig cannot be declared
                // at all, so the frame falls through to the harness's plain
                // located-fov path below. Say so once rather than silently
                // showing a scene framed by somebody else's frustum.
                static bool s_warnedNoRigExt = false;
                if (!s_warnedNoRigExt) {
                    s_warnedNoRigExt = true;
                    LOG_WARN("Camera rig: runtime does not advertise %s — rendering the "
                             "located frustum without the photo's intrinsics",
                             XR_DXR_VIEW_RIG_EXTENSION_NAME);
                }
            }

            // XR_DXR_depth_budget (windows/main.cpp #100): the runtime's
            // advisory rear depth budget, appended to the SAME viewState chain
            // after view_rig so both survive. Its `type` stays 0 unless a
            // runtime that knows the struct fills it — that is how "filled" is
            // told from "not". Only the display rig consults it (the camera
            // rig clips at its pivot, as on Windows). No content bounds / mask
            // are chained on this leg, so the runtime measures its fallback
            // region (the whole canvas).
            XrRearDepthBudgetDXR depthBudget{};
            if (xr.hasDepthBudgetExt) dxr::ChainRearDepthBudget(viewState, depthBudget);

            uint32_t runtimeViewCount = xr.maxViewCount > 8 ? 8 : (xr.maxViewCount ? xr.maxViewCount : 2);
            XrView views[8] = {};
            for (uint32_t v = 0; v < runtimeViewCount; v++) views[v].type = XR_TYPE_VIEW;

            XrResult locRes = xrLocateViews(xr.session, &locateInfo, &viewState,
                runtimeViewCount, &runtimeViewCount, views);

            if (XR_SUCCEEDED(locRes) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                float camShiftU = 0.0f, camShiftV = 0.0f;
                if (camRig) {
                    // The raw channel is the only place the UNSCALED eye spread
                    // appears, and it is what the camera rig's absolute
                    // ipdFactor multiplies. Measured once per locate (it tracks
                    // a real face), used on the next — one frame of lag on a
                    // number that barely moves, versus a nominal that is simply
                    // wrong (the sim display reports 60 mm, not 63).
                    if (viewRigRaw.eyeCountOutput >= 2) {
                        const float dx = viewRigRaw.rawEyes[1].x - viewRigRaw.rawEyes[0].x;
                        const float dy = viewRigRaw.rawEyes[1].y - viewRigRaw.rawEyes[0].y;
                        const float dz = viewRigRaw.rawEyes[1].z - viewRigRaw.rawEyes[0].z;
                        const float sep = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (sep > 1.0e-4f) g_camRigMeasuredIpdM = sep;
                    }

                    // Sample the rest anchor once, on an UNTRACKED frame: the
                    // eye centroid the panel reports with no lock, and the
                    // frustum centre the rig produced from it. Both are then
                    // cancelled — here in the fov, and in cameraPose above.
                    // (Untracked only — sampling a tracked frame would bake a
                    // real head position in as if it were the rest.)
                    if (!g_camRigRestSampled && useRig &&
                        viewRigRaw.isTracking != XR_TRUE) {
                        float sx = 0.0f, sy = 0.0f;
                        uint32_t n = 0;
                        for (uint32_t v = 0; v < viewRigRaw.eyeCountOutput && v < 8; v++) {
                            sx += viewRigRaw.rawEyes[v].x;
                            sy += viewRigRaw.rawEyes[v].y;
                            n++;
                        }
                        if (n > 0) {
                            g_camRigRestEyeXY[0] = sx / (float)n;
                            g_camRigRestEyeXY[1] = sy / (float)n;
                        }
                        const XrFovf& f0 = views[0].fov;
                        g_camRigRestTan[0] = 0.5f * (tanf(f0.angleRight) + tanf(f0.angleLeft));
                        g_camRigRestTan[1] = 0.5f * (tanf(f0.angleUp) + tanf(f0.angleDown));
                        g_camRigRestSampled = true;
                        LOG_INFO("Camera rig rest anchor: eye=(%.4f, %.4f) m, "
                                 "frustum centre tan=(%.5f, %.5f) — cancelled so the "
                                 "rest view is the capture camera",
                                 g_camRigRestEyeXY[0], g_camRigRestEyeXY[1],
                                 g_camRigRestTan[0], g_camRigRestTan[1]);
                    }

                    // The ONE intrinsic XrCameraRigDXR cannot carry: an
                    // off-centre principal point. Applied on top of the
                    // render-ready fov rather than re-derived, and exactly zero
                    // for the centred principal point every asset has today.
                    g_camRig.PrincipalShiftTan(camShiftU, camShiftV);
                    camShiftU -= g_camRigRestTan[0];
                    camShiftV -= g_camRigRestTan[1];

                    // One-shot: what we DECLARED vs what came back. The camera
                    // rig lives or dies on the returned frustum matching the
                    // photo's, and a silent aspect disagreement is exactly the
                    // "wrong zoom" this rig exists to prevent.
                    static bool s_fovLogged = false;
                    if (!s_fovLogged) {
                        s_fovLogged = true;
                        const XrFovf& f = views[0].fov;
                        LOG_INFO("Camera rig fov: declared vFOV=%.3fdeg (half-tan %.5f, "
                                 "photo half-tan h=%.5f v=%.5f) -> returned "
                                 "L=%.3f R=%.3f U=%.3f D=%.3f deg canvasAspect=%.4f",
                                 g_camRig.VerticalFovRad(canvasAspect) * 57.2957795f,
                                 tanf(0.5f * g_camRig.VerticalFovRad(canvasAspect)),
                                 g_camRig.TanHalfPhotoW(), g_camRig.TanHalfPhotoH(),
                                 f.angleLeft * 57.2957795f, f.angleRight * 57.2957795f,
                                 f.angleUp * 57.2957795f, f.angleDown * 57.2957795f,
                                 canvasAspect);
                    }
                }

                // The drag's rotation is a MODEL transform folded into the view
                // matrix below (this renderer has no separate model stage).
                float camSceneOrbit[16];
                const bool camOrbiting =
                    camRig && (g_camOrbitYaw != 0.0f || g_camOrbitPitch != 0.0f);
                if (camOrbiting)
                    g_camRig.SceneOrbitMatrix(g_camOrbitYaw, g_camOrbitPitch, camSceneOrbit);

                uint32_t mode = xr.currentRenderingMode < xr.renderingModeCount ? xr.currentRenderingMode : 0;
                uint32_t modeViewCount = xr.renderingModeCount > 0 ? xr.renderingModeViewCounts[mode] : 2u;
                if (modeViewCount < 1) modeViewCount = 1;
                if (modeViewCount > runtimeViewCount) modeViewCount = runtimeViewCount;
                bool display3D = xr.renderingModeCount > 0 ? xr.renderingModeDisplay3D[mode] : true;
                bool monoMode = !display3D;
                uint32_t tileColumns = xr.renderingModeCount > 0 && xr.renderingModeTileColumns[mode] > 0
                    ? xr.renderingModeTileColumns[mode] : (monoMode ? 1u : 2u);
                uint32_t tileRows = xr.renderingModeCount > 0 && xr.renderingModeTileRows[mode] > 0
                    ? xr.renderingModeTileRows[mode] : 1u;
                int eyeCount = monoMode ? 1 : (int)modeViewCount;

                // runtime#1486 INV-3.1: eyeCount is what gets RENDERED (the layer
                // itself carries every located view — ADR-041, below), so it is the
                // single place to reconcile the counts that can disagree: the active
                // mode's view count, the count xrLocateViews returned (modeViewCount is
                // already clamped to it above), and the number of tiles the swapchain
                // atlas actually has. Never submit more views than any of them; never drop
                // to 0 (that blanks the display) — floor is 1. One-shot log per
                // distinct disagreement, never per-frame.
                {
                    const uint32_t sliceCap = tileColumns * tileRows;
                    int capped = eyeCount;
                    if (capped > (int)runtimeViewCount) capped = (int)runtimeViewCount;
                    if (capped > (int)sliceCap) capped = (int)sliceCap;
                    if (capped < 1) capped = 1;
                    if (capped != eyeCount) {
                        static uint32_t s_lastClampKey = 0;
                        const uint32_t key = ((uint32_t)eyeCount << 24) |
                                             (runtimeViewCount << 16) |
                                             (sliceCap << 8) | (uint32_t)capped;
                        if (key != s_lastClampKey) {
                            s_lastClampKey = key;
                            LOG_WARN("Submitted view count clamped %d -> %d "
                                     "(mode=%u located=%u slices=%u)",
                                     eyeCount, capped, modeViewCount,
                                     runtimeViewCount, sliceCap);
                        }
                        eyeCount = capped;
                    }
                }

                // Render tile = window × recommendedViewScaleXY (never the
                // swapchain/display size) — the handle-app tiling rule
                // (runtime#729, multiview-tiling.md).
                float scaleX = xr.renderingModeCount > 0 ? xr.renderingModeScaleX[mode] : 1.0f;
                float scaleY = xr.renderingModeCount > 0 ? xr.renderingModeScaleY[mode] : 1.0f;
                uint32_t renderW = (uint32_t)((double)g_windowW * scaleX);
                uint32_t renderH = (uint32_t)((double)g_windowH * scaleY);
                if (renderW == 0) renderW = 1;
                if (renderH == 0) renderH = 1;

                uint32_t imageIndex;
                if (AcquireSwapchainImage(xr, imageIndex)) {
                    // ADR-041 (runtime#1612): the layer carries EVERY located view;
                    // only [0, eyeCount) are rendered and the tail is aliased onto
                    // view 0 after the loop. Under PRIMARY_MULTIVIEW_DXR xrEndFrame
                    // refuses anything shorter, so a 1-view (2D) frame submitted as
                    // one view was dropped outright — the panel went 2D while still
                    // showing the last woven 3D frame. The max() only matters if the
                    // locate returned 0 views (eyeCount is floored at 1).
                    projectionViews.assign(
                        (size_t)(runtimeViewCount > (uint32_t)eyeCount ? runtimeViewCount : (uint32_t)eyeCount),
                        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                    VkImage targetImage = swapchainImages[imageIndex].image;
                    VkFormat swapFormat = (VkFormat)xr.swapchain.format;

                    for (int eye = 0; eye < eyeCount; eye++) {
                        int srcView = eye < (int)runtimeViewCount ? eye : 0;
                        float viewMat[16], projMat[16];
                        float clipNear = 0.0f, clipFar = 0.0f;
                        float clipCull = 0.0f; // renderEye's far cull; 0 = none (Windows' clip.clipFar)
                        XrFovf fov = views[srcView].fov;
                        if (useRig && camRig) {
                            // Tangent-space window shift for the asset's own
                            // principal point, minus the rest frustum centre.
                            if (camShiftU != 0.0f) {
                                fov.angleLeft  = atanf(tanf(fov.angleLeft)  + camShiftU);
                                fov.angleRight = atanf(tanf(fov.angleRight) + camShiftU);
                            }
                            if (camShiftV != 0.0f) {
                                fov.angleUp   = atanf(tanf(fov.angleUp)   + camShiftV);
                                fov.angleDown = atanf(tanf(fov.angleDown) + camShiftV);
                            }
                            // Scene-absolute planes, not vHeight-relative: on
                            // this rig there is no virtual display to anchor
                            // them to. FAR is deliberately far past any content
                            // (a deconverged lift puts the sky at the depth
                            // worker's cap); a splat rasteriser sorts rather
                            // than depth-tests, so the huge near:far ratio costs
                            // no precision. Transparent mode still clips at the
                            // ZDP, which here is the pivot (windows/main.cpp).
                            clipNear = kGsCameraRigNearM;
                            clipFar  = g_transparentBg ? g_camRig.pivotM : kGsCameraRigFarM;
                            if (clipFar < clipNear + 1e-4f) clipFar = clipNear + 1e-4f;
                            clipCull = g_transparentBg ? clipFar : 0.0f;
                            mat4_view_from_xr_pose(viewMat, views[srcView].pose);
                            if (camOrbiting) {
                                // view * scene: rigid, so the gaussian
                                // covariances rotate with it correctly.
                                float combined[16];
                                mat4_multiply(combined, viewMat, camSceneOrbit);
                                memcpy(viewMat, combined, sizeof(combined));
                            }
                            mat4_from_xr_fov(projMat, fov, clipNear, clipFar);
                        } else if (useRig) {
                            float ez = RigLocalEyeZ(cameraPose, views[srcView].pose.position);
                            // The Windows leg's one clip rule (clip_policy.h): opaque
                            // = unrestricted; transparent = the runtime's rear depth
                            // budget, or the standalone ZDP clip when it reports none.
                            const XrRearDepthBudgetDXR* budgetPtr =
                                (xr.hasDepthBudgetExt && depthBudget.type == XR_TYPE_REAR_DEPTH_BUDGET_DXR)
                                    ? &depthBudget : nullptr;
                            const dxr::ClipPlanes clip = dxr::ResolveClipPlanes(
                                ez, virtualDisplayHeight, budgetPtr, g_transparentBg, /*standalone=*/true);
                            if (eye == 0) {
                                // One line per change of the applied rear offset
                                // (rounded to 0.01 vH) — what the Windows HUD shows.
                                static int s_lastCenti = -1;
                                const int centi = (int)lroundf(clip.farOffsetVH * 100.0f);
                                if (centi != s_lastCenti) {
                                    s_lastCenti = centi;
                                    LOG_INFO("Rear clip: farOffsetVH=%.2f (%s, transparent=%d)",
                                             clip.farOffsetVH,
                                             budgetPtr ? dxr::RearDepthBudgetStateName(budgetPtr->state)
                                                       : "no budget",
                                             g_transparentBg ? 1 : 0);
                                }
                            }
                            clipNear = clip.near_z;
                            clipFar = clip.far_z;
                            clipCull = clip.clipFar;
                            mat4_view_from_xr_pose(viewMat, views[srcView].pose);
                            mat4_from_xr_fov(projMat, views[srcView].fov, clipNear, clipFar);
                        } else {
                            mat4_view_from_xr_pose(viewMat, views[srcView].pose);
                            mat4_from_xr_fov(projMat, views[srcView].fov, 0.01f, 100.0f);
                        }

                        uint32_t tileX = (uint32_t)(eye % (int)tileColumns);
                        uint32_t tileY = (uint32_t)(eye / (int)tileColumns);
                        uint32_t vpX = tileX * renderW;
                        uint32_t vpY = tileY * renderH;

                        projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                        projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                        projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                        projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                        projectionViews[eye].subImage.imageArrayIndex = 0;
                        projectionViews[eye].pose = views[srcView].pose;
                        // `fov` is the located one everywhere except the camera
                        // rig, where it also carries the principal-point shift —
                        // submit what was actually projected with.
                        projectionViews[eye].fov = fov;

                        if (g_gsRenderer.hasScene()) {
                            g_gsRenderer.renderEye(
                                targetImage, swapFormat,
                                xr.swapchain.width, xr.swapchain.height,
                                vpX, vpY, renderW, renderH,
                                viewMat, projMat,
                                g_transparentBg,
                                clipNear, clipCull, /*clipFadeFrac=*/0.15f);
                        } else if (eye == 0) {
                            // Nothing loaded yet: the Windows leg's slate
                            // placeholder, not stale swapchain contents (which
                            // the click-through would also read as coverage).
                            RenderPlaceholder(vkDevice, graphicsQueue, queueFamilyIndex,
                                              targetImage, swapFormat);
                        }
                    }
                    DxrAliasInactiveViews(projectionViews.data(), views, runtimeViewCount, (uint32_t)eyeCount);

                    // #833 click-through (windows/main.cpp's g_punch): while
                    // transparent, shape the window's INPUT region from the
                    // frame's own alpha — first and last view unioned — so
                    // clicks pass through to the desktop around the splats.
                    // Before the release: it hands the atlas back in
                    // COLOR_ATTACHMENT_OPTIMAL, the layout the runtime expects.
                    if (xr.hasAppWindow) {
                        uint32_t winPxW = 0, winPxH = 0;
                        g_window.current_size(&winPxW, &winPxH);
                        const uint32_t lastEye = (uint32_t)(eyeCount - 1);
                        ClickthroughParams cp;
                        cp.dev = vkDevice;
                        cp.phys = physDevice;
                        cp.queue = graphicsQueue;
                        cp.queueFamily = queueFamilyIndex;
                        cp.viewImage = targetImage;
                        cp.viewFormat = swapFormat;
                        cp.tileW = renderW;
                        cp.tileH = renderH;
                        cp.firstTileX = 0;
                        cp.firstTileY = 0;
                        cp.lastTileX = (lastEye % tileColumns) * renderW;
                        cp.lastTileY = (lastEye / tileColumns) * renderH;
                        cp.twoViews = eyeCount > 1;
                        cp.window = &g_window;
                        cp.winW = winPxW;
                        cp.winH = winPxH;
                        cp.transparentBg = g_transparentBg;
                        // A WM-decorated X11 window or a fullscreen one is never
                        // shaped: the frame needs the whole window for
                        // move/resize, and a fullscreen overlay has nothing
                        // beside it to click through to.
                        static const bool s_wmDecorated = [] {
                            const char* e = getenv("DXR_X11_WM_DECORATIONS");
                            return e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
                        }();
                        cp.decorated = g_window.is_fullscreen() ||
                                       (s_wmDecorated && g_window.backend() == DxrWindowBackend::X11);
                        cp.chromeVisible = g_window.header_bar_visible();
                        ClickthroughUpdate(cp);
                    }
                    ReleaseSwapchainImage(xr);
                }
            }
        }

        EndFrame(xr, fs.predictedDisplayTime,
            projectionViews.empty() ? nullptr : projectionViews.data(),
            (uint32_t)projectionViews.size());
    }

    LOG_INFO("Shutting down");
    if (xr.session) xrRequestExitSession(xr.session);
    vkDeviceWaitIdle(vkDevice);
    ClickthroughDestroy(vkDevice);
    if (g_placeholderPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, g_placeholderPool, nullptr);
    g_gsRenderer.cleanup();
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    g_window.destroy();   // LAST: the runtime's VkSurfaceKHR borrowed this connection
    return 0;
}

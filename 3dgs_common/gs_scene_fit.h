// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Scene fit for the DISPLAY rig: where the subject is, and how big the
 *         virtual display has to be to frame it comfortably.
 *
 * ## Why this file exists
 *
 * The display rig's auto-fit answers two questions at load: WHERE is the
 * subject (the fit centre, which is also the orbit pivot and the convergence
 * plane) and HOW BIG is the virtual display that frames it (`vHeight`). Both
 * answers used to live inside the renderers, and there were two renderers:
 *
 *   - the COMPUTE leg (`gs_renderer.cpp`, desktop x86-64) answered WHERE with
 *     an opacity-weighted 64^3 voxel density grid and a flood-fill from the
 *     peak voxel — it finds the main contiguous blob and ignores the walls and
 *     the floor, which are air-separated from it;
 *   - the GRAPHICS leg (`gs_adreno_renderer.cpp`, Apple Silicon / Android /
 *     Windows-on-ARM) answered it with the 5th-95th percentile box per axis,
 *     because that leg keeps only splat centres and the flood-fill was
 *     described as "a refinement, not a correctness requirement".
 *
 * They are not the same answer. The same `butterfly.spz` framed one way on
 * Windows and another way on a Mac or a tablet, which is a framing bug that
 * only shows up when you put the two screens side by side. This module is the
 * ONE implementation both legs are meant to call: it is pure, it takes the
 * loader's CPU-side `GsVertex` array, and it touches no Vulkan and no renderer
 * state. The flood-fill wins because it is the better algorithm; the
 * percentile box survives as @ref GsPercentileBounds, the documented fallback
 * for when the flood-fill finds nothing to fill.
 *
 * ## The second thing that was wrong: the fit was flat
 *
 * `dxr::AutoFitVHeight` (displayxr-common `common/auto_fit.h`) caps the
 * subject at `fill` of the viewport in x and y. That is the right rule and it
 * is kept here verbatim as the core — but on a 3D display x and y are not the
 * whole story:
 *
 *   - a DEEP scene (a corridor, a street, a room seen down its long axis)
 *     "fits" by that rule while its far end sits far behind the screen and its
 *     near end far in front of it, i.e. at a disparity nobody can fuse;
 *   - the rule measures WORLD x and y, but the viewer sees the subject from
 *     the rig's pose. A blob that is wide in world-x is not wide on screen
 *     once the rig has yawed 90 degrees around it.
 *
 * @ref GsFitFrame fixes both: it projects the bounds into the viewing pose's
 * own screen axes before applying the x/y rule, and then bounds the result by
 * a DEPTH budget. At `yaw == pitch == 0` the projection reduces exactly to the
 * old `extent[0]`, `extent[1]` (the AABB corners land on the axes), so the
 * flat case is unchanged to the bit.
 *
 * ## The depth budget, and why it grows vHeight instead of moving the pivot
 *
 * The fit centre is the pivot AND the zero-disparity plane (ZDP). Content at a
 * depth `d` off that plane shows up on the panel as horizontal disparity. With
 * the viewer at perpendicular distance `ez` from the ZDP and eyes `E` apart:
 *
 *     behind the screen:  disparity = E * d / (ez + d)
 *     in front of it:     disparity = E * d / (ez - d)
 *
 * Both are pure functions of `d/vHeight` once `ez` and `E` are expressed in
 * units of `vHeight` (see @ref GsFitComfort), which is what makes this
 * scale-free: doubling the size of a scene must not change how it is framed.
 * Capping the disparity at `maxDisparityVH` gives a maximum half-depth on each
 * side, in vHeight units:
 *
 *     deltaBehind = m*k / (s - m)      deltaFront = m*k / (s + m)
 *
 * with `k = viewerDistanceVH`, `s = eyeSeparationVH`, `m = maxDisparityVH`.
 * The front limit is always the tighter of the two — that asymmetry is real,
 * not a modelling artefact, and it is why content that pokes out of the screen
 * fatigues first.
 *
 * Two ways to respect that budget: GROW vHeight (the subject renders smaller,
 * every disparity shrinks with it) or MOVE the pivot back (trade front budget
 * for rear budget). We grow vHeight. Moving the pivot moves the orbit centre
 * and the convergence plane, which is the one thing `gs_camera_rig.h` is
 * emphatic must stay a single point — a viewer that orbits about one place and
 * converges at another sees two different scenes depending on whether they are
 * moving. `pivotDepthShift` is still REPORTED, because the shift that would
 * equalise the front and rear headroom is a genuinely useful number for a
 * caller that decides to spend it; nothing in the returned `vHeight` assumes
 * it was applied.
 *
 * A scene that cannot be made comfortable by growing vHeight by
 * @ref kGsFitMaxDepthGrowth is not a display-rig scene. We clamp, set
 * `depthBudgetSatisfied = false`, and let the caller say so — that is the
 * honest signal for an open photo-lifted cloud, which wants the CAMERA rig
 * (`gs_camera_rig.h`) and not this one.
 *
 * ## Units and frame
 *
 * World units throughout, app convention (RUB: +x right, +y up, +z back), the
 * same space the loader hands back. `extent[]` is always a FULL size, never a
 * half-extent — matching `getMainObjectBounds` and `dxr::AutoFitVHeight`.
 *
 * ## What this module deliberately does NOT do
 *
 * No logging on any per-frame path (there is no per-frame path here: every
 * function is a load-time measurement), no OpenXR types, no Vulkan, no global
 * state. It is testable from a command line — see `gs_fit_probe`.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gs_scene_loader.h"  // GsVertex

// ─────────────────────────────────────────────────────────────────────────
// The point set the fit measures
// ─────────────────────────────────────────────────────────────────────────

//! One splat reduced to what the fit actually reads: a centre and an opacity.
//!
//! Mirrors the `GsPickData` the compute leg already builds, minus `maxScale`
//! (the ray test wants it; the fit does not). Kept as its own type so this
//! module does not depend on either renderer's header.
struct GsFitPoint {
    float x, y, z;
    float opacity;
};

//! Build the point set the fit measures, INCLUDING the floater rejection.
//!
//! This is not a formality. The compute leg's flood-fill has never run on the
//! raw cloud: `GsRenderer::loadScene` drops far-flung outliers from
//! `pickData_` first (training artefacts — `KAWS FAMILY.spz` scatters ~3.4% of
//! its splats out to +/-240 while the model spans +/-15), and the flood-fill's
//! 5-95 percentile bootstrap is taken over what SURVIVES that. Porting the
//! flood-fill without the trim would silently change its answer on exactly the
//! scenes the trim was added for, so the trim comes with it.
//!
//! The rule, verbatim from the compute leg: for each axis take the robust
//! [p2, p98] range, keep anything within @ref kGsFitOutlierMargin times half
//! that range of its centre. Clean scenes lose nothing — the margin
//! comfortably exceeds their true extent. Gated at 1024 splats, below which
//! percentiles mean nothing and every splat is kept.
//!
//! Order is preserved (stable compaction), because the voxel densities this
//! feeds are float sums and their order is part of the answer.
void GsBuildFitPoints(const GsVertex* verts, size_t count,
                      std::vector<GsFitPoint>& out);

//! Margin on the [p2, p98] core, in multiples of half that range.
constexpr float kGsFitOutlierMargin = 3.0f;

//! Minimum splat count for the percentile machinery to mean anything.
constexpr size_t kGsFitMinPercentileCount = 1024;

// ─────────────────────────────────────────────────────────────────────────
// WHERE: the bounds of the thing being framed
// ─────────────────────────────────────────────────────────────────────────

//! How the bounds were arrived at. A wrong framing and a right one look
//! identical until you know which branch produced it — the same reason
//! `GsCameraRig` records `rigSource` / `focusSource` / `intrinsicsSource`.
enum class GsBoundsSource {
    None,             //!< nothing measurable
    FloodFillObject,  //!< flood-fill, single-object regime (full AABB + comfort)
    FloodFillScene,   //!< flood-fill, scene-with-central-object regime (centroid)
    Percentile,       //!< the [p5, p95] box — the documented fallback
};

//! Centre + FULL extent of the region to frame, and where it came from.
struct GsFitBounds {
    bool  valid  = false;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float extent[3] = {0.0f, 0.0f, 0.0f};  //!< FULL sizes, not half-extents

    GsBoundsSource source = GsBoundsSource::None;

    //! Flood-fill diagnostics, zero unless `source` is one of the FloodFill*
    //! values. `fillRatio` is the fraction of the 64^3 grid the fill covered
    //! and is what picks the regime.
    size_t filledVoxels = 0;
    size_t totalVoxels  = 0;
    float  fillRatio    = 0.0f;
    float  threshold    = 0.0f;  //!< accepted threshold, x peak density
};

//! Locate the main object: opacity-weighted voxel density, flood-fill from the
//! peak voxel, bbox of what filled.
//!
//! PORTED VERBATIM from the compute leg (`GsRenderer::getMainObjectBounds`),
//! including the adaptive threshold ladder, the two output regimes and the
//! 1.10x comfort margin they both bake in. Callers of the old method get the
//! same numbers out of this one; callers of the graphics leg's percentile box
//! get a DIFFERENT (better) number, which is the point of the change.
//!
//! The 1.10x is load-bearing at the call sites: `macos/main.mm` and
//! `windows/main.cpp` pass `fill = 0.88 = 0.80 * 1.10` to cancel it and net an
//! 80% cap of the TRUE object. Do not unbake it here — the multiplier differs
//! between the two regimes and the call sites cannot tell which one ran.
//!
//! `gridSize` is the voxel grid edge (64 everywhere today); below 4 it refuses.
//! Returns false when there is nothing to find, and the caller should then
//! fall back to @ref GsPercentileBounds.
bool GsMainObjectBounds(const GsVertex* verts, size_t count,
                        uint32_t gridSize,
                        float outCenter[3], float outExtent[3]);

//! Same, reporting the provenance and the fill diagnostics.
GsFitBounds GsMainObjectBoundsEx(const GsVertex* verts, size_t count,
                                 uint32_t gridSize = 64u);

//! The [loPct, hiPct] percentile box per axis — the graphics leg's historical
//! answer, kept as the fallback for when the flood-fill finds nothing.
//!
//! Runs over the SAME trimmed point set as the flood-fill, so the two are
//! comparable; the graphics leg used to run it over the untrimmed cloud, which
//! is a second, quieter reason its numbers differed. No comfort margin is
//! baked in here (there never was one), so a caller that falls back to this
//! and keeps passing `fill = 0.88` is framing 10% larger than it meant to —
//! @ref GsFitFrame handles that by taking the bounds as given and saying so.
bool GsPercentileBounds(const GsVertex* verts, size_t count,
                        float loPct, float hiPct,
                        float outCenter[3], float outExtent[3]);

//! Bounds with the fallback already wired: flood-fill, then percentile.
//! This is what a call site wants.
GsFitBounds GsResolveFitBounds(const GsVertex* verts, size_t count,
                               uint32_t gridSize = 64u);

// ─────────────────────────────────────────────────────────────────────────
// HOW BIG: vHeight, depth-aware
// ─────────────────────────────────────────────────────────────────────────

//! The nominal viewer this module frames for, in units of the virtual display
//! height. Every field is a RATIO, which is what keeps the fit scale-free.
//!
//! The defaults describe an ordinary desktop 3D panel — roughly a 30 cm-tall
//! display at 60 cm, seen by a 63 mm face:
//!
//!   - `viewerDistanceVH = 2.0`  — 60 cm / 30 cm.
//!   - `eyeSeparationVH  = 0.21` — 63 mm / 30 cm. This is the on-panel
//!     disparity of a point at infinity, which is the only disparity the
//!     geometry has a hard ceiling at.
//!   - `maxDisparityVH   = 0.03` — 3% of the panel height, i.e. ~1.7% of the
//!     width at 16:9, inside the 1-2%-of-width band autostereo displays are
//!     normally specified to.
//!
//! A caller that KNOWS the real numbers should pass them: the runtime reports
//! the tracked eye spread (`XrViewDisplayRawDXR::rawEyes`) and the canvas
//! height in metres, and `eyeSeparationVH = ipd_m / canvasHeight_m` is then
//! exact rather than nominal. The defaults exist so a load-time fit does not
//! have to wait for the first `xrLocateViews`.
struct GsFitComfort {
    float viewerDistanceVH = 2.0f;
    float eyeSeparationVH  = 0.21f;
    float maxDisparityVH   = 0.03f;
};

//! Hard ceiling on what the depth budget may do to the x/y fit, as a
//! multiplier on the flat vHeight.
//!
//! Past this the subject is so small that respecting the budget has stopped
//! being a framing and started being a retreat — the scene is deep in a way
//! the display rig cannot show, and the caller should hear about it rather
//! than watch the subject shrink to a dot. 4x is a little over two stops.
constexpr float kGsFitMaxDepthGrowth = 4.0f;

//! What @ref GsFitFrame worked out.
struct GsFitFrameResult {
    bool  valid = false;

    //! The answer: the display rig's `virtualDisplayHeight`.
    float vHeight = 0.0f;

    //! The flat (x/y-only) fit, i.e. exactly what `dxr::AutoFitVHeight` would
    //! have returned for the pose-projected extents. `vHeight` is this, or
    //! this grown by the depth term. Reported so a caller can see the cost.
    float vHeightFlat = 0.0f;

    //! The vHeight the depth budget alone asks for, before the growth clamp.
    float vHeightDepth = 0.0f;

    //! Pose-projected FULL extents of the bounds: on-screen width, on-screen
    //! height, and depth along the view axis. At yaw = pitch = 0 these are
    //! `extent[0]`, `extent[1]`, `extent[2]`.
    float screenW = 0.0f, screenH = 0.0f, screenD = 0.0f;

    //! Advisory: move the pivot this far ALONG the view axis, away from the
    //! viewer, to equalise the front and rear disparity headroom. World units,
    //! >= 0. The returned `vHeight` does NOT assume it was applied — see the
    //! file header for why the pivot stays put by default.
    float pivotDepthShift = 0.0f;

    //! False when the depth term hit @ref kGsFitMaxDepthGrowth, i.e. the scene
    //! is too deep for the display rig to frame comfortably at any sane size.
    bool  depthBudgetSatisfied = true;

    //! Which term set `vHeight`: "height", "width" or "depth".
    const char* boundBy = "height";
};

//! Frame `bounds` for a viewer looking at it from (yaw, pitch), in a viewport
//! of `viewportW` x `viewportH` (pixels or metres — only the aspect is read).
//!
//! `yaw`/`pitch` are radians in the display rig's own convention, the one
//! `quat_from_yaw_pitch` implements: yaw about +y, pitch about +x, applied
//! Ry(yaw) * Rx(pitch), with forward = -z at rest. `g_input.yaw` /
//! `g_input.pitch` go straight in.
//!
//! `fill` is `dxr::AutoFitVHeight`'s fill fraction and keeps its meaning
//! exactly (0.88 at the existing call sites — see @ref GsMainObjectBounds for
//! why it is not 0.80).
void GsFitFrame(const GsFitBounds& bounds,
                float viewportW, float viewportH,
                float fill,
                float yaw, float pitch,
                float& outVHeight, float& outPivotDepthShift);

//! Same, with the working shown.
GsFitFrameResult GsFitFrameEx(const GsFitBounds& bounds,
                              float viewportW, float viewportH,
                              float fill,
                              float yaw, float pitch,
                              const GsFitComfort& comfort = GsFitComfort());

//! The maximum half-depth, in vHeight units, that `comfort` allows in front of
//! and behind the ZDP. Exposed because it is the whole depth model in two
//! numbers, and a HUD or a probe wants to print them.
//!
//! `outBehind` is +infinity when `eyeSeparationVH <= maxDisparityVH`: a point
//! at infinity then sits inside the budget on its own and nothing behind the
//! screen can ever violate it.
void GsFitDepthBudgetVH(const GsFitComfort& comfort,
                        float& outFront, float& outBehind);

//! The viewing basis for (yaw, pitch): the screen right and up axes and the
//! forward (away-from-viewer) axis, in world space. Shared with the probe so
//! the projection is described in exactly one place.
void GsFitViewBasis(float yaw, float pitch,
                    float outRight[3], float outUp[3], float outForward[3]);

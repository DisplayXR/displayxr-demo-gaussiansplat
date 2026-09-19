// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The CAMERA rig: framing a photo-lifted splat through the camera it
 *         was predicted from, and the flags that describe one on the CLI.
 *
 * ## Two rigs, chosen by scene type
 *
 * DISPLAY RIG (the viewer's original, unchanged). Object-centric scenes — the
 * bundled `butterfly.spz`, product turntables. Auto-fit the scene's AABB to
 * the virtual display, orbit the DISPLAY around the subject, idle turntable.
 *
 * CAMERA RIG (this file). Open scenes lifted from a photograph. The splat's
 * origin IS the left capture camera and the gaussians were unprojected through
 * a known focal, so the correct rest view is that camera with its intrinsics
 * conserved — "the render IS the left photo". An auto-fit picture of such a
 * scene is wrong three ways at once: wrong zoom (a splat built at f_s rendered
 * through a frustum of f_v is scaled by f_v/f_s about the centre), wrong pivot,
 * and a turntable the data cannot support (the outer haze has no support more
 * than ~15 deg off the capture axis — it is grain, not parallax).
 *
 * Selected by the scene, overridden by the user: a `camera` block in the SOG
 * `meta.json` means camera rig, its absence means display rig, and `--rig=`
 * forces either.
 *
 * ## DECLARE, never compute
 *
 * On the native viewer the eyes are the RUNTIME's — head-tracked, correct for
 * the panel. So the camera rig is DECLARED as an `XR_DXR_view_rig`
 * `XrCameraRigDXR` descriptor built from the photo's intrinsics, and the app
 * consumes the render-ready `XrView{pose, fov}` that comes back. This file
 * computes the DESCRIPTOR, never the eye math. It deliberately contains no
 * Kooima, no off-axis frustum and no OpenXR types — the platform glue turns
 * these numbers into the descriptor.
 *
 * The one thing the descriptor cannot carry is an off-centre principal point
 * (`XrCameraRigDXR` has `verticalFov` and nothing else about the image plane),
 * so `PrincipalShiftTan()` hands the glue the tangent-space shift to apply to
 * the returned fov. That is the ASSET's own intrinsic, not a re-derivation of
 * the runtime's eye perturbation, and it is exactly zero for the centred
 * principal point every asset has today.
 *
 * ## Reference implementation
 *
 * The gallery's Spatial View tier
 * (`displayxr-gallery-pvt/src/lib/spatialView/camera.ts`) is the model this
 * mirrors: `rigFromAsset` / `frustumFor` / `restEye` / `clampEye` /
 * `orbitFromDrag`, with NEAR 0.02, FAR 5000, a 15 deg comfort cone and the
 * pivot at `min(dConv, dSubject)`. Two deliberate differences, both because
 * this is the native viewer and not a web canvas:
 *
 *  - the web tier MOVES THE EYE (hover / device tilt are head analogues).
 *    Here the runtime owns the eye, so eye motion is head tracking and the
 *    comfort cone applies only to the DRAG, which orbits the scene.
 *  - `camera.ts` derives the pivot from the pair's stored `convergence` /
 *    `disp_median` columns, i.e. `min(dConv, dSubject)`. A v2 `camera` block
 *    carries that decision directly as `focus.point`; without one the viewer
 *    falls back to the cloud's median disparity, which is the `dSubject` half
 *    of the same `min`. Full waterfall: GsResolveCameraRig.
 *
 * ## Focus is one point, and it is the same point on both rigs
 *
 * The orbit centre, the pivot plane that stays put under head motion, and the
 * convergence depth are THE SAME THING and are stored once
 * (`GsCameraRig::focusLocal`). A viewer that orbits about one place and
 * converges at another shows the user two different scenes depending on
 * whether they are moving. A double-click moves that one point; everything
 * follows.
 *
 * ## Units and frame
 *
 * Intrinsics are OpenCV, in pixels of ONE EYE's image; the scene is metric.
 * The loader has already righted the gaussians RDF -> RUB, so in app space the
 * rest camera sits at the origin looking down -z with +y up, and everything
 * derived from `cy` flips sign.
 */

#pragma once

#include <string>
#include <vector>

#include "gs_scene_loader.h"  // GsVertex, GsSceneCamera

// GsRigKind is declared in gs_scene_loader.h, because the FILE can carry a rig
// hint and the loader must be able to express it without depending on this
// module.

//! Near/far of the camera rig, metres. Deliberately far past any content: a
//! deconverged lift puts the sky at the depth worker's cap and the refinement
//! scatters some of it further still, and a splat rasteriser composites by
//! sorting rather than depth-testing, so a huge near:far ratio costs nothing.
//! (camera.ts NEAR/FAR, same numbers, same reason.)
constexpr float kGsCameraRigNearM = 0.02f;
constexpr float kGsCameraRigFarM  = 5000.0f;

//! Half-angle of the comfort cone about the pivot, degrees. 15, not 20: at 20
//! real captures show heavy floaters, so the extra 5 deg buys grain rather
//! than parallax. Measured on the shipping gallery assets.
constexpr float kGsRigMaxAngleDeg = 15.0f;

//! Sanity clamp on the pivot depth, metres. Only bites on missing or
//! degenerate calibration; real assets land in 0.5-5 m.
constexpr float kGsPivotMinM = 0.1f;
constexpr float kGsPivotMaxM = 20.0f;

//! Nominal human interpupillary distance, metres — the separation the runtime's
//! tracked eyes have when the viewer is an average adult. `XrCameraRigDXR`'s
//! ipdFactor is an ABSOLUTE scale on that spread, so turning a capture baseline
//! into it means dividing by this. It is also, not by coincidence, the default
//! capture baseline: a 63 mm rig and a 63 mm face give ipdFactor 1.
constexpr float kGsNominalHumanIpdM = 0.063f;

//! Fallback capture baseline for a `camera` block with no `stereo` object.
constexpr float kGsDefaultBaselineM = 0.063f;

//! Pivot used when neither `--pivot=` nor the cloud offers anything better.
constexpr float kGsFallbackPivotM = 2.0f;

//! A scene rotation about the pivot, radians. What a drag produces.
struct GsOrbit {
    float yaw   = 0.0f;
    float pitch = 0.0f;
};

//! The rig flags, parsed from argv. Every field is opt-in: an unset flag means
//! "take it from the file", so a bare launch of a `camera`-carrying `.sog` is
//! unaffected and a bare launch of anything else keeps the display rig.
struct GsRigFlags {
    bool      hasRig = false;
    GsRigKind rig    = GsRigKind::Display;

    bool  hasFx = false;  float fx = 0.0f;
    bool  hasFy = false;  float fy = 0.0f;
    bool  hasCx = false;  float cx = 0.0f;
    bool  hasCy = false;  float cy = 0.0f;
    bool  hasSize = false; int width = 0, height = 0;   //!< --size=WxH
    bool  hasBaseline = false; float baselineM = 0.0f;
    bool  hasPivot = false;    float pivotM = 0.0f;

    //! Initial rendering-mode index (`--mode=`), i.e. what the 0/1/2/3 keys
    //! select: 0 is the 2D passthrough the runtime advertises first, 1 the
    //! first 3D mode, and so on. Not a rig property — it lives here because
    //! this is the viewer's own flag namespace, and launching straight into a
    //! known mode is what makes a rest view reproducible from a script.
    bool  hasMode = false;     int mode = 0;

    //! `--focus-weight=centre` — restrict the median-disparity focus estimate
    //! to the middle of the frame. Off by default: on a scene whose subject
    //! IS central it barely moves the answer, and on one whose subject is not
    //! it silently prefers whatever happens to be in the middle. Kept because
    //! it is the right answer for a portrait and the wrong one for a
    //! landscape, and only the user knows which they have.
    bool  centreWeightedFocus = false;

    //! `--window=WxH` — the window size to open at, in points. Not a rig
    //! property either, but the rig's whole job is to conserve a frustum and
    //! the window's ASPECT is what the runtime derives the horizontal half of
    //! that frustum from. Being able to ask for a portrait window is what
    //! makes "does a portrait asset frame correctly" a thing you can check
    //! rather than assert.
    bool  hasWindow = false;   int windowW = 0, windowH = 0;
};

//! Intrinsics recovered from the cloud itself, for a scene that declares none.
//!
//! A photo-lifted cloud remembers its camera whether or not anyone wrote it
//! down: every gaussian was unprojected along a ray through the lens, so the
//! ANGULAR EXTENT of the cloud about the rest camera IS the frustum it was
//! fitted in. Measuring the 1st and 99th percentile of x/z and y/z recovers the
//! half-tangents (the percentiles rather than the extremes because a lift
//! always leaves a few stray gaussians far outside the frame, and one of those
//! would set the focal on its own).
//!
//! Validated on the gallery's harbour asset, whose true intrinsics are known:
//! P1/P99 of x/z came out -0.854/+0.863 against a true +/-0.857, and of y/z
//! -0.480/+0.505 against +/-0.482 — i.e. within a percent, and the asymmetry
//! it reports IS the principal-point offset rather than noise.
struct GsIntrinsicsEstimate {
    bool  valid = false;
    float fx = 0.0f, fy = 0.0f, cx = 0.0f, cy = 0.0f;
    int   width = 0, height = 0;
    //! The measured tangent window, for the log and for comparison against a
    //! block that also declares intrinsics.
    float tanLeft = 0.0f, tanRight = 0.0f, tanDown = 0.0f, tanUp = 0.0f;
    //! Implied 35 mm-equivalent focal, the number a photographer would
    //! recognise, and the one the sanity gate is expressed in.
    float focal35mm = 0.0f;
    //! True when the gate rejected the measurement and a nominal 28 mm-eq
    //! frustum was substituted (the aspect is still the measured one).
    bool  usedFallback = false;
    std::string note;
};

//! Widest and narrowest 35 mm-equivalent focal the estimator will believe.
//! Outside this the measurement is not a lens, it is a cloud with a long tail
//! or a scene that was never a photograph.
constexpr float kGsEstMinFocal35 = 14.0f;
constexpr float kGsEstMaxFocal35 = 85.0f;
//! What to assume when the gate rejects the measurement: a 28 mm-eq frustum,
//! the most ordinary phone-camera field of view there is.
constexpr float kGsFallbackFocal35 = 28.0f;

//! Recover intrinsics from a righted (RUB) cloud. Returns false only when the
//! cloud has too little in front of the camera to measure at all.
bool GsEstimateIntrinsics(const std::vector<GsVertex>& vertices,
                          GsIntrinsicsEstimate& out);

//! Parse the rig flags out of argv.
//!
//! Grammar is the repo's `--key=value` (never `--key value`), matching
//! displayxr-common's launch-args parser, and `--` ends flag parsing. Tokens
//! this does not recognise are left alone — argv is ALSO handed to the shared
//! launch parser, which owns `--src`/`--vh`/`--pose`/... — so both read the
//! same command line without either having to know the other's flags.
//!
//! Unparseable values append a one-line reason to `warnings` (when non-null)
//! and leave the field unset: a typo must not silently frame the scene through
//! a camera nobody asked for.
//!
//!   --rig=display|camera
//!   --fx=<px> --fy=<px> --cx=<px> --cy=<px>
//!   --size=WxH        eye-image size in pixels
//!   --baseline=<m>    capture baseline, metres
//!   --pivot=<m>       pivot / convergence depth, metres
//!   --mode=<index>    initial rendering mode (0 = 2D passthrough, 1 = first 3D)
//!   --focus-weight=centre|frame   restrict the median-disparity focus to the
//!                                 middle of the frame (default: frame)
//!   --window=WxH      open the window at this size in points
void GsParseRigFlags(int argc, const char* const* argv, GsRigFlags& out,
                     std::vector<std::string>* warnings = nullptr);

//! The resolved camera rig: everything the platform glue needs to declare an
//! XrCameraRigDXR and to orbit the scene, and nothing else.
struct GsCameraRig {
    bool  valid = false;

    //! Intrinsics, EYE-image pixels (OpenCV).
    float fx = 0.0f, fy = 0.0f, cx = 0.0f, cy = 0.0f;
    int   width = 0, height = 0;

    //! Capture baseline, metres.
    float baselineM = kGsDefaultBaselineM;

    //! THE FOCUS, in rest-camera space, app convention (+x right, +y up,
    //! +z back — so a point in front has a NEGATIVE z).
    //!
    //! One point, three jobs that must never disagree: the orbit centre, the
    //! pivot plane that stays put under head motion, and the convergence
    //! depth. Keeping them as one field is the point — a viewer that orbits
    //! about one place and converges at another shows the user two different
    //! scenes depending on whether they are moving.
    float focusLocal[3] = {0.0f, 0.0f, -kGsFallbackPivotM};

    //! Pivot / convergence depth, metres: the focus point's distance along the
    //! camera axis, i.e. -focusLocal[2]. Derived, never set independently.
    float pivotM = kGsFallbackPivotM;

    //! Where each resolved field came from, for the log and the HUD. The
    //! waterfall has four levels and a wrong answer looks identical to a right
    //! one until you know which level produced it.
    std::string rigSource;         //!< "--rig", "camera.rig", "block present", "no block"
    std::string focusSource;       //!< "--pivot", "block", "median-disparity", "scene bounds", "fallback"
    std::string intrinsicsSource;  //!< "block", "flags", "estimated", "estimated (28mm fallback)"

    //! Populated when the intrinsics were measured from the cloud, OR when the
    //! block declared them and a measurement was taken anyway to compare.
    GsIntrinsicsEstimate estimate;

    //! Absolute scalars from the block's `dxr` object, applied on top of the
    //! measured-IPD scaling. 1.0 unless the file asked otherwise.
    float dxrIpdFactor = 1.0f;
    float dxrParallaxFactor = 1.0f;

    //! Move the focus. Recomputes pivotM, so these two can never drift apart.
    //! Takes rest-camera space, app convention.
    void SetFocusLocal(float x, float y, float z);

    //! Rest pose of the camera in APP space (RUB), converted from the block's
    //! OpenCV pose. Position metres, quaternion xyzw.
    float restPosition[3] = {0.0f, 0.0f, 0.0f};
    float restRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    //! Photo half-tangents (half-width / focal), i.e. tan of the photo's own
    //! half-FOV on each axis.
    float TanHalfPhotoW() const { return (fx > 0.0f) ? (0.5f * (float)width) / fx : 0.0f; }
    float TanHalfPhotoH() const { return (fy > 0.0f) ? (0.5f * (float)height) / fy : 0.0f; }

    //! Full vertical FOV, radians, to declare for a canvas of this aspect
    //! (width/height).
    //!
    //! COVER, not letterbox. The descriptor carries one angle and the runtime
    //! derives the horizontal from the canvas aspect, so a canvas whose aspect
    //! differs from the photo's cannot show exactly the photo's rectangle. It
    //! can show a CROP of it at the photo's own angular scale, which is what
    //! "the render IS the left photo" actually asks for — every rendered pixel
    //! is the ray the photo had there. Overscanning instead would draw beyond
    //! the frustum the gaussians were fitted in, which is where the haze and
    //! the floaters live. When the canvas aspect equals the photo's, this is
    //! the photo's vertical FOV exactly.
    float VerticalFovRad(float canvasAspect) const;

    //! 1 / pivot depth, in world units (metres) — XrCameraRigDXR's
    //! convergenceDiopters.
    float ConvergenceDiopters() const;

    //! ABSOLUTE eye-separation scale for XrCameraRigDXR::ipdFactor, so the
    //! runtime's two eyes end up `baselineM` apart in world units.
    //!
    //! `measuredEyeSeparationM` is the spread the runtime actually reports for
    //! this display (XrViewDisplayRawDXR::rawEyes), because ipdFactor scales
    //! THAT, not some nominal. Passing <= 0 falls back to a nominal 63 mm face,
    //! which is only right by luck: the sim display reports 60 mm and a real
    //! panel reports whatever it tracked, so a fixed divisor would render the
    //! stereo pair at up to a few percent of the wrong baseline.
    //!
    //! NOT normalised against convergence: on the camera rig the ipd and the
    //! parallax are absolute scales, and folding 1/convergence into them (the
    //! trick that makes a display<->camera toggle disturbance-free) would make
    //! the stereo depth of a photo-lifted scene depend on where the pivot
    //! happened to land.
    float IpdScale(float measuredEyeSeparationM) const;

    //! The pose to DECLARE as XrCameraRigDXR::pose.
    //!
    //! NOT the same point as `restPosition`, and it depends on how many views
    //! the active rendering mode asks for.
    //!
    //! MULTI-VIEW (`monoView == false`): the descriptor's pose is the camera
    //! the runtime straddles with the tracked eye pair — the head CENTRE —
    //! while the block's `rest` is the LEFT capture camera, because that is
    //! where a single-camera lift puts the origin. So the rig sits half a
    //! baseline to the right of rest, which lands the left rendered view on the
    //! left capture camera and the right one on the right: at rest the two
    //! views ARE the captured pair. (It also makes the comfort cone symmetric
    //! about the head rather than lopsided about one eye — camera.ts clampEye
    //! measures from (B/2, 0) for exactly this reason.)
    //!
    //! MONO (`monoView == true`): there is no pair to straddle, so the one view
    //! should be the photograph itself — the LEFT capture camera, i.e. `rest`
    //! unchanged. This is what the gallery's 2D tier does (`restEye()` is the
    //! origin), and it is the difference between "the 2D view IS the photo" and
    //! "the 2D view is a synthesised half-baseline-right of the photo".
    void RigPose(bool monoView, float outPosition[3], float outRotation[4]) const;

    //! Tangent-space shift for an off-centre principal point: add `du` to both
    //! horizontal fov tangents and `dv` to both vertical ones. Zero for a
    //! centred principal point. `dv` already carries the OpenCV-y-down ->
    //! app-y-up sign flip.
    void PrincipalShiftTan(float& du, float& dv) const;

    //! Column-major 4x4 that rotates the SCENE about the pivot by (yaw, pitch),
    //! radians — what a drag produces. Pre-multiply the runtime's view matrix
    //! by this (`view * scene`), never move the camera: on the wall the eyes
    //! are the tracker's and an eye displacement would fight it, and on a flat
    //! panel an off-axis window shift reads as shear rather than rotation.
    void SceneOrbitMatrix(float yawRad, float pitchRad, float out[16]) const;

    //! Clamp an orbit into the comfort cone.
    void ClampOrbit(float& yawRad, float& pitchRad) const;
};

//! What the cloud itself says, measured ONCE at load.
//!
//! The rig is resolved long after the vertices are freed (they go to the GPU
//! and the CPU copy is dropped — it is 270 MB on a million-gaussian scene), so
//! everything the waterfall might need from the cloud is measured while it is
//! still in hand and carried in this. Both medians are taken because which one
//! the user wants is a flag, and re-reading the cloud to answer it is not an
//! option.
struct GsSceneMeasurements {
    bool  valid = false;
    GsIntrinsicsEstimate estimate;
    float medianDepthM       = 0.0f;  //!< median disparity, whole frame
    float medianDepthCentreM = 0.0f;  //!< median disparity, middle of the frame
};

//! Measure a righted (RUB) cloud. O(n), a few passes, once per load.
GsSceneMeasurements GsMeasureScene(const std::vector<GsVertex>& vertices);

//! Everything the resolver needs that is not the file or the flags.
struct GsRigResolveInput {
    //! What GsMeasureScene found at load. Null means the cloud levels of the
    //! waterfall are skipped.
    const GsSceneMeasurements* measurements = nullptr;
    //! Coarse fallback focus depth — the main object's centre depth — which is
    //! all a `--rig=camera` override on a non-photo scene can offer. <= 0 when
    //! unknown.
    float boundsForwardDepthM = 0.0f;
};

//! Resolve the rig from the file, the flags and the cloud, in that order of
//! preference, recording where every field came from.
//!
//! THE WATERFALL — each level is tried in turn and the first that yields an
//! answer wins. `out.rigSource` / `intrinsicsSource` / `focusSource` name the
//! level that did, because a wrong answer and a right one look identical until
//! you know which level produced it.
//!
//!   intrinsics: block -> --fx/--fy/--cx/--cy/--size -> estimated from the
//!               cloud's angular extent -> 28 mm-eq with the measured aspect
//!   focus:      --pivot= -> camera.focus.point -> median disparity
//!               -> scene bounds -> 2 m
//!
//! Returns false (with `why` set) only when there is nothing to frame with at
//! all — no block, no flags and no cloud. The caller must then stay on the
//! display rig rather than invent a camera.
bool GsResolveCameraRig(const GsSceneCamera& cam,
                        const GsRigFlags& flags,
                        const GsRigResolveInput& in,
                        GsCameraRig& out,
                        std::string* why = nullptr);

//! Which rig a scene should be framed with, and why.
//!   --rig= -> camera.rig -> (block present ? camera : display)
GsRigKind GsSelectRigKind(const GsSceneCamera& cam, const GsRigFlags& flags,
                          std::string* source = nullptr);

//! The block's rest pose, righted from its OpenCV convention into app space
//! (RUB), as `GsResolveCameraRig` does internally. Exposed because the DISPLAY
//! rig needs the same conversion for a block that carries a rest pose and asks
//! for that rig — and doing it twice, differently, is how conventions drift.
//! Identity in, identity out.
void GsCameraRestPoseRub(const GsSceneCamera& cam, float outPosition[3],
                         float outRotation[4]);

//! Focus depth from the cloud's MEDIAN DISPARITY: the median of 1/z over the
//! gaussians in front of the camera, inverted. Returns 0 when there are none.
//!
//! Disparity rather than depth because disparity is what a stereo pair
//! measures and what the eye fuses — but note the two agree exactly here:
//! 1/x is monotonic on z > 0, so median(1/z) == 1/median(z). The name is the
//! honest one for what the quantity IS.
//!
//! `centreWeighted` restricts the sample to the middle of the frame (see
//! GsRigFlags::centreWeightedFocus).
float GsMedianDisparityDepth(const std::vector<GsVertex>& vertices,
                             bool centreWeighted = false);

//! Drag -> scene orbit, radians. `dx`/`dy` are drag deltas as a FRACTION of the
//! canvas box, so a full-width drag is 2x the comfort cone and the gesture
//! reaches the cap in either direction from centre and no further.
//!
//! SIGN: the TURNTABLE convention — the scene follows the finger, as if you had
//! taken hold of the object. Dragging right turns the subject's near side to
//! the right. This is the opposite of the display rig's camera-orbit drag, and
//! it is deliberate (David, 2026-09-07: "it's better if we feel we move the
//! scene / object, more intuitive"); it is also the web SDK's own convention.
GsOrbit GsOrbitFromDrag(float dxFrac, float dyFrac);

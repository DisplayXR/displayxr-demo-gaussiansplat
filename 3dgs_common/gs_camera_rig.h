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
 *    `disp_median` columns. The `camera` block carries no such field, so the
 *    pivot comes from `--pivot=`, else the median forward depth of the cloud
 *    (the `dSubject` half of the same `min`), else a fallback. See
 *    GsResolveCameraRig.
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

//! Which rig frames the scene.
enum class GsRigKind { Display, Camera };

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
};

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

    //! Pivot / convergence depth, metres: the plane that stays put under head
    //! motion and the vertex of the comfort cone.
    float pivotM = kGsFallbackPivotM;

    //! Where the pivot came from, for the log and the HUD ("--pivot",
    //! "scene median depth", "scene bounds", "fallback").
    std::string pivotSource;

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
    //! NOT normalised against convergence: on the camera rig the ipd and the
    //! parallax are absolute scales, and folding 1/convergence into them (the
    //! trick that makes a display<->camera toggle disturbance-free) would make
    //! the stereo depth of a photo-lifted scene depend on where the pivot
    //! happened to land.
    float IpdScale() const;

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

//! Resolve the rig from what the file declared plus what the CLI overrode.
//!
//! `medianForwardDepthM` is the cloud's own median forward depth (<= 0 when
//! unknown); it stands in for `camera.ts`'s `dSubject`. `boundsForwardDepthM`
//! is a coarser fallback — the scene AABB's centre depth, which is all a
//! `--rig=camera` override on a non-photo scene can offer — and is likewise
//! <= 0 when unknown. Pivot priority: `--pivot=` > median > bounds > 2 m.
//!
//! Returns false (with `why` set, when non-null) when there are not enough
//! intrinsics to frame anything — the caller must then stay on the display rig
//! rather than invent a camera.
bool GsResolveCameraRig(const GsSceneCamera& cam,
                        const GsRigFlags& flags,
                        float medianForwardDepthM,
                        float boundsForwardDepthM,
                        GsCameraRig& out,
                        std::string* why = nullptr);

//! Which rig a scene should be framed with: the file's `camera` block decides,
//! `--rig=` overrides.
GsRigKind GsSelectRigKind(const GsSceneCamera& cam, const GsRigFlags& flags);

//! Median forward depth (-z, i.e. into the screen) of a righted RUB cloud, in
//! metres; 0 when the cloud is empty or entirely behind the camera. O(n) via
//! nth_element over a copy of one float per gaussian.
float GsMedianForwardDepth(const std::vector<GsVertex>& vertices);

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

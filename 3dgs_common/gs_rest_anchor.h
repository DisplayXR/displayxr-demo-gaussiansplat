// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Camera-rig REST anchor math, relative to the CANVAS (#162).
 *
 * XrCameraRigDXR measures the eye from the canvas centre ("the canvas-centre
 * eye (0, 0, N) maps here"). For a photo-lifted scene the rest view has to BE
 * the photograph, so the app cancels the rest eye's offset from the centre of
 * the window as it sits on the panel NOW — not its display-space position, and
 * not a value sampled once at launch. With the window in a corner of a panel the
 * two differ by tens of centimetres, and a later move or F11 left a one-shot
 * sample stale.
 *
 * Header-only and dependency-free so the platform legs (linux/main.cpp today;
 * windows/main.cpp carries the same arithmetic inline, PR #163) and the unit test
 * (gs_rest_anchor_test.cpp) share one definition.
 *
 * Conventions: display space is metres, origin at the panel centre, +x right,
 * +y up. Canvas rects are panel pixels, origin top-left, +y down.
 */

#pragma once

#include <cstdint>

//! Offset of the rest eye from the CURRENT canvas centre, metres.
//!
//! `restEyeX/Y` is the display-space eye centroid at rest (the untracked raw
//! eyes, or the display's nominal viewer before one was seen). The canvas rect
//! is XrViewDisplayRawDXR::canvasRectPx. Returns false (outputs untouched) when
//! the canvas or the display geometry is unknown.
inline bool GsRestOffsetFromCanvas(float restEyeX, float restEyeY,
                                   int32_t canvasX, int32_t canvasY,
                                   int32_t canvasW, int32_t canvasH,
                                   uint32_t displayPxW, uint32_t displayPxH,
                                   float displayWM, float displayHM,
                                   float* outOffX, float* outOffY,
                                   float* outCanvasCxM = nullptr, float* outCanvasCyM = nullptr)
{
    if (canvasW <= 0 || canvasH <= 0 || displayPxW == 0 || displayPxH == 0 ||
        !(displayWM > 0.0f) || !(displayHM > 0.0f)) {
        return false;
    }
    const float cxPx = (float)canvasX + 0.5f * (float)canvasW;
    const float cyPx = (float)canvasY + 0.5f * (float)canvasH;
    const float canvasCx = (cxPx / (float)displayPxW - 0.5f) * displayWM;
    const float canvasCy = (0.5f - cyPx / (float)displayPxH) * displayHM;
    if (outOffX) *outOffX = restEyeX - canvasCx;
    if (outOffY) *outOffY = restEyeY - canvasCy;
    if (outCanvasCxM) *outCanvasCxM = canvasCx;
    if (outCanvasCyM) *outCanvasCyM = canvasCy;
    return true;
}

//! Smallest |offset|^2 (m^2) a gain is measured from: below 1 cm the ratio is
//! noise, and the nominal/previous gain is kept.
constexpr float kGsRestGainMinOffset2 = 1.0e-4f;

//! The rig's two gains, measured from ONE untracked locate: how far it moved the
//! eye (`meanEyeLocalX/Y`, mean view position minus the declared camera, in the
//! camera's own frame) and how far it sheared the frustum (`meanTanU/V`, mean
//! frustum-centre tangent), each per metre of the offset (`offX/Y`). Means over
//! the views, so the per-eye stereo terms cancel. Projection onto the offset
//! direction. Returns false (outputs untouched) when the offset is too small.
inline bool GsRestGainsFromLocate(float offX, float offY,
                                  float meanEyeLocalX, float meanEyeLocalY,
                                  float meanTanU, float meanTanV,
                                  float* outPoseGain, float* outTanGain)
{
    const float off2 = offX * offX + offY * offY;
    if (!(off2 > kGsRestGainMinOffset2)) return false;
    if (outPoseGain) *outPoseGain = (meanEyeLocalX * offX + meanEyeLocalY * offY) / off2;
    if (outTanGain)  *outTanGain  = (meanTanU * offX + meanTanV * offY) / off2;
    return true;
}

//! The cancellation to apply this frame.
//!
//! `poseDelta` is added to the declared camera position IN THE CAMERA'S FRAME
//! (rotate by its orientation first); `tanDelta` is added to both horizontal /
//! both vertical fov tangents. Until a gain was measured (`gainsMeasured`
//! false) the pose gain is 1 and the frustum shears by the offset over the
//! convergence distance (`-convergenceDiopters`), which is what an off-axis
//! window at that distance does.
inline void GsRestCancellation(float offX, float offY, bool gainsMeasured,
                               float poseGain, float tanGain, float convergenceDiopters,
                               float outPoseDelta[2], float outTanDelta[2])
{
    const float pg = gainsMeasured ? poseGain : 1.0f;
    const float tg = gainsMeasured ? tanGain : -convergenceDiopters;
    outPoseDelta[0] = -pg * offX;
    outPoseDelta[1] = -pg * offY;
    outTanDelta[0] = -tg * offX;
    outTanDelta[1] = -tg * offY;
}

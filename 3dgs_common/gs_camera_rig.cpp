// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Camera-rig resolution + the `--rig=`/intrinsics flags. See
 *         gs_camera_rig.h for the model and its provenance.
 */

#include "gs_camera_rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

//! Strict float parse: the whole token must be consumed and finite.
bool ParseFloatStrict(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double d = strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    if (!std::isfinite(d)) return false;
    out = (float)d;
    return true;
}

bool ParseIntStrict(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size()) return false;
    if (v < 0 || v > 1000000) return false;
    out = (int)v;
    return true;
}

void Warn(std::vector<std::string>* warnings, const std::string& text) {
    if (warnings) warnings->push_back(text);
    fprintf(stderr, "gs_camera_rig: %s\n", text.c_str());
}

void Mat4Identity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

//! result = a * b, column-major (same convention as view_rig_math.h).
void Mat4Multiply(float* result, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            tmp[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    std::memcpy(result, tmp, 16 * sizeof(float));
}

void Mat4Translation(float* m, float tx, float ty, float tz) {
    Mat4Identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

//! Column-major rotation from a quaternion (xyzw).
void Mat4FromQuatXyzw(float* m, const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4Identity(m);
    m[0]  = 1 - 2 * (y * y + z * z);
    m[1]  = 2 * (x * y + z * w);
    m[2]  = 2 * (x * z - y * w);
    m[4]  = 2 * (x * y - z * w);
    m[5]  = 1 - 2 * (x * x + z * z);
    m[6]  = 2 * (y * z + x * w);
    m[8]  = 2 * (x * z + y * w);
    m[9]  = 2 * (y * z - x * w);
    m[10] = 1 - 2 * (x * x + y * y);
}

//! Inverse of a rigid transform whose rotation is `q` (xyzw) and translation
//! `t`: R^T and -R^T t.
void Mat4RigidInverse(float* m, const float q[4], const float t[3]) {
    const float inv[4] = {-q[0], -q[1], -q[2], q[3]};
    Mat4FromQuatXyzw(m, inv);
    // -R^T * t, with R^T already loaded above.
    const float tx = -(m[0] * t[0] + m[4] * t[1] + m[8]  * t[2]);
    const float ty = -(m[1] * t[0] + m[5] * t[1] + m[9]  * t[2]);
    const float tz = -(m[2] * t[0] + m[6] * t[1] + m[10] * t[2]);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

//! Yaw about +y then pitch about +x, column-major. Matches the turntable sense
//! documented on GsOrbitFromDrag.
void Mat4FromYawPitch(float* m, float yaw, float pitch) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    // R = Ry(yaw) * Rx(pitch)
    Mat4Identity(m);
    m[0] = cy;            m[4] = sy * sp;   m[8]  = sy * cp;
    m[1] = 0.0f;          m[5] = cp;        m[9]  = -sp;
    m[2] = -sy;           m[6] = cy * sp;   m[10] = cy * cp;
}

//! Convert a camera->world pose expressed in the block's OpenCV world into the
//! app's canonical RUB world.
//!
//! The loader rights the cloud with a half-turn about x (RDF -> RUB), which is
//! the involution F = diag(1, -1, -1). The SAME half-turn relates the two
//! camera conventions (OpenCV looks +z with +y down, the app looks -z with +y
//! up), so a pose (R, t) becomes (F R F, F t) — and an identity OpenCV pose is
//! an identity app pose, which is the whole point: for a single-camera lift the
//! rest camera is simply the origin looking down -z.
void OpenCvPoseToRub(const float posCv[3], const float quatCv[4],
                     float posOut[3], float quatOut[4]) {
    posOut[0] =  posCv[0];
    posOut[1] = -posCv[1];
    posOut[2] = -posCv[2];
    // Conjugating a quaternion by the half-turn about x, q_x = (1,0,0,0) xyzw:
    // (x, y, z, w) -> (x, -y, -z, w). (q_x q q_x flips the two axes that F
    // negates and leaves the x component and the scalar alone; the overall sign
    // is immaterial under the double cover.)
    quatOut[0] =  quatCv[0];
    quatOut[1] = -quatCv[1];
    quatOut[2] = -quatCv[2];
    quatOut[3] =  quatCv[3];
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Flags
// ─────────────────────────────────────────────────────────────────────────────

void GsParseRigFlags(int argc, const char* const* argv, GsRigFlags& out,
                     std::vector<std::string>* warnings) {
    out = GsRigFlags();
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        std::string tok(argv[i]);
        if (tok == "--") break;
        if (tok.rfind("--", 0) != 0) continue;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;  // bare flag: not one of ours
        const std::string key = tok.substr(2, eq - 2);
        const std::string val = tok.substr(eq + 1);

        if (key == "rig") {
            if (val == "display") { out.hasRig = true; out.rig = GsRigKind::Display; }
            else if (val == "camera") { out.hasRig = true; out.rig = GsRigKind::Camera; }
            else Warn(warnings, "--rig must be 'display' or 'camera'; ignored");
        } else if (key == "fx" || key == "fy" || key == "cx" || key == "cy") {
            float v = 0.0f;
            if (!ParseFloatStrict(val, v)) {
                Warn(warnings, "--" + key + " is not a number; ignored");
            } else if ((key == "fx" || key == "fy") && !(v > 0.0f)) {
                Warn(warnings, "--" + key + " must be positive; ignored");
            } else {
                if (key == "fx") { out.hasFx = true; out.fx = v; }
                if (key == "fy") { out.hasFy = true; out.fy = v; }
                if (key == "cx") { out.hasCx = true; out.cx = v; }
                if (key == "cy") { out.hasCy = true; out.cy = v; }
            }
        } else if (key == "size" || key == "window") {
            const size_t x = val.find_first_of("xX");
            int w = 0, h = 0;
            const bool isWindow = (key == "window");
            if (x == std::string::npos ||
                !ParseIntStrict(val.substr(0, x), w) ||
                !ParseIntStrict(val.substr(x + 1), h) || w <= 0 || h <= 0) {
                Warn(warnings, "--" + key + " must be WxH; ignored");
            } else if (isWindow && (w < 64 || h < 64 || w > 16384 || h > 16384)) {
                Warn(warnings, "--window must be between 64 and 16384 on each side; ignored");
            } else if (isWindow) {
                out.hasWindow = true; out.windowW = w; out.windowH = h;
            } else {
                out.hasSize = true; out.width = w; out.height = h;
            }
        } else if (key == "baseline") {
            float v = 0.0f;
            if (!ParseFloatStrict(val, v) || !(v > 0.0f) || v > 10.0f) {
                Warn(warnings, "--baseline must be a positive number of metres (<= 10); ignored");
            } else {
                out.hasBaseline = true; out.baselineM = v;
            }
        } else if (key == "fit") {
            if (val == "legacy") { out.hasFitMode = true; out.fitMode = GsFitMode::Legacy; }
            else if (val == "flood") { out.hasFitMode = true; out.fitMode = GsFitMode::Flood; }
            else if (val == "depth") { out.hasFitMode = true; out.fitMode = GsFitMode::Depth; }
            else Warn(warnings, "--fit must be 'legacy', 'flood' or 'depth'; ignored");
        } else if (key == "fit-disparity") {
            float v = 0.0f;
            // Above ~0.2 vH the "budget" is wider than the disparity of a
            // point at infinity and stops bounding anything; below 0 it is
            // meaningless. Refuse both rather than silently do nothing.
            if (!ParseFloatStrict(val, v) || !(v > 0.0f) || v > 0.5f) {
                Warn(warnings, "--fit-disparity must be a fraction of the display "
                               "height in (0, 0.5]; ignored");
            } else {
                out.hasFitDisparity = true; out.fitDisparityVH = v;
            }
        } else if (key == "focus-weight") {
            if (val == "centre" || val == "center") out.centreWeightedFocus = true;
            else if (val == "frame" || val == "off") out.centreWeightedFocus = false;
            else Warn(warnings, "--focus-weight must be 'centre' or 'frame'; ignored");
        } else if (key == "mode") {
            int v = 0;
            if (!ParseIntStrict(val, v) || v > 15) {
                Warn(warnings, "--mode must be a small rendering-mode index; ignored");
            } else {
                out.hasMode = true; out.mode = v;
            }
        } else if (key == "pivot") {
            float v = 0.0f;
            if (!ParseFloatStrict(val, v) || !(v > 0.0f)) {
                Warn(warnings, "--pivot must be a positive number of metres; ignored");
            } else {
                out.hasPivot = true;
                out.pivotM = Clampf(v, kGsPivotMinM, kGsPivotMaxM);
                if (out.pivotM != v)
                    Warn(warnings, "--pivot clamped into [0.1, 20] metres");
            }
        }
    }
}

bool GsIsAppOwnedFlagName(const std::string& name) {
    // Every flag GsParseRigFlags reads, plus the ones other parts of this
    // viewer read straight from argv. Kept as one list so a new flag has one
    // place to be declared rather than two.
    static const char* kOwned[] = {
        "rig", "fx", "fy", "cx", "cy", "size", "baseline", "pivot",
        "mode", "focus-weight", "window", "fit", "fit-disparity",
        "opaque",   // read by the macOS arm at startup; harmless elsewhere
    };
    for (const char* k : kOwned) if (name == k) return true;
    return false;
}

bool GsIsOwnFlagWarning(const std::string& warning) {
    // Both shapes quote the token; pull out whatever is between the quotes and
    // strip a leading "--" so the two forms compare against the same list.
    const size_t a = warning.find('\'');
    if (a == std::string::npos) return false;
    const size_t b = warning.find('\'', a + 1);
    if (b == std::string::npos) return false;
    std::string name = warning.substr(a + 1, b - a - 1);
    if (name.rfind("--", 0) == 0) name = name.substr(2);
    const size_t eq = name.find('=');
    if (eq != std::string::npos) name = name.substr(0, eq);
    return GsIsAppOwnedFlagName(name);
}

// ─────────────────────────────────────────────────────────────────────────────
// Resolution
// ─────────────────────────────────────────────────────────────────────────────

GsRigKind GsSelectRigKind(const GsSceneCamera& cam, const GsRigFlags& flags,
                          std::string* source, const GsPhotoLiftSignature* sig) {
    if (flags.hasRig) {
        if (source) *source = "--rig";
        return flags.rig;
    }
    if (cam.present && cam.hasRigHint) {
        if (source) *source = "camera.rig";
        return cam.rigHint;
    }
    if (cam.present) {
        if (source) *source = "block present";
        return GsRigKind::Camera;
    }
    // Nothing declared anything. A `.spz` or `.ply` conversion of a photo lift
    // carries no metadata at all, and framing it as an object gives it a crop
    // at the wrong field of view — so ask the cloud what it looks like.
    if (sig && sig->isPhotoLift) {
        if (source) *source = "auto: photo-lift signature";
        return GsRigKind::Camera;
    }
    if (source) *source = "no block";
    return GsRigKind::Display;
}

bool GsEstimateIntrinsics(const std::vector<GsVertex>& vertices,
                          GsIntrinsicsEstimate& out) {
    out = GsIntrinsicsEstimate();
    if (vertices.empty()) { out.note = "empty cloud"; return false; }

    // Half-tangents about the rest camera, for everything in front of it.
    // 0.05 m rather than 0 because a gaussian a millimetre from the lens
    // produces an enormous tangent from a rounding error.
    std::vector<float> tx, ty;
    tx.reserve(vertices.size());
    ty.reserve(vertices.size());
    for (const GsVertex& v : vertices) {
        const float z = -v.position[2];            // app +z is BACK
        if (!(z > 0.05f) || !std::isfinite(z)) continue;
        const float a = v.position[0] / z, b = v.position[1] / z;
        if (std::isfinite(a) && std::isfinite(b)) { tx.push_back(a); ty.push_back(b); }
    }
    if (tx.size() < 1000) {
        out.note = "too few gaussians in front of the camera (" +
                   std::to_string(tx.size()) + ")";
        return false;
    }

    // P1/P99, not the extremes: a lift always leaves strays far outside the
    // frame and any one of them would set the focal by itself.
    auto pct = [](std::vector<float>& v, double p) {
        const size_t k = (size_t)(p * (double)(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
        return v[k];
    };
    out.tanLeft  = pct(tx, 0.01);
    out.tanRight = pct(tx, 0.99);
    out.tanDown  = pct(ty, 0.01);
    out.tanUp    = pct(ty, 0.99);

    float halfX = 0.5f * (out.tanRight - out.tanLeft);
    float halfY = 0.5f * (out.tanUp - out.tanDown);
    if (!(halfX > 1.0e-4f) || !(halfY > 1.0e-4f)) {
        out.note = "degenerate angular extent";
        return false;
    }

    // The aspect is trusted even when the focal is not: a cloud's shape says
    // what orientation the photograph was, whatever its long tail does to the
    // scale.
    const float aspect = halfX / halfY;

    // 36 mm is the full-frame width, so the 35 mm-equivalent focal is
    // 18 mm / tan(hfov/2). This is the number the gate is expressed in
    // because it is the one a photographer can sanity-check by eye.
    out.focal35mm = 18.0f / halfX;
    if (!(out.focal35mm >= kGsEstMinFocal35) || !(out.focal35mm <= kGsEstMaxFocal35)) {
        out.usedFallback = true;
        out.note = "implied " + std::to_string((int)(out.focal35mm + 0.5f)) +
                   "mm-eq is outside [" + std::to_string((int)kGsEstMinFocal35) + ", " +
                   std::to_string((int)kGsEstMaxFocal35) + "] — using " +
                   std::to_string((int)kGsFallbackFocal35) + "mm-eq";
        const float newHalfX = 18.0f / kGsFallbackFocal35;
        const float k = newHalfX / halfX;
        // Scale the window about its own centre so the measured asymmetry —
        // the principal-point offset — survives the substitution.
        const float cxTan = 0.5f * (out.tanRight + out.tanLeft);
        const float cyTan = 0.5f * (out.tanUp + out.tanDown);
        out.tanLeft  = cxTan + (out.tanLeft  - cxTan) * k;
        out.tanRight = cxTan + (out.tanRight - cxTan) * k;
        out.tanDown  = cyTan + (out.tanDown  - cyTan) * k;
        out.tanUp    = cyTan + (out.tanUp    - cyTan) * k;
        halfX = newHalfX;
        halfY = halfX / aspect;
        out.focal35mm = kGsFallbackFocal35;
    }

    // Nominal pixel dims. They carry no information the tangents do not — the
    // block is specified in pixels, so the estimate has to be expressed in
    // some — but the ASPECT is the measured one, which is what makes a
    // portrait come out portrait.
    if (aspect >= 1.0f) {
        out.width = 2048;
        out.height = (int)(2048.0f / aspect + 0.5f);
    } else {
        out.height = 2048;
        out.width = (int)(2048.0f * aspect + 0.5f);
    }
    if (out.width < 2) out.width = 2;
    if (out.height < 2) out.height = 2;

    out.fx = (float)out.width / (out.tanRight - out.tanLeft);
    out.fy = (float)out.height / (out.tanUp - out.tanDown);
    // u = cx + fx*tx maps tanLeft -> 0, so cx = -fx*tanLeft. OpenCV's +y is
    // DOWN while the app's is up, so v = cy - fy*ty maps tanUp -> 0.
    out.cx = -out.fx * out.tanLeft;
    out.cy = out.fy * out.tanUp;
    out.valid = true;
    if (out.note.empty())
        out.note = std::to_string((int)(out.focal35mm + 0.5f)) + "mm-eq";
    return true;
}

GsSceneMeasurements GsMeasureScene(const std::vector<GsVertex>& vertices) {
    GsSceneMeasurements m;
    if (vertices.empty()) return m;
    GsEstimateIntrinsics(vertices, m.estimate);
    m.medianDepthM       = GsMedianDisparityDepth(vertices, /*centreWeighted=*/false);
    m.medianDepthCentreM = GsMedianDisparityDepth(vertices, /*centreWeighted=*/true);
    {
        size_t inFront = 0;
        for (const GsVertex& v : vertices) if (-v.position[2] > 0.05f) inFront++;
        m.forwardFraction = (float)((double)inFront / (double)vertices.size());
    }
    m.valid = m.estimate.valid || m.medianDepthM > 0.0f;
    return m;
}

float GsMedianDisparityDepth(const std::vector<GsVertex>& vertices, bool centreWeighted) {
    if (vertices.empty()) return 0.0f;

    // Centre weighting needs the frame's own extent to know what "centre"
    // means, so measure it first. Without it, every gaussian counts.
    float halfX = 0.0f, halfY = 0.0f, cxTan = 0.0f, cyTan = 0.0f;
    if (centreWeighted) {
        GsIntrinsicsEstimate e;
        if (GsEstimateIntrinsics(vertices, e) && e.valid) {
            halfX = 0.5f * (e.tanRight - e.tanLeft);
            halfY = 0.5f * (e.tanUp - e.tanDown);
            cxTan = 0.5f * (e.tanRight + e.tanLeft);
            cyTan = 0.5f * (e.tanUp + e.tanDown);
        } else {
            centreWeighted = false;  // nothing to be central to
        }
    }

    std::vector<float> disp;
    disp.reserve(vertices.size());
    for (const GsVertex& v : vertices) {
        const float z = -v.position[2];
        if (!(z > 0.05f) || !std::isfinite(z)) continue;
        if (centreWeighted) {
            const float a = v.position[0] / z - cxTan;
            const float b = v.position[1] / z - cyTan;
            // The middle half of the frame on each axis.
            if (std::fabs(a) > 0.5f * halfX || std::fabs(b) > 0.5f * halfY) continue;
        }
        disp.push_back(1.0f / z);
    }
    if (disp.empty()) return 0.0f;
    const size_t mid = disp.size() / 2;
    std::nth_element(disp.begin(), disp.begin() + (ptrdiff_t)mid, disp.end());
    const float m = disp[mid];
    return (m > 1.0e-6f) ? (1.0f / m) : 0.0f;
}

void GsCameraRestPoseRub(const GsSceneCamera& cam, float outPosition[3],
                         float outRotation[4]) {
    OpenCvPoseToRub(cam.restPosition, cam.restRotation, outPosition, outRotation);
}

void GsCameraRig::SetFocusLocal(float x, float y, float z) {
    focusLocal[0] = x;
    focusLocal[1] = y;
    // A focus at or behind the camera has no convergence plane; keep it in
    // front and inside the sane range, so nothing downstream divides by it.
    const float depth = Clampf(-z, kGsPivotMinM, kGsPivotMaxM);
    focusLocal[2] = -depth;
    pivotM = depth;
}

GsPhotoLiftSignature GsDetectPhotoLift(const GsSceneMeasurements& m,
                                       const GsFitBounds& bounds) {
    GsPhotoLiftSignature s;
    s.forwardFraction = m.forwardFraction;
    s.focal35mm = m.estimate.focal35mm;
    // A lens, not a fallback: `usedFallback` means the measured focal fell
    // outside 14-85 mm-eq and a nominal 28 was substituted, which is precisely
    // the case where the cloud's angular extent is NOT telling us about a lens.
    s.focalGatePassed = m.estimate.valid && !m.estimate.usedFallback;

    float blobMax = 0.0f;
    if (bounds.valid) {
        for (int a = 0; a < 3; a++) blobMax = std::max(blobMax, bounds.extent[a]);
        // Distance from the origin to the blob's box; zero when inside it.
        double d2 = 0.0;
        for (int a = 0; a < 3; a++) {
            const double half = 0.5 * (double)bounds.extent[a];
            const double delta = std::fabs((double)bounds.center[a]) - half;
            if (delta > 0.0) d2 += delta * delta;
        }
        if (blobMax > 1.0e-9f)
            s.originOutsideRatio = (float)(std::sqrt(d2) / (double)blobMax);
    }

    // All three, in the order that makes a failure cheapest to read: the
    // decisive one first.
    if (!(s.forwardFraction >= kGsPhotoLiftMinForwardFrac)) {
        s.failedTerm = "not all in front of the origin";
    } else if (!s.focalGatePassed) {
        s.failedTerm = "angular extent is not a plausible lens";
    } else if (!bounds.valid) {
        s.failedTerm = "no object blob to stand outside of";
    } else if (!(s.originOutsideRatio >= kGsPhotoLiftMinOriginOutside)) {
        s.failedTerm = "camera sits inside the subject";
    } else {
        s.isPhotoLift = true;
    }
    return s;
}

bool GsResolveCameraRig(const GsSceneCamera& cam,
                        const GsRigFlags& flags,
                        const GsRigResolveInput& in,
                        GsCameraRig& out,
                        std::string* why) {
    out = GsCameraRig();

    // The cloud's own frustum is carried even when the block declares
    // intrinsics, so the log can say whether the two agree. A disagreement is
    // exactly a wrong zoom, and it is silent otherwise.
    const bool haveCloud = in.measurements && in.measurements->valid;
    if (haveCloud) out.estimate = in.measurements->estimate;

    // ── Intrinsics: block -> flags -> estimate -> nominal ────────────────
    float fx = 0.0f, fy = 0.0f, cx = 0.0f, cy = 0.0f;
    int   w = 0, h = 0;
    if (cam.present) {
        fx = cam.fx; fy = cam.fy; cx = cam.cx; cy = cam.cy;
        w = cam.width; h = cam.height;
        out.intrinsicsSource = "block";
    }
    if ((!(fx > 0.0f) || !(fy > 0.0f) || w <= 0 || h <= 0) && out.estimate.valid) {
        // Nothing declared them, so recover them from the cloud. This is what
        // lets a `.sog` whose block omits intrinsics — or carries none at all —
        // still be framed as the photograph it is, instead of auto-fitted like
        // an object.
        fx = out.estimate.fx; fy = out.estimate.fy;
        w = out.estimate.width; h = out.estimate.height;
        // The SCALE is measured; the PRINCIPAL POINT is centred, and that is a
        // measurement result, not a shortcut. The estimator's window asymmetry
        // is real but it is not the lens: a lifted cloud spans the union of
        // what both cameras saw plus whatever the model extrapolated, so its
        // angular centre drifts off the optical axis by more than the true
        // principal-point offset and in an unrelated direction.
        //
        // Measured on the harbour asset, grey MAE against the two photographs:
        //   estimate, asymmetry applied   32.5 / 29.3
        //   estimate, point centred       23.6 / 23.6
        //   the block's own intrinsics    23.6 / 23.5
        // So the asymmetry costs about seven points and centring reproduces
        // the declared camera to within a tenth of one. The half-tangents, by
        // contrast, land within 0.2% and are worth trusting. Trust the extent,
        // centre the point.
        cx = 0.5f * (float)w;
        cy = 0.5f * (float)h;
        out.intrinsicsSource = out.estimate.usedFallback
                                   ? "estimated (28mm fallback)" : "estimated";
    }
    // Flags outrank both the block and the estimate, and do it field by field:
    // --cx alone is a legitimate correction to an otherwise good camera, not a
    // request to discard the rest of it. Applied LAST for that reason.
    if (flags.hasSize) { w = flags.width; h = flags.height; }
    if (flags.hasFx) fx = flags.fx;
    if (flags.hasFy) fy = flags.fy;
    if (fx > 0.0f && !(fy > 0.0f)) fy = fx;   // square pixels are the norm
    if (fy > 0.0f && !(fx > 0.0f)) fx = fy;
    if (flags.hasSize && !flags.hasCx && out.intrinsicsSource.empty()) cx = 0.5f * (float)w;
    if (flags.hasSize && !flags.hasCy && out.intrinsicsSource.empty()) cy = 0.5f * (float)h;
    if (flags.hasCx) cx = flags.cx;
    if (flags.hasCy) cy = flags.cy;
    if (flags.hasFx || flags.hasFy || flags.hasSize || flags.hasCx || flags.hasCy) {
        out.intrinsicsSource = out.intrinsicsSource.empty()
                                   ? "flags" : (out.intrinsicsSource + " + flags");
    }
    if (!(fx > 0.0f) || !(fy > 0.0f) || w <= 0 || h <= 0) {
        if (why)
            *why = "no camera intrinsics and none recoverable from the cloud "
                   "(need a `camera` block, or --fx/--fy and --size=WxH)";
        return false;
    }

    out.fx = fx; out.fy = fy; out.cx = cx; out.cy = cy;
    out.width = w; out.height = h;

    out.baselineM = flags.hasBaseline ? flags.baselineM
                  : (cam.hasStereo ? cam.baselineM : kGsDefaultBaselineM);

    out.dxrIpdFactor      = cam.hasDxrIpd      ? cam.dxrIpdFactor      : 1.0f;
    out.dxrParallaxFactor = cam.hasDxrParallax ? cam.dxrParallaxFactor : 1.0f;

    if (cam.present) {
        OpenCvPoseToRub(cam.restPosition, cam.restRotation, out.restPosition,
                        out.restRotation);
    }

    // ── Focus: --pivot -> block -> median disparity -> bounds -> 2 m ─────
    //
    // The gallery's model takes min(dConv, dSubject) — the nearer of the
    // stored convergence plane and the median scene point — because the focus
    // is both the plane that stays put and the vertex of the comfort cone, and
    // both want it on or in front of the subject. `camera.focus.point` is that
    // decision, made by the producer, which is why it outranks anything this
    // viewer can measure.
    if (flags.hasPivot) {
        out.SetFocusLocal(0.0f, 0.0f, -flags.pivotM);
        out.focusSource = "--pivot";
    } else if (cam.present && cam.hasFocus) {
        // The block's point is OpenCV (+y down, +z forward); the cloud has
        // already been righted to RUB, so the focus must take the same
        // half-turn about x or it will sit mirrored behind the camera.
        out.SetFocusLocal(cam.focusPoint[0], -cam.focusPoint[1], -cam.focusPoint[2]);
        out.focusSource = cam.focusSourceLabel.empty()
                              ? "block" : ("block (" + cam.focusSourceLabel + ")");
    } else if (haveCloud) {
        const float d = flags.centreWeightedFocus ? in.measurements->medianDepthCentreM
                                                  : in.measurements->medianDepthM;
        if (d > 0.0f) {
            out.SetFocusLocal(0.0f, 0.0f, -d);
            out.focusSource = flags.centreWeightedFocus ? "median-disparity (centre)"
                                                        : "median-disparity";
        }
    }
    if (out.focusSource.empty()) {
        if (in.boundsForwardDepthM > 0.0f) {
            out.SetFocusLocal(0.0f, 0.0f, -in.boundsForwardDepthM);
            out.focusSource = "scene bounds";
        } else {
            out.SetFocusLocal(0.0f, 0.0f, -kGsFallbackPivotM);
            out.focusSource = "fallback";
        }
    }

    out.valid = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Derived quantities
// ─────────────────────────────────────────────────────────────────────────────

float GsCameraRig::VerticalFovRad(float canvasAspect) const {
    const float tv = TanHalfPhotoH();
    const float th = TanHalfPhotoW();
    if (!(tv > 0.0f)) return 0.0f;
    float t = tv;
    if (canvasAspect > 1.0e-4f && th > 0.0f) t = std::min(tv, th / canvasAspect);
    return 2.0f * std::atan(t);
}

float GsCameraRig::ConvergenceDiopters() const {
    return (pivotM > 1.0e-4f) ? (1.0f / pivotM) : 0.0f;
}

float GsCameraRig::IpdScale(float measuredEyeSeparationM) const {
    const float base = (measuredEyeSeparationM > 1.0e-4f) ? measuredEyeSeparationM
                                                          : kGsNominalHumanIpdM;
    return baselineM / base;
}

void GsCameraRig::RigPose(bool monoView, float outPosition[3], float outRotation[4]) const {
    for (int i = 0; i < 4; i++) outRotation[i] = restRotation[i];
    // Half a baseline along the camera's own +x (its right), so the pair of
    // views the runtime straddles this pose with lands on the captured pair.
    // A mono mode has no pair, so it stays on the left camera = the photo.
    const float half = monoView ? 0.0f : (0.5f * baselineM);
    float R[16];
    Mat4FromQuatXyzw(R, restRotation);
    outPosition[0] = restPosition[0] + R[0] * half;
    outPosition[1] = restPosition[1] + R[1] * half;
    outPosition[2] = restPosition[2] + R[2] * half;
}

void GsCameraRig::PrincipalShiftTan(float& du, float& dv) const {
    du = 0.0f;
    dv = 0.0f;
    if (!valid) return;
    // An off-centre principal point shifts the image-plane window, which in
    // tangent space is a constant added to both edges of the axis. OpenCV's
    // +y is down and the app's is up, hence the negation on dv.
    if (fx > 0.0f) du = (0.5f * (float)width  - cx) / fx;
    if (fy > 0.0f) dv = -(0.5f * (float)height - cy) / fy;
}

void GsCameraRig::SceneOrbitMatrix(float yawRad, float pitchRad, float out[16]) const {
    // Rotate the scene about THE FOCUS POINT: T(f) * R * T(-f) — translate the
    // focus to the origin, turn, put it back. Straight ahead at -pivot on z
    // until a double-click moves it somewhere off-axis, which is exactly the
    // case the general form exists for. A non-identity rest pose is conjugated
    // back through it so the focus stays the point the camera is looking at.
    float local[16], rot[16], toPivot[16], fromPivot[16];
    Mat4Translation(toPivot, focusLocal[0], focusLocal[1], focusLocal[2]);
    Mat4Translation(fromPivot, -focusLocal[0], -focusLocal[1], -focusLocal[2]);
    Mat4FromYawPitch(rot, yawRad, pitchRad);
    Mat4Multiply(local, rot, fromPivot);
    Mat4Multiply(local, toPivot, local);

    // Conjugate through the RIG pose (the head centre), not `rest`: the pivot
    // is the point straight ahead of the camera the frustum is built around.
    float rigPos[3], rigRot[4];
    RigPose(/*monoView=*/false, rigPos, rigRot);
    const bool identityRig =
        rigPos[0] == 0.0f && rigPos[1] == 0.0f && rigPos[2] == 0.0f &&
        rigRot[0] == 0.0f && rigRot[1] == 0.0f && rigRot[2] == 0.0f &&
        std::fabs(rigRot[3]) == 1.0f;
    if (identityRig) {
        std::memcpy(out, local, 16 * sizeof(float));
        return;
    }
    float P[16], Pinv[16], tmp[16];
    Mat4FromQuatXyzw(P, rigRot);
    P[12] = rigPos[0]; P[13] = rigPos[1]; P[14] = rigPos[2];
    Mat4RigidInverse(Pinv, rigRot, rigPos);
    Mat4Multiply(tmp, local, Pinv);
    Mat4Multiply(out, P, tmp);
}

void GsCameraRig::ClampOrbit(float& yawRad, float& pitchRad) const {
    const float cap = kGsRigMaxAngleDeg * 3.14159265358979323846f / 180.0f;
    yawRad   = Clampf(yawRad, -cap, cap);
    pitchRad = Clampf(pitchRad, -cap, cap);
}

GsOrbit GsOrbitFromDrag(float dxFrac, float dyFrac) {
    const float cap  = kGsRigMaxAngleDeg * 3.14159265358979323846f / 180.0f;
    const float gain = 2.0f * cap;
    GsOrbit o;
    o.yaw   = Clampf(dxFrac * gain, -cap, cap);
    o.pitch = Clampf(dyFrac * gain, -cap, cap);
    return o;
}

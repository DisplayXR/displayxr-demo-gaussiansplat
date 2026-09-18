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
        } else if (key == "size") {
            const size_t x = val.find_first_of("xX");
            int w = 0, h = 0;
            if (x == std::string::npos ||
                !ParseIntStrict(val.substr(0, x), w) ||
                !ParseIntStrict(val.substr(x + 1), h) || w <= 0 || h <= 0) {
                Warn(warnings, "--size must be WxH in pixels; ignored");
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

// ─────────────────────────────────────────────────────────────────────────────
// Resolution
// ─────────────────────────────────────────────────────────────────────────────

GsRigKind GsSelectRigKind(const GsSceneCamera& cam, const GsRigFlags& flags) {
    if (flags.hasRig) return flags.rig;
    return cam.present ? GsRigKind::Camera : GsRigKind::Display;
}

float GsMedianForwardDepth(const std::vector<GsVertex>& vertices) {
    if (vertices.empty()) return 0.0f;
    std::vector<float> depth;
    depth.reserve(vertices.size());
    for (const GsVertex& v : vertices) {
        // RUB: +z is back, so forward depth is -z. Gaussians behind the camera
        // (a lift always leaves a few) carry no information about where the
        // subject is and would drag the median toward zero.
        const float d = -v.position[2];
        if (d > 0.0f && std::isfinite(d)) depth.push_back(d);
    }
    if (depth.empty()) return 0.0f;
    const size_t mid = depth.size() / 2;
    std::nth_element(depth.begin(), depth.begin() + (ptrdiff_t)mid, depth.end());
    return depth[mid];
}

bool GsResolveCameraRig(const GsSceneCamera& cam,
                        const GsRigFlags& flags,
                        float medianForwardDepthM,
                        float boundsForwardDepthM,
                        GsCameraRig& out,
                        std::string* why) {
    out = GsCameraRig();

    // Intrinsics: the file first, the CLI on top. A forced --rig=camera on a
    // scene with no block is legal, but only if the flags supply everything.
    float fx = cam.present ? cam.fx : 0.0f;
    float fy = cam.present ? cam.fy : 0.0f;
    int   w  = cam.present ? cam.width  : 0;
    int   h  = cam.present ? cam.height : 0;
    if (flags.hasFx) fx = flags.fx;
    if (flags.hasFy) fy = flags.fy;
    if (flags.hasSize) { w = flags.width; h = flags.height; }
    // A square-pixel camera is the norm; accept one focal for both.
    if (fx > 0.0f && !(fy > 0.0f)) fy = fx;
    if (fy > 0.0f && !(fx > 0.0f)) fx = fy;

    if (!(fx > 0.0f) || !(fy > 0.0f) || w <= 0 || h <= 0) {
        if (why)
            *why = "no camera intrinsics (need a `camera` block, or --fx/--fy and --size=WxH)";
        return false;
    }

    out.fx = fx;
    out.fy = fy;
    out.width = w;
    out.height = h;
    // Principal point: the file's, else the CLI's, else centred.
    out.cx = flags.hasCx ? flags.cx : (cam.present ? cam.cx : 0.5f * (float)w);
    out.cy = flags.hasCy ? flags.cy : (cam.present ? cam.cy : 0.5f * (float)h);
    if (!cam.present && !flags.hasCx) out.cx = 0.5f * (float)w;
    if (!cam.present && !flags.hasCy) out.cy = 0.5f * (float)h;

    out.baselineM = flags.hasBaseline ? flags.baselineM
                  : (cam.hasStereo ? cam.baselineM : kGsDefaultBaselineM);

    // Pivot. `camera.ts` takes min(dConv, dSubject) — the nearer of the stored
    // convergence plane and the median scene point — because the pivot is both
    // the plane that stays put and the vertex of the comfort cone, and both
    // want it on or in front of the subject. The `camera` block carries no
    // convergence, so dConv is unknown (effectively infinite) and the min
    // degenerates to dSubject: the cloud's own median forward depth.
    if (flags.hasPivot) {
        out.pivotM = flags.pivotM;
        out.pivotSource = "--pivot";
    } else if (medianForwardDepthM > 0.0f) {
        out.pivotM = Clampf(medianForwardDepthM, kGsPivotMinM, kGsPivotMaxM);
        out.pivotSource = "scene median depth";
    } else if (boundsForwardDepthM > 0.0f) {
        out.pivotM = Clampf(boundsForwardDepthM, kGsPivotMinM, kGsPivotMaxM);
        out.pivotSource = "scene bounds";
    } else {
        out.pivotM = kGsFallbackPivotM;
        out.pivotSource = "fallback";
    }

    if (cam.present) {
        OpenCvPoseToRub(cam.restPosition, cam.restRotation, out.restPosition,
                        out.restRotation);
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

float GsCameraRig::IpdScale() const {
    return baselineM / kGsNominalHumanIpdM;
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
    // In the rig's own frame the pivot sits straight ahead at -pivot on z, so
    // the rotation is T(0,0,-d) * R * T(0,0,+d): translate the pivot to the
    // origin, turn, put it back. A non-identity rest pose is conjugated back
    // through it so the pivot stays the point the camera is actually looking at.
    float local[16], rot[16], toPivot[16], fromPivot[16];
    Mat4Translation(toPivot, 0.0f, 0.0f, -pivotM);
    Mat4Translation(fromPivot, 0.0f, 0.0f, pivotM);
    Mat4FromYawPitch(rot, yawRad, pitchRad);
    Mat4Multiply(local, rot, fromPivot);
    Mat4Multiply(local, toPivot, local);

    const bool identityRest =
        restPosition[0] == 0.0f && restPosition[1] == 0.0f && restPosition[2] == 0.0f &&
        restRotation[0] == 0.0f && restRotation[1] == 0.0f && restRotation[2] == 0.0f &&
        std::fabs(restRotation[3]) == 1.0f;
    if (identityRest) {
        std::memcpy(out, local, 16 * sizeof(float));
        return;
    }
    float P[16], Pinv[16], tmp[16];
    Mat4FromQuatXyzw(P, restRotation);
    P[12] = restPosition[0]; P[13] = restPosition[1]; P[14] = restPosition[2];
    Mat4RigidInverse(Pinv, restRotation, restPosition);
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

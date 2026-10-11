// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The splat viewer's adoption of the depth-aware cursor (XR_DXR_cursor_depth).
 *
 * The runtime DRAWS the cursor (ADR-046 Amendment 2): an arrow lifted just in
 * front of whatever pops out of the glass under the pointer, and nothing at
 * all over content on or behind the glass (the OS cursor stays the cursor
 * there). The app owns only the depth under the pointer. displayxr-common's
 * `dxr::CursorDepth` does the bookkeeping; this header is the part every
 * desktop leg of THIS viewer shares on top of it:
 *
 *   - the depth source: the renderer's `GsSplatRayCaster` (3dgs_common/
 *     gs_raycast.h) — the first point along the ray where the splats'
 *     front-to-back composite turns opaque, i.e. the surface the viewer sees,
 *     not the nearest splat centre. The cloud is the only thing in the scene
 *     that can stick out of the glass (every UI surface is 2D).
 *   - the locate-space -> cloud-space mapping and the renderer's NEAR cull,
 *     both refreshed per frame by the leg in `CursorSceneState`: splats in
 *     front of the near plane are culled from the picture, so they must not
 *     catch the cursor either. (No far window: content behind the glass never
 *     gets a drawn cursor — the runtime decides that — so a far cull changes
 *     nothing the user can see.)
 *   - `DISPLAYXR_CURSOR_DEPTH=0` opts out (the extension is then not enabled
 *     at all and the renderer builds no ray-cast structure).
 *   - `DISPLAYXR_CURSOR_DEPTH_UV=u,v` scripts the pointer (canvas UV, origin
 *     top-left) for headless verification; the leg then ignores real motion.
 *   - a throttled cost line: the ray cast's wall time per frame.
 *
 * Per leg (see macos/main.mm, linux/main.cpp, windows/main.cpp):
 *
 *     // instance: enable XR_DXR_cursor_depth if present && CursorDepthWanted()
 *     renderer.setCursorRayCast(specVersion > 0);   // BEFORE the first loadScene()
 *     gsdemo::CursorDepthAdoption cursorAdopt;
 *     cursorAdopt.setup(cursor, renderer.rayCaster(), specVersion);
 *     cursor.setWindowCocoa(view);                  // or setWindowWin32 / the manual path
 *     // per frame, around xrLocateViews:
 *     cursor.chain(locateInfo, viewState);
 *     xrLocateViews(...);
 *     cursor.afterLocate(views, activeViewCount);
 *     cursorAdopt.state() = {...};                  // this frame's rig / clip / model transform
 *     if (cursorAdopt.endFrame(cursor)) LOG_INFO("%s", cursorAdopt.summary());
 */

#pragma once

#include "cursor_depth.h"   // displayxr-common v2.29.0+
#include "gs_raycast.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gsdemo {

//! False when DISPLAYXR_CURSOR_DEPTH=0: do not enable the extension.
inline bool CursorDepthWanted()
{
    const char* e = std::getenv("DISPLAYXR_CURSOR_DEPTH");
    return !(e != nullptr && std::strcmp(e, "0") == 0);
}

/*!
 * What the ray cast needs to know about the frame the rays come from. The
 * rays are fired from the PREVIOUS locate's views (dxr::CursorDepth::chain),
 * so the leg writes this once per frame after it has resolved its rig, and
 * the next frame's chain() reads it — the two always describe the same frame.
 */
struct CursorSceneState
{
    bool valid = false;
    //! View forward, unit, in the locate space (the rig pose's -Z).
    float forward[3] = {0.0f, 0.0f, -1.0f};
    //! Display rig: the renderer's near plane sits vH in front of the ZDP,
    //! i.e. at view depth (dot(displayCenter - eye, forward) - vH) per eye.
    bool displayRig = true;
    float displayCenter[3] = {0.0f, 0.0f, 0.0f};
    float vH = 0.0f;
    //! Camera rig: a fixed view-depth near plane, metres.
    float nearDepth = 0.0f;
    //! Column-major cloud-from-locate affine transform (the inverse of the
    //! model matrix the splats are rendered with); identity when none.
    bool hasModel = false;
    float cloudFromLocate[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

//! Column-major inverse of an affine 4x4 (rotation/scale + translation).
inline bool InvertAffine(const float m[16], float out[16])
{
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-20f)) return false;
    const float r = 1.0f / det;
    float inv[9] = {A * r, -(b * i - c * h) * r, (b * f - c * e) * r,
                    B * r, (a * i - c * g) * r, -(a * f - c * d) * r,
                    C * r, -(a * h - b * g) * r, (a * e - b * d) * r};   // row-major
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) out[col * 4 + row] = inv[row * 3 + col];
    for (int row = 0; row < 3; row++)
        out[12 + row] = -(out[row] * m[12] + out[4 + row] * m[13] + out[8 + row] * m[14]);
    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
    return true;
}

class CursorDepthAdoption
{
public:
    //! enable(specVersion) (0 = extension not enabled: the helper stays inert),
    //! the splat ray cast, and the scripted pointer. The window adapter stays
    //! with the leg (it is the only platform-specific part).
    void setup(dxr::CursorDepth& cursor, const GsSplatRayCaster& rayCaster, uint32_t specVersion)
    {
        cursor.enable(specVersion);
        const GsSplatRayCaster* rc = &rayCaster;
        CursorDepthAdoption* self = this;
        cursor.setRayCast([rc, self](const dxr::CursorRay& ray) -> std::optional<float> {
            const auto t0 = std::chrono::steady_clock::now();
            float t = 0.0f;
            const bool hit = self->cast(*rc, ray, t);
            self->stats_.frameUs +=
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            self->stats_.frameRays++;
            return hit ? std::optional<float>(t) : std::nullopt;
        });
        scripted_ = false;
        if (const char* uv = std::getenv("DISPLAYXR_CURSOR_DEPTH_UV")) {
            float u = 0.0f, v = 0.0f;
            if (std::sscanf(uv, "%f,%f", &u, &v) == 2) {
                cursor.setPointerUV(u, v);
                cursor.setWantPlacement(true);   // the placement goes into the summary
                scripted_ = true;
            }
        }
    }

    //! This frame's rig / clip / model transform (see CursorSceneState).
    CursorSceneState& state() { return state_; }

    //! The pointer is scripted (DISPLAYXR_CURSOR_DEPTH_UV): ignore real motion.
    bool scripted() const { return scripted_; }

    //! Call once per frame after afterLocate(). True when a cost summary is
    //! ready (every 600 frames that ran the ray cast); read it with summary().
    bool endFrame(const dxr::CursorDepth& cursor)
    {
        if (stats_.frameRays > 0) {
            stats_.frames++;
            stats_.sumUs += stats_.frameUs;
            stats_.sumRays += stats_.frameRays;
            if (stats_.frameUs > stats_.maxUs) stats_.maxUs = stats_.frameUs;
            if (cursor.hint().hasContent) stats_.hitFrames++;
            if (cursor.drawState().willDraw) stats_.drawFrames++;
        }
        stats_.frameUs = 0.0;
        stats_.frameRays = 0;
        if (stats_.frames < kSummaryFrames) return false;
        std::snprintf(summary_, sizeof(summary_),
                      "cursor depth: ray cast %.1f us/frame mean, %.1f us max, %.1f rays/frame "
                      "over %u frames (hit %u, runtime drew %u)",
                      stats_.sumUs / stats_.frames, stats_.maxUs, (double)stats_.sumRays / stats_.frames,
                      stats_.frames, stats_.hitFrames, stats_.drawFrames);
        if (scripted_) {
            const XrCursorDepthPlacementDXR& pl = cursor.placement();
            const XrCursorDepthHintDXR& hn = cursor.hint();
            // How far the hit sits in FRONT of the display plane, in vH (display
            // rig only; > 0 pops out, which is where a cursor may be drawn).
            float pop = 0.0f;
            const CursorSceneState& s = state_;
            if (hn.hasContent && s.displayRig && s.vH > 0.0f)
                pop = -((hn.nearestPoint.x - s.displayCenter[0]) * s.forward[0] +
                        (hn.nearestPoint.y - s.displayCenter[1]) * s.forward[1] +
                        (hn.nearestPoint.z - s.displayCenter[2]) * s.forward[2]) / s.vH;
            const size_t n = std::strlen(summary_);
            std::snprintf(summary_ + n, sizeof(summary_) - n,
                          "; nearest=(%.4f, %.4f, %.4f) pop=%+.3f vH; placement active=%d disparity=%.3f "
                          "target=%.3f pos=(%.4f, %.4f, %.4f) h=%.4f",
                          hn.nearestPoint.x, hn.nearestPoint.y, hn.nearestPoint.z, pop, (int)pl.isActive,
                          pl.disparity, pl.targetDisparity, pl.pose.position.x, pl.pose.position.y,
                          pl.pose.position.z, pl.height);
        }
        stats_ = Stats{};
        return true;
    }
    const char* summary() const { return summary_; }

private:
    //! Locate-space ray -> the renderer's visible window -> cloud space -> cast.
    bool cast(const GsSplatRayCaster& rc, const dxr::CursorRay& ray, float& t) const
    {
        const CursorSceneState& s = state_;
        if (!s.valid) return false;
        const float o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
        const float d[3] = {ray.dir.x, ray.dir.y, ray.dir.z};
        // View depth grows by fd per unit of t along the ray.
        const float fd = d[0] * s.forward[0] + d[1] * s.forward[1] + d[2] * s.forward[2];
        if (!(fd > 1e-4f)) return false;
        float nearDepth = s.nearDepth;
        if (s.displayRig) {
            const float ez = (s.displayCenter[0] - o[0]) * s.forward[0] +
                             (s.displayCenter[1] - o[1]) * s.forward[1] +
                             (s.displayCenter[2] - o[2]) * s.forward[2];
            nearDepth = ez - s.vH;
        }
        const float tMin = nearDepth > 0.0f ? nearDepth / fd : 0.0f;
        if (!s.hasModel) return rc.raycast(o, d, tMin, 1e30f, t);
        // Affine map: t is preserved, so the result is a locate-space t.
        const float* m = s.cloudFromLocate;
        const float oc[3] = {m[0] * o[0] + m[4] * o[1] + m[8] * o[2] + m[12],
                             m[1] * o[0] + m[5] * o[1] + m[9] * o[2] + m[13],
                             m[2] * o[0] + m[6] * o[1] + m[10] * o[2] + m[14]};
        const float dc[3] = {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2],
                             m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
        return rc.raycast(oc, dc, tMin, 1e30f, t);
    }

    static constexpr uint32_t kSummaryFrames = 600;
    struct Stats
    {
        double frameUs = 0.0;
        uint32_t frameRays = 0;
        uint32_t frames = 0, hitFrames = 0, drawFrames = 0;
        uint64_t sumRays = 0;
        double sumUs = 0.0, maxUs = 0.0;
    };
    Stats stats_;
    CursorSceneState state_;
    bool scripted_ = false;
    char summary_[640] = {};
};

} // namespace gsdemo

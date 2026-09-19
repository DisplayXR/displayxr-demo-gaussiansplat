// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Scene fit for the display rig. See gs_scene_fit.h for the model.
 */

#include "gs_scene_fit.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

//! Per-axis [loPct, hiPct] percentile range over a point set.
//!
//! Verbatim from `GsRenderer::getRobustSceneBounds` / the 5-95 bootstrap
//! inside `getMainObjectBounds`: the scratch vector is REFILLED for every axis
//! (so the partial ordering `nth_element` leaves behind never carries over),
//! and the second `nth_element` starts at `loIdx + 1` because everything at or
//! below `loIdx` is already partitioned.
bool PercentileRange(const std::vector<GsFitPoint>& pts,
                     float loPct, float hiPct,
                     float outLo[3], float outHi[3])
{
    const size_t n = pts.size();
    if (n == 0) return false;

    std::vector<float> coord(n);
    for (int axis = 0; axis < 3; axis++) {
        for (size_t i = 0; i < n; i++) {
            const GsFitPoint& g = pts[i];
            coord[i] = (axis == 0) ? g.x : (axis == 1) ? g.y : g.z;
        }
        size_t loIdx = (size_t)(loPct * (float)(n - 1));
        size_t hiIdx = (size_t)(hiPct * (float)(n - 1));
        if (hiIdx <= loIdx) hiIdx = loIdx + 1;
        if (hiIdx >= n) hiIdx = n - 1;
        if (loIdx >= n) loIdx = n ? n - 1 : 0;
        std::nth_element(coord.begin(), coord.begin() + loIdx, coord.end());
        const float lo = coord[loIdx];
        std::nth_element(coord.begin() + loIdx + 1, coord.begin() + hiIdx, coord.end());
        const float hi = coord[hiIdx];
        outLo[axis] = lo;
        outHi[axis] = hi;
    }
    return true;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// GsBuildFitPoints — the trimmed point set both bounds functions measure
// ═════════════════════════════════════════════════════════════════════════

void GsBuildFitPoints(const GsVertex* verts, size_t count,
                      std::vector<GsFitPoint>& out)
{
    out.clear();
    if (verts == nullptr || count == 0) return;

    out.resize(count);
    for (size_t i = 0; i < count; i++) {
        out[i].x = verts[i].position[0];
        out[i].y = verts[i].position[1];
        out[i].z = verts[i].position[2];
        out[i].opacity = verts[i].scale_opacity[3];
    }

    // Floater rejection, verbatim from GsRenderer::loadScene. Below the gate
    // every splat is kept: a percentile over a few hundred points says more
    // about the sample than the scene.
    if (count < kGsFitMinPercentileCount) return;

    float lo[3], hi[3];
    if (!PercentileRange(out, 0.02f, 0.98f, lo, hi)) return;

    float keepMin[3], keepMax[3];
    for (int axis = 0; axis < 3; axis++) {
        const float center = 0.5f * (lo[axis] + hi[axis]);
        const float halfRange = 0.5f * (hi[axis] - lo[axis]);
        keepMin[axis] = center - kGsFitOutlierMargin * halfRange;
        keepMax[axis] = center + kGsFitOutlierMargin * halfRange;
    }

    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        const GsFitPoint& g = out[i];
        if (g.x < keepMin[0] || g.x > keepMax[0] ||
            g.y < keepMin[1] || g.y > keepMax[1] ||
            g.z < keepMin[2] || g.z > keepMax[2]) continue;
        out[kept++] = g;
    }
    out.resize(kept);
}

// ═════════════════════════════════════════════════════════════════════════
// GsPercentileBounds — the fallback, and the graphics leg's old answer
// ═════════════════════════════════════════════════════════════════════════

bool GsPercentileBounds(const GsVertex* verts, size_t count,
                        float loPct, float hiPct,
                        float outCenter[3], float outExtent[3])
{
    std::vector<GsFitPoint> pts;
    GsBuildFitPoints(verts, count, pts);
    if (pts.empty()) return false;

    if (loPct < 0.0f) loPct = 0.0f;
    if (hiPct > 1.0f) hiPct = 1.0f;
    if (hiPct <= loPct) { hiPct = loPct + 1e-3f; if (hiPct > 1.0f) hiPct = 1.0f; }

    float lo[3], hi[3];
    if (!PercentileRange(pts, loPct, hiPct, lo, hi)) return false;
    for (int axis = 0; axis < 3; axis++) {
        outCenter[axis] = 0.5f * (lo[axis] + hi[axis]);
        outExtent[axis] = hi[axis] - lo[axis];
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════
// GsMainObjectBounds — voxelize splats into an opacity-weighted density grid,
// find the peak voxel, BFS-flood-fill at adaptive threshold, return the
// world-space bbox of the filled region. Walls/floor are physically
// air-separated from the figure so the flood-fill stays on the figure.
//
// Ported from GsRenderer::getMainObjectBounds. Every constant, every branch
// and every early-out is the original's; the only change is that the point set
// arrives as an argument instead of as `pickData_`.
// ═════════════════════════════════════════════════════════════════════════

GsFitBounds GsMainObjectBoundsEx(const GsVertex* verts, size_t count,
                                 uint32_t gridSize)
{
    GsFitBounds result;

    std::vector<GsFitPoint> pts;
    GsBuildFitPoints(verts, count, pts);
    if (pts.empty() || gridSize < 4) return result;

    const uint32_t G = gridSize;
    const size_t totalVoxels = (size_t)G * G * G;

    // 1. Scene bounds via 5-95 percentile (ignores extreme outliers).
    float vmin[3], vmax[3];
    if (!PercentileRange(pts, 0.05f, 0.95f, vmin, vmax)) return result;
    for (int axis = 0; axis < 3; axis++) {
        if (vmax[axis] - vmin[axis] < 1e-6f) return result;
    }

    // 2. Voxelize: opacity-weighted density per cell (idx = (x*G + y)*G + z).
    std::vector<float> density(totalVoxels, 0.0f);
    float invSize[3];
    for (int a = 0; a < 3; a++) invSize[a] = (float)G / (vmax[a] - vmin[a]);
    for (const GsFitPoint& g : pts) {
        const float p[3] = {g.x, g.y, g.z};
        int idx[3];
        bool inside = true;
        for (int a = 0; a < 3; a++) {
            const int i = (int)((p[a] - vmin[a]) * invSize[a]);
            if (i < 0 || i >= (int)G) { inside = false; break; }
            idx[a] = i;
        }
        if (!inside) continue;
        density[((size_t)idx[0] * G + (size_t)idx[1]) * G + (size_t)idx[2]] += g.opacity;
    }

    // 3. Find peak voxel.
    float peakDensity = 0.0f;
    size_t peakIdx = 0;
    for (size_t i = 0; i < totalVoxels; i++) {
        if (density[i] > peakDensity) { peakDensity = density[i]; peakIdx = i; }
    }
    if (peakDensity < 1e-6f) return result;

    // 4. Flood-fill helper: BFS from peak including 6-neighbors with
    //    density >= absThreshold. Returns the bool grid + count.
    auto floodFill = [&](float absThreshold, std::vector<bool>& filled) -> size_t {
        std::fill(filled.begin(), filled.end(), false);
        if (density[peakIdx] < absThreshold) return 0;
        filled[peakIdx] = true;
        std::vector<size_t> queue;
        queue.reserve(4096);
        queue.push_back(peakIdx);
        size_t cnt = 1, head = 0;
        const int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        while (head < queue.size()) {
            const size_t idx = queue[head++];
            const int z = (int)(idx % G);
            const int y = (int)((idx / G) % G);
            const int x = (int)(idx / ((size_t)G * G));
            for (int d = 0; d < 6; d++) {
                const int nx = x + dirs[d][0];
                const int ny = y + dirs[d][1];
                const int nz = z + dirs[d][2];
                if (nx < 0 || nx >= (int)G || ny < 0 || ny >= (int)G ||
                    nz < 0 || nz >= (int)G) continue;
                const size_t nidx = ((size_t)nx * G + (size_t)ny) * G + (size_t)nz;
                if (filled[nidx] || density[nidx] < absThreshold) continue;
                filled[nidx] = true;
                queue.push_back(nidx);
                cnt++;
            }
        }
        return cnt;
    };

    // 5. Adaptive threshold search — MASS-FRACTION STOP.
    //
    //    The old rule accepted the first level whose blob covered [1 %, 30 %]
    //    of the GRID. That is scale-dependent, and it fails exactly where it
    //    matters: a small object in a big room. On `KAWS FAMILY.spz` the grid
    //    spans the whole museum (14.6 x 6.2 x 13.2 m, ~0.2 m cells), so every
    //    level that isolates the statue covers under 0.2 % of the grid and was
    //    rejected on sight; the largest-under-maxFill fallback then walked down
    //    to 0.5 % of peak and returned the ROOM. Measured on the cloud:
    //
    //      10 %  ->  111 vox (0.04 % grid), 30 % mass, 1.59 x 1.46 x 1.44
    //       5 %  ->  223 vox (0.09 %),      38 % mass, 1.82 x 1.95 x 1.65  <- statue
    //       1 %  ->  449 vox (0.17 %),      44 % mass, 3.18 x 2.05 x 3.30
    //     0.5 %  -> 1354 vox (0.52 %),      54 % mass, 9.78 x 3.90 x 7.63  <- room
    //
    //    So ask the scale-free question instead: how much of the scene's
    //    opacity-weighted MASS is in the blob? Descend the ladder and take the
    //    HIGHEST threshold (tightest blob) that already holds a third of it.
    //    A third is enough to mean "this is what the scene is of" while still
    //    excluding the room, which only arrives with the last few percent.
    //
    //    Plus a knee guard: refuse to descend past a level whose largest
    //    extent grew more than kFitExtentJump in ONE step. That is the moment
    //    the fill leaks out of the object and into the room (3.18 -> 9.78
    //    above, a 3.1x jump) and it is visible without knowing any absolute
    //    size. If the knee comes before the mass target is met, the level
    //    BEFORE the knee wins — better a tight object than a room.
    const float thresholds[] = {0.50f, 0.30f, 0.20f, 0.10f, 0.07f,
                                0.05f, 0.03f, 0.02f, 0.01f, 0.005f};

    // Total opacity-weighted mass of the SCENE — every trimmed splat, not just
    // the ones that landed inside the grid. The grid only spans the [p5, p95]
    // box, so summing it instead would quietly shrink the denominator by
    // whatever lies outside that box and inflate every fraction. On
    // `KAWS FAMILY.spz` that alone moved the answer a whole rung (the 10 %
    // level reads 30 % of the scene but 35 % of the grid), which is the
    // difference between framing the statue and framing its plinth.
    double totalMass = 0.0;
    for (const GsFitPoint& g : pts) totalMass += (double)g.opacity;
    if (!(totalMass > 0.0)) return result;

    std::vector<bool> filled(totalVoxels, false);
    std::vector<bool> bestFilled(totalVoxels, false);
    size_t bestCount = 0;
    float bestThresh = 0.0f;
    float bestMassFrac = 0.0f;
    float prevMaxExtent = 0.0f;
    float prevMassFrac = 0.0f;
    bool  haveCandidate = false;
    // DXR_FIT_LADDER=1 prints every rung. The stop rule is a judgement about
    // the shape of a curve, so seeing the curve is how it gets argued about.
    const bool ladderLog = (getenv("DXR_FIT_LADDER") != nullptr);

    for (float t : thresholds) {
        const size_t cnt = floodFill(t * peakDensity, filled);
        if (cnt == 0) continue;

        // Mass and voxel bbox of THIS blob.
        double blobMass = 0.0;
        int lo[3] = {(int)G, (int)G, (int)G}, hi[3] = {-1, -1, -1};
        for (size_t idx = 0; idx < totalVoxels; idx++) {
            if (!filled[idx]) continue;
            blobMass += (double)density[idx];
            const int z = (int)(idx % G);
            const int y = (int)((idx / G) % G);
            const int x = (int)(idx / ((size_t)G * G));
            if (x < lo[0]) lo[0] = x; if (x > hi[0]) hi[0] = x;
            if (y < lo[1]) lo[1] = y; if (y > hi[1]) hi[1] = y;
            if (z < lo[2]) lo[2] = z; if (z > hi[2]) hi[2] = z;
        }
        const float massFrac = (float)(blobMass / totalMass);
        float maxExtent = 0.0f;
        for (int a = 0; a < 3; a++) {
            const float voxSize = (vmax[a] - vmin[a]) / (float)G;
            const float e = (float)(hi[a] - lo[a] + 1) * voxSize;
            if (e > maxExtent) maxExtent = e;
        }

        if (ladderLog) {
            fprintf(stderr, "  [fit ladder] t=%.3f x peak: %zu vox (%.3f%% grid), "
                            "mass %.1f%%, extent %.3f x %.3f x %.3f (max %.3f)\n",
                    (double)t, cnt, 100.0 * (double)cnt / (double)totalVoxels,
                    100.0 * (double)massFrac,
                    (double)((float)(hi[0]-lo[0]+1) * (vmax[0]-vmin[0]) / (float)G),
                    (double)((float)(hi[1]-lo[1]+1) * (vmax[1]-vmin[1]) / (float)G),
                    (double)((float)(hi[2]-lo[2]+1) * (vmax[2]-vmin[2]) / (float)G),
                    (double)maxExtent);
        }

        // The knee: this level leaked out of the object and into whatever it
        // is standing in. Keep what we had and stop.
        //
        // Gated on the PREVIOUS blob already being substantial. At the top of
        // the ladder a blob is a handful of voxels and one extra row more than
        // 1.6x-es its extent, so an ungated knee fires on the first step every
        // time — it did, and it framed butterfly.spz at 0.118 vH (five voxels).
        // A leak is only a leak once there is something to leak OUT of.
        if (haveCandidate && prevMassFrac >= kFitKneeMinMassFrac &&
            prevMaxExtent > 1.0e-6f &&
            maxExtent > kFitExtentJump * prevMaxExtent) {
            if (ladderLog) fprintf(stderr, "  [fit ladder] knee -> keeping t=%.3f\n",
                                   (double)bestThresh);
            break;
        }

        bestFilled = filled;
        bestCount = cnt;
        bestThresh = t;
        bestMassFrac = massFrac;
        prevMaxExtent = maxExtent;
        prevMassFrac = massFrac;
        haveCandidate = true;

        // Tightest blob that already holds a third of the scene's mass.
        if (massFrac >= kFitMinMassFrac) break;
    }
    if (bestCount == 0) return result;
    result.massFraction = bestMassFrac;

    // 6. Bbox of filled voxels in voxel coords, then convert to world.
    int minVox[3] = {(int)G, (int)G, (int)G};
    int maxVox[3] = {-1, -1, -1};
    for (size_t idx = 0; idx < totalVoxels; idx++) {
        if (!bestFilled[idx]) continue;
        const int z = (int)(idx % G);
        const int y = (int)((idx / G) % G);
        const int x = (int)(idx / ((size_t)G * G));
        if (x < minVox[0]) minVox[0] = x; if (x > maxVox[0]) maxVox[0] = x;
        if (y < minVox[1]) minVox[1] = y; if (y > maxVox[1]) maxVox[1] = y;
        if (z < minVox[2]) minVox[2] = z; if (z > maxVox[2]) maxVox[2] = z;
    }
    float denseCenter[3], denseExt[3];
    for (int a = 0; a < 3; a++) {
        const float voxSize = (vmax[a] - vmin[a]) / (float)G;
        const float denseMinW = vmin[a] + (float)minVox[a] * voxSize;
        const float denseMaxW = vmin[a] + (float)(maxVox[a] + 1) * voxSize;
        denseCenter[a] = 0.5f * (denseMinW + denseMaxW);
        denseExt[a] = denseMaxW - denseMinW;
    }

    // 7. Branch on regime: a high fill ratio means the dense cluster
    //    occupies most of the scene's 5-95 bbox, so we have a single tight
    //    object (butterfly) — use full min/max so sparse extremities like
    //    antennae aren't clipped. A low fill ratio means the dense cluster
    //    is a small central region inside a larger scene (KAWS gallery,
    //    Leila room) — use the exact bbox of gaussians within the flood-
    //    fill voxels (tighter than the voxel-aligned bbox).
    const float fillRatio = (float)bestCount / (float)totalVoxels;
    const float kSingleObjectFillThresh = 0.02f;
    const float kComfort = 1.10f;  // 10 % margin on the object bbox

    const bool isSingleObject = (fillRatio > kSingleObjectFillThresh);
    if (isSingleObject) {
        // Full min/max bounding box of all gaussians.
        float fullMin[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
        float fullMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const GsFitPoint& g : pts) {
            if (g.x < fullMin[0]) fullMin[0] = g.x;
            if (g.x > fullMax[0]) fullMax[0] = g.x;
            if (g.y < fullMin[1]) fullMin[1] = g.y;
            if (g.y > fullMax[1]) fullMax[1] = g.y;
            if (g.z < fullMin[2]) fullMin[2] = g.z;
            if (g.z > fullMax[2]) fullMax[2] = g.z;
        }
        for (int a = 0; a < 3; a++) {
            result.center[a] = 0.5f * (fullMin[a] + fullMax[a]);
            result.extent[a] = (fullMax[a] - fullMin[a]) * kComfort;
        }
        result.source = GsBoundsSource::FloodFillObject;
    } else {
        // Gaussian-precise bbox + opacity-weighted centroid for in-flood-
        // fill gaussians. Center on the centroid (pulls toward the densest
        // part of the object — e.g. the can in a Leila scene — away from
        // sparse appendages like a chain extending upward). Extent is
        // symmetric around the centroid covering the full precise bbox so
        // sparse extensions stay visible at the frame edge but don't
        // dominate the framing center.
        float preciseMin[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
        float preciseMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        double sumW = 0.0;
        double centroidSum[3] = {0.0, 0.0, 0.0};
        for (const GsFitPoint& g : pts) {
            const int xi = (int)((g.x - vmin[0]) * invSize[0]);
            const int yi = (int)((g.y - vmin[1]) * invSize[1]);
            const int zi = (int)((g.z - vmin[2]) * invSize[2]);
            if (xi < 0 || xi >= (int)G || yi < 0 || yi >= (int)G ||
                zi < 0 || zi >= (int)G) continue;
            const size_t vid = ((size_t)xi * G + (size_t)yi) * G + (size_t)zi;
            if (!bestFilled[vid]) continue;
            if (g.x < preciseMin[0]) preciseMin[0] = g.x;
            if (g.x > preciseMax[0]) preciseMax[0] = g.x;
            if (g.y < preciseMin[1]) preciseMin[1] = g.y;
            if (g.y > preciseMax[1]) preciseMax[1] = g.y;
            if (g.z < preciseMin[2]) preciseMin[2] = g.z;
            if (g.z > preciseMax[2]) preciseMax[2] = g.z;
            const double w = g.opacity;
            sumW += w;
            centroidSum[0] += w * g.x;
            centroidSum[1] += w * g.y;
            centroidSum[2] += w * g.z;
        }
        // Defensive fall-back (shouldn't trigger in practice).
        if (preciseMin[0] > preciseMax[0] || sumW < 1e-6) {
            for (int a = 0; a < 3; a++) {
                result.center[a] = denseCenter[a];
                result.extent[a] = denseExt[a] * kComfort;
            }
        } else {
            // Centre on the opacity centroid — it pulls toward the dense part
            // of the subject and away from sparse appendages — but take the
            // extent from the precise bbox ITSELF rather than symmetrising it
            // about that centroid.
            //
            // The symmetrisation was compensating for a blob that might be the
            // whole room: doubling the larger half guaranteed the subject
            // stayed inside whatever had been found. The mass-fraction stop
            // now identifies the subject directly, so the compensation has
            // become pure inflation — on `KAWS FAMILY.spz` it turned a 1.95 m
            // statue into a 2.38 m box and framed it a quarter too far out.
            // What survives is the centring, which is the half that was always
            // about the picture rather than about not trusting the blob.
            const float centroid[3] = {(float)(centroidSum[0] / sumW),
                                       (float)(centroidSum[1] / sumW),
                                       (float)(centroidSum[2] / sumW)};
            for (int a = 0; a < 3; a++) {
                result.center[a] = centroid[a];
                result.extent[a] = (preciseMax[a] - preciseMin[a]) * kComfort;
            }
        }
        result.source = GsBoundsSource::FloodFillScene;
    }

    result.valid = true;
    result.filledVoxels = bestCount;
    result.totalVoxels = totalVoxels;
    result.fillRatio = fillRatio;
    result.threshold = bestThresh;
    return result;
}

bool GsMainObjectBounds(const GsVertex* verts, size_t count,
                        uint32_t gridSize,
                        float outCenter[3], float outExtent[3])
{
    const GsFitBounds b = GsMainObjectBoundsEx(verts, count, gridSize);
    if (!b.valid) return false;
    for (int a = 0; a < 3; a++) {
        outCenter[a] = b.center[a];
        outExtent[a] = b.extent[a];
    }
    return true;
}

GsFitBounds GsResolveFitBounds(const GsVertex* verts, size_t count,
                               uint32_t gridSize)
{
    GsFitBounds b = GsMainObjectBoundsEx(verts, count, gridSize);
    if (b.valid) return b;

    // The flood-fill found nothing to fill — a cloud with no dense core, or
    // one too small for the percentile bootstrap. The [p5, p95] box is the
    // documented fallback, and the graphics leg's historical answer.
    float c[3], e[3];
    if (!GsPercentileBounds(verts, count, 0.05f, 0.95f, c, e)) return b;
    for (int a = 0; a < 3; a++) { b.center[a] = c[a]; b.extent[a] = e[a]; }
    b.valid = true;
    b.source = GsBoundsSource::Percentile;
    return b;
}

// ═════════════════════════════════════════════════════════════════════════
// GsFitFrame — the pose-projected, depth-aware vHeight
// ═════════════════════════════════════════════════════════════════════════

void GsFitViewBasis(float yaw, float pitch,
                    float outRight[3], float outUp[3], float outForward[3])
{
    // R = Ry(yaw) * Rx(pitch), the convention quat_from_yaw_pitch implements
    // (view_rig_math.h: "rotates (0,0,-1) to (-cos(p)sin(y), sin(p),
    // -cos(p)cos(y))"). Columns of R are the rig's right / up / back axes; the
    // viewing direction is -back, so `forward` — the away-from-viewer axis
    // this module measures depth along — is -col2.
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);

    outRight[0] =  cy;      outRight[1] = 0.0f; outRight[2] = -sy;
    outUp[0]    =  sy * sp; outUp[1]    = cp;   outUp[2]    =  cy * sp;
    outForward[0] = -sy * cp; outForward[1] = sp; outForward[2] = -cy * cp;
}

void GsFitDepthBudgetVH(const GsFitComfort& comfort,
                        float& outFront, float& outBehind)
{
    const float k = comfort.viewerDistanceVH;
    const float s = comfort.eyeSeparationVH;
    const float m = comfort.maxDisparityVH;

    if (!(k > 0.0f) || !(s > 0.0f) || !(m > 0.0f)) {
        // Degenerate comfort model: no budget to spend, so the depth term
        // never binds and the fit is the flat one.
        outFront = outBehind = std::numeric_limits<float>::infinity();
        return;
    }

    // behind: s*d/(k+d) <= m  ->  d <= m*k/(s-m)   (unbounded when s <= m:
    //                                               even infinity fuses)
    // front:  s*d/(k-d) <= m  ->  d <= m*k/(s+m)
    outFront  = m * k / (s + m);
    outBehind = (s > m) ? (m * k / (s - m))
                        : std::numeric_limits<float>::infinity();
}

GsFitFrameResult GsFitFrameEx(const GsFitBounds& bounds,
                              float viewportW, float viewportH,
                              float fill,
                              float yaw, float pitch,
                              const GsFitComfort& comfort)
{
    GsFitFrameResult r;
    if (!bounds.valid) return r;

    // ── Project the bounds into the viewing pose's screen axes ───────────
    // The AABB's support along a unit axis `u` is sum_a |extent[a]/2 * u[a]|
    // (the corner that maximises the dot product picks each axis's sign
    // independently). At yaw = pitch = 0 the basis is the world basis and
    // this returns extent[0], extent[1], extent[2] exactly — the flat case is
    // bit-for-bit what the call sites computed before.
    float right[3], up[3], forward[3];
    GsFitViewBasis(yaw, pitch, right, up, forward);

    const float half[3] = {0.5f * bounds.extent[0],
                           0.5f * bounds.extent[1],
                           0.5f * bounds.extent[2]};
    auto support = [&](const float axis[3]) -> float {
        return std::fabs(half[0] * axis[0]) +
               std::fabs(half[1] * axis[1]) +
               std::fabs(half[2] * axis[2]);
    };
    const float halfW = support(right);
    const float halfH = support(up);
    const float halfD = support(forward);

    r.screenW = 2.0f * halfW;
    r.screenH = 2.0f * halfH;
    r.screenD = 2.0f * halfD;

    // ── The x/y core: dxr::AutoFitVHeight, on the projected extents ──────
    // Kept as its own term rather than calling the header, because
    // 3dgs_common must not depend on displayxr-common (the Android leg builds
    // this directory without it). The rule is copied, not reinterpreted:
    //     vHeight = max(H, W / aspect) / fill
    if (!(r.screenH > 0.0f) || !(fill > 0.0f)) return r;
    float vhFlat = r.screenH / fill;
    r.boundBy = "height";
    if (r.screenW > 0.0f && viewportW > 0.0f && viewportH > 0.0f) {
        const float aspect = viewportW / viewportH;
        const float vhForWidth = r.screenW / (fill * aspect);
        if (vhForWidth > vhFlat) { vhFlat = vhForWidth; r.boundBy = "width"; }
    }
    r.vHeightFlat = vhFlat;

    // ── The depth term ───────────────────────────────────────────────────
    // The pivot sits at the fit centre, so the blob straddles the ZDP with
    // `halfD` on each side. The front budget is the tighter one, so it is the
    // one that sets vHeight.
    float deltaFront = 0.0f, deltaBehind = 0.0f;
    GsFitDepthBudgetVH(comfort, deltaFront, deltaBehind);

    float vh = vhFlat;
    if (halfD > 0.0f && deltaFront > 0.0f && std::isfinite(deltaFront)) {
        r.vHeightDepth = halfD / deltaFront;

        // Advisory pivot shift: pushing the pivot `t` further from the viewer
        // moves `t` of front depth into the rear budget. Both sides bind at
        //     t = halfD * (dBehind - dFront) / (dBehind + dFront)
        // which is `halfD` itself when the rear budget is unbounded.
        r.pivotDepthShift =
            std::isfinite(deltaBehind)
                ? halfD * (deltaBehind - deltaFront) / (deltaBehind + deltaFront)
                : halfD;
        if (r.pivotDepthShift < 0.0f) r.pivotDepthShift = 0.0f;

        if (r.vHeightDepth > vh) {
            const float ceiling = vhFlat * kGsFitMaxDepthGrowth;
            if (r.vHeightDepth > ceiling) {
                vh = ceiling;
                r.depthBudgetSatisfied = false;
            } else {
                vh = r.vHeightDepth;
            }
            r.boundBy = "depth";
        }
    }

    r.vHeight = vh;
    r.valid = true;
    return r;
}

void GsFitFrame(const GsFitBounds& bounds,
                float viewportW, float viewportH,
                float fill,
                float yaw, float pitch,
                float& outVHeight, float& outPivotDepthShift)
{
    const GsFitFrameResult r =
        GsFitFrameEx(bounds, viewportW, viewportH, fill, yaw, pitch);
    outVHeight = r.valid ? r.vHeight : 0.0f;
    outPivotDepthShift = r.valid ? r.pivotDepthShift : 0.0f;
}

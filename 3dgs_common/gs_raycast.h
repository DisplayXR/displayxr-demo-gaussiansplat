// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CPU ray cast against a Gaussian-splat cloud: the depth under the
 *         pointer for the depth-aware cursor (XR_DXR_cursor_depth).
 *
 * The cursor needs "ray -> the first SURFACE the viewer sees", ~20 times a
 * frame (10 footprint points x the two outer views), on clouds of up to a few
 * million splats. Two things make the obvious answers wrong here:
 *
 *  - A splat is not a surface. "Nearest centre to the ray" (pickGaussian)
 *    happily returns a faint splat of haze in front of the subject, or a
 *    splat on the BACK of the subject whose centre happens to lie closest to
 *    the ray. What the viewer sees at a pixel is the front-to-back alpha
 *    composite, so the depth wanted is where that composite turns opaque:
 *    the ray parameter at which accumulated opacity first crosses 0.5 (the
 *    median depth of the composite, the "first opaque surface").
 *  - A full scan is O(N) per ray: 20 x 1-3 M splats a frame is ~100x over
 *    budget.
 *
 * Each splat is evaluated exactly the way the rasteriser composites it:
 *
 *     alpha = opacity * exp(-0.5 * D^2),  D^2 = min over the ray of the
 *     Mahalanobis distance to the splat centre, reached at t*
 *
 * (the 3D peak of the Gaussian along the ray — what the projected 2D splat
 * evaluates to at that pixel), with the rasteriser's 0.99 clamp and 1/255
 * floor; the hit depth is t*.
 *
 * Acceleration: an implicit 8-ary bounding-volume hierarchy over the splats
 * sorted along a Morton curve. Leaves are 8 consecutive splats, every level
 * above groups 8 consecutive nodes, and a node's box is the union of its
 * splats' k-sigma boxes (the exact AABB of each k-sigma ellipsoid). The sort
 * is one radix pass set and the boxes one bottom-up sweep, so a 3 M-splat
 * cloud builds in well under a second — on a worker thread, off the load path.
 * It adapts to density on its own (a uniform grid does not: on an outdoor
 * scene its surface cells hold thousands of splats while most cells are
 * empty).
 *
 * The walk is front to back: nodes come off a min-heap keyed by where the ray
 * ENTERS their box. A splat whose alpha matters has its closest-approach
 * point t* inside its own k-sigma box, hence inside every ancestor's box, so
 * no unvisited node can still produce a hit before the heap's smallest entry.
 * Pending hits before that frontier are therefore final: they are composited
 * in order, and the walk stops the moment transmittance reaches the
 * threshold. Empty space costs a box test per node.
 *
 * Everything is in the cloud's own (splat) space. The caller maps its
 * locate-space ray in with the inverse of the model transform — an affine map
 * preserves the ray parameter t, so the result is directly a locate-space t.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct GsVertex;

class GsSplatRayCaster {
public:
    struct Options {
        //! Splats below this opacity are left out entirely. One alone cannot
        //! make the composite opaque — it takes >= 7 stacked to cross 0.5 —
        //! and a stack of them is haze, not a surface for the cursor.
        float minOpacity = 0.1f;
        //! Ellipsoid cut-off in standard deviations (the box size). Beyond
        //! 2.5 sigma a splat contributes alpha <= 0.044 * opacity, which does
        //! not move a 0.5 crossing.
        float sigmaCutoff = 2.5f;
        //! Transmittance at which the composite counts as a surface (0.5 =
        //! accumulated opacity crosses 0.5).
        float transmittanceThreshold = 0.5f;
        //! Hard cap on work per ray (splat evaluations + box tests): a ray
        //! skimming a dense cloud that never turns opaque. Exceeding it = no hit.
        uint32_t maxWorkPerRay = 60000;
        //! Linear scan instead of the hierarchy — the reference the probe
        //! checks the hierarchy against. Never for interactive use.
        bool bruteForce = false;
    };

    struct BuildStats {
        size_t input = 0;       //!< splats offered
        size_t kept = 0;        //!< after the opacity filter + floater trim
        size_t nodes = 0;       //!< hierarchy nodes, all levels
        uint32_t levels = 0;
        double ms = 0.0;        //!< wall time of the build itself
        size_t bytes = 0;       //!< memory held
    };

    struct RayStats {
        uint32_t nodes = 0;     //!< boxes tested
        uint32_t tests = 0;     //!< splat evaluations
        float transmittance = 1.0f; //!< composite transmittance where the walk stopped
    };

    GsSplatRayCaster() = default;
    ~GsSplatRayCaster();
    GsSplatRayCaster(const GsSplatRayCaster&) = delete;
    GsSplatRayCaster& operator=(const GsSplatRayCaster&) = delete;

    //! Build synchronously from the splats as uploaded (after any load-time
    //! cull or decimation, so the ray cast sees what the renderer draws).
    void build(const GsVertex* verts, size_t count, const Options& opt);
    void build(const GsVertex* verts, size_t count);

    //! Build on a worker thread from a vertex array the caller no longer
    //! needs (the renderer hands over its CPU copy after upload). raycast()
    //! reports no hit until the build lands. A newer build or clear() cancels
    //! one still running.
    void buildAsync(std::vector<GsVertex>&& verts, const Options& opt);
    void buildAsync(std::vector<GsVertex>&& verts);

    void clear();
    //! True once a build has landed.
    bool valid() const;
    //! Stats of the landed build (zeroed until then).
    BuildStats buildStats() const;

    /*!
     * First surface along a ray, in splat space.
     *
     * @param o,d  ray origin and direction (d need not be unit; t is in units
     *             of d, so an affine-mapped ray keeps the caller's t).
     * @param tMin,tMax  the visible window along the ray (the renderer's
     *             near / far culls); hits outside it are ignored.
     * @param tHit out: the ray parameter where the composite becomes opaque.
     * @return false = no surface in the window (the composite never reached
     *         the threshold, the work cap was hit, or no build has landed).
     */
    bool raycast(const float o[3], const float d[3], float tMin, float tMax, float& tHit,
                 RayStats* rs = nullptr) const;

private:
    struct Splat {
        float mu[3];
        float opacity;
        float invScale[3];
        int16_t q[4];   //!< rotation (w, x, y, z), unit quaternion x 32767
    };
    struct Box {
        float lo[3];
        float hi[3];
    };
    struct Built {
        Options opt;
        BuildStats stats;
        float k2 = 6.25f;
        std::vector<Splat> splats;          //!< in Morton order
        std::vector<Box> nodes;             //!< all levels, leaves first
        std::vector<uint32_t> levelStart;   //!< first node of each level (+ end)
    };
    struct Hit {
        float t;
        float alpha;
    };

    static std::shared_ptr<Built> doBuild(const GsVertex* verts, size_t count, const Options& opt,
                                          const std::atomic<bool>* cancel);
    static bool evalSplat(const Built& b, const Splat& s, const float o[3], const float d[3], Hit& h);

    void joinWorker();

    mutable std::mutex mu_;
    std::shared_ptr<const Built> built_;    //!< swapped under mu_
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

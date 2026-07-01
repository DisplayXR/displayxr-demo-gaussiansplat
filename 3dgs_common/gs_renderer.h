// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  3DGS renderer — embedded compute pipeline for Gaussian splatting
 *
 * Manages 8 compute shaders (precomp_cov3d, preprocess, prefix_sum,
 * preprocess_sort, hist, sort, tile_boundary, render) to splat Gaussians
 * loaded from a .ply file.  Renders to an internal UNORM storage image,
 * then copies to the swapchain viewport region.
 *
 * GsRenderer manages its own command buffers because the 3DGS pipeline
 * requires a CPU readback between preprocess and sort stages.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>
#include <vector>
#include "gs_vulkan_utils.h"
#include "gs_scene_loader.h"  // GsVertex (used by filterFloaters signature)

struct GsPickData {
    float px, py, pz;   // world-space position
    float maxScale;      // max(sx, sy, sz) — sphere radius for ray test
    float opacity;       // for scoring (favor visible splats)
};

struct GsRenderer {
    // Initialize with the OpenXR runtime's Vulkan resources.
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    // Load a .ply Gaussian splatting scene file.
    // Parses PLY, uploads vertex data, creates compute pipelines & buffers.
    bool loadScene(const char* plyPath);

    // Load a debug scene: single red splat at the given position/radius.
    bool loadDebugScene(float x, float y, float z, float radius);

    // Returns true if a scene is currently loaded.
    bool hasScene() const;

    // Returns the loaded scene file path.
    const std::string& scenePath() const;

    // Returns the number of Gaussians in the loaded scene.
    uint32_t gaussianCount() const;

    // Raycast pick: find the nearest visible gaussian along a ray.
    // Returns true if a hit was found, with the gaussian center written to hitPos.
    // When viewMatrix != nullptr and clipNear/clipFar > 0, candidates whose
    // view-space forward depth (dot(center - eye, viewForward)) falls outside
    // [clipNear, clipFar] are rejected — the SAME visibility window the renderer
    // clips to (see renderEye), so splats clipped in front of the near plane (or
    // behind the far plane in foreground mode) can never be picked/recentered.
    // viewMatrix is the column-major world->view matrix used to build the ray.
    bool pickGaussian(const float rayOrigin[3], const float rayDir[3],
                      float hitPos[3], float maxDistance = 100.0f,
                      const float viewMatrix[16] = nullptr,
                      float clipNear = 0.0f, float clipFar = 0.0f) const;

    // World-space axis-aligned bounding box of loaded splat centers.
    // Returns false if no scene is loaded; otherwise writes min/max xyz.
    bool getSceneBBox(float outMin[3], float outMax[3]) const;

    // Find the display yaw (radians, around world +Y) that maximises the
    // opacity-weighted mass of gaussians in front of the viewer. Tests
    // numCandidates evenly-spaced yaws from 0 to 2π. Used by demos for
    // initial framing so the user starts facing the captured side of the
    // object rather than its back.
    //
    // displayCenter[3] = world position of the display rig (typically the
    //                    robust scene centroid).
    // viewerOffsetLocal[3] = viewer position in display-local coords (e.g.
    //                       {0, 0.1, 0.6} matching nominalViewer).
    // Returns 0.0 if no scene is loaded.
    float findBestYaw(const float displayCenter[3],
                      const float viewerOffsetLocal[3],
                      uint32_t numCandidates = 8) const;

    // Locate the main object via voxel-density flood-fill from the peak
    // voxel. Voxelizes splats into a gridSize³ grid (opacity-weighted),
    // finds the densest voxel, BFS-fills to neighbors at ≥ threshold × peak.
    // The threshold is auto-adapted so the filled region falls between
    // ~1 % and ~30 % of the grid. Returns the world-space bbox of the
    // filled region.
    //
    // Works because the main object occupies a contiguous 3D blob, while
    // walls/floor/ceiling are physically separated by air gaps that the
    // flood-fill cannot cross at typical voxel sizes (≤ object–wall gap).
    //
    // gridSize: voxels per axis (64 is a good default — 25 cm voxels for
    //           a 16 m scene, well under typical room air-gap distance).
    // Returns false if no scene loaded or peak density is zero.
    bool getMainObjectBounds(uint32_t gridSize,
                             float outCenter[3], float outExtent[3]) const;

    // Robust scene centroid + per-axis extent, excluding outlier gaussians.
    // For each axis, takes the positions at the loPct / hiPct quantiles
    // (e.g. 0.05 / 0.95) and returns center = midpoint, extent = hi − lo.
    // Splat PLYs commonly contain stray gaussians far from the main cluster;
    // a raw AABB is dominated by them. Percentile trimming matches what
    // mature splat viewers do for auto-framing.
    // Returns false if no scene is loaded.
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;

    // Render one eye's view to a region of a Vulkan swapchain image.
    // Manages its own command buffers internally (allocate, record, submit, wait).
    // viewMatrix and projMatrix are column-major float[16].
    // transparentBg=true makes the render shader output premultiplied alpha
    // (1 - T) so background-uncovered pixels are 0 — the runtime then strips
    // them on the chroma-key pass for desktop see-through.
    // clipNearViewSpace>0 culls splats whose view-space forward distance is LESS
    // than it (near plane: hide content popping toward the viewer). 0=off.
    // clipFarViewSpace>0 culls splats whose view-space forward distance exceeds
    // it (foreground-only mode: hide content behind the display plane). 0=off.
    // NOTE: this splat rasterizer does NOT clip against the projection matrix's
    // near/far planes (only ndc.xy + p_view.z are used downstream), so geometric
    // near/far clipping MUST come through these explicit view-space culls.
    // clipFadeFrac>0 softens the NEAR cut into an opacity rolloff over the band
    // [clipNearViewSpace, clipNearViewSpace*(1+frac)] instead of a hard discard,
    // removing the pop as a splat center crosses the near plane. 0=hard near cut.
    // The FAR cut is always hard (no fade) regardless of clipFadeFrac.
    void renderEye(VkImage swapchainImage,
                   VkFormat swapchainFormat,
                   uint32_t imageWidth,
                   uint32_t imageHeight,
                   uint32_t viewportX,
                   uint32_t viewportY,
                   uint32_t viewportWidth,
                   uint32_t viewportHeight,
                   const float viewMatrix[16],
                   const float projMatrix[16],
                   bool transparentBg = false,
                   float clipNearViewSpace = 0.0f,
                   float clipFarViewSpace = 0.0f,
                   float clipFadeFrac = 0.0f);

    // Render-scale: run the entire compute pipeline (projection, tile grid,
    // sort, per-pixel composite) at scale*viewport dims, then upscale-blit the
    // result to the full viewport. Splats are soft so moderate downscale costs
    // little visually, while it cuts the per-pixel render cost AND shrinks the
    // tile grid → fewer (gaussian,tile) instances → less sort/preSort work.
    // 1.0 = native (default, desktop). Clamped to (0.05, 1.0].
    void setRenderScale(float s) { renderScale_ = (s > 0.05f && s <= 1.0f) ? s : 1.0f; }
    float renderScale() const { return renderScale_; }

    // Load-time opacity cull: drop gaussians whose (already sigmoid'd) opacity is
    // below this before upload. Near-transparent splats still expand into
    // (gaussian,tile) fragments that get sorted + composited every eye but
    // contribute almost nothing — culling them cuts preprocess + sort + render
    // together (the GPU-bound cost on mobile). 0 = off (keep all). Must be set
    // BEFORE loadScene(). Typical 0.02–0.1.
    void setCullMinOpacity(float a) { cullMinOpacity_ = (a >= 0.0f && a < 1.0f) ? a : 0.0f; }
    float cullMinOpacity() const { return cullMinOpacity_; }

    // Load-time decimation: keep ~this fraction of gaussians (hash-selected so
    // it's uniform regardless of file ordering), dropping the rest before
    // upload. Unlike the opacity cull this works on dense OPAQUE scenes (e.g.
    // butterfly.spz, ~all splats >0.1 opacity) where opacity culling drops
    // nothing — it cuts the gaussian count, hence the (gaussian,tile) fragment
    // count, hence preprocess + sort + composite, ~linearly. Splats are soft so
    // moderate decimation thins gracefully. 1.0 = off. Set BEFORE loadScene().
    void setKeepFraction(float f) { cullKeepFrac_ = (f > 0.05f && f <= 1.0f) ? f : 1.0f; }
    float keepFraction() const { return cullKeepFrac_; }

    // Clean up all resources.
    void cleanup();

    ~GsRenderer();

private:
    // ── Core Vulkan handles (not owned, from OpenXR runtime) ─────────────
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t subgroupSize_ = 32;
    float renderScale_ = 1.0f;      // see setRenderScale()
    float cullMinOpacity_ = 0.0f;   // see setCullMinOpacity()
    float cullKeepFrac_ = 1.0f;     // see setKeepFraction()

    // ── GPU timestamp profiling ──────────────────────────────────────────
    // Per-stage VkQueryPool timestamps around each renderEye dispatch group
    // (preprocess / prefix-sum / preprocess-sort / radix-sort / tile-boundary
    // / render / blit) to pinpoint where the per-eye GPU time goes. Logged
    // every kTsLogPeriod renderEye calls via GS_LOGI (logcat on Android,
    // stdout on desktop). timestampPeriod_ == 0 → timestamps unsupported on
    // this queue, profiling disabled.
    static constexpr uint32_t kNumTimestamps = 8;
    VkQueryPool tsPool_ = VK_NULL_HANDLE;
    float timestampPeriod_ = 0.0f;   // ns per tick (0 = unsupported)
    uint32_t tsValidBits_ = 0;       // valid high bits of the queue's timestamp
    uint64_t frameCounter_ = 0;

    bool initialized_ = false;
    bool sceneLoaded_ = false;
    std::string loadedScenePath_;
    uint32_t numGaussians_ = 0;

    // ── Derived dimensions ───────────────────────────────────────────────
    uint32_t tileX_ = 0;     // ceil(width/16)
    uint32_t tileY_ = 0;     // ceil(height/16)
    uint32_t maxSortInstances_ = 0;  // grows on overflow via growSortBuffers()
    uint32_t numPrefixSumIter_ = 0;  // ceil(log2(numGaussians))

    // ── GPU Buffers (14 total) ───────────────────────────────────────────
    GsBuffer vertexBuffer_;         // N * 240 bytes
    GsBuffer cov3DBuffer_;          // N * 24 bytes
    GsBuffer uniformBuffer_;        // 176 bytes (host-visible)
    GsBuffer vertexAttrBuffer_;     // N * 64 bytes
    GsBuffer tileOverlapBuffer_;    // N * 4 bytes
    GsBuffer prefixSumPingBuffer_;  // N * 4 bytes
    GsBuffer prefixSumPongBuffer_;  // N * 4 bytes
    GsBuffer totalSumHostBuffer_;   // 4 bytes (host-visible)
    GsBuffer sortKeysEvenBuffer_;   // maxSort * 4 bytes (32-bit packed keys)
    GsBuffer sortKeysOddBuffer_;    // maxSort * 4 bytes
    GsBuffer sortValsEvenBuffer_;   // maxSort * 4 bytes
    GsBuffer sortValsOddBuffer_;    // maxSort * 4 bytes
    GsBuffer sortHistBuffer_;       // numWorkgroups * 256 * 4 bytes
    GsBuffer tileBoundaryBuffer_;   // tileX * tileY * 2 * 4 bytes

    // ── Internal render image ────────────────────────────────────────────
    GsImage renderImage_;  // R8G8B8A8_UNORM, width_ x height_

    // ── Compute pipelines (8) ────────────────────────────────────────────
    VkPipeline pipePrecompCov3d_ = VK_NULL_HANDLE;
    VkPipeline pipePreprocess_ = VK_NULL_HANDLE;
    VkPipeline pipePrefixSum_ = VK_NULL_HANDLE;
    VkPipeline pipePreprocessSort_ = VK_NULL_HANDLE;
    VkPipeline pipeHist_ = VK_NULL_HANDLE;
    VkPipeline pipeSort_ = VK_NULL_HANDLE;
    VkPipeline pipeTileBoundary_ = VK_NULL_HANDLE;
    VkPipeline pipeRender_ = VK_NULL_HANDLE;

    // ── Pipeline layouts ─────────────────────────────────────────────────
    VkPipelineLayout layoutPrecompCov3d_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutPreprocess_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutPrefixSum_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutPreprocessSort_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutHist_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutSort_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutTileBoundary_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutRender_ = VK_NULL_HANDLE;

    // ── Descriptor set layouts ───────────────────────────────────────────
    VkDescriptorSetLayout dslPrecompCov3d_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPreprocessSet0_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPreprocessSet1_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPrefixSum_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPreprocessSort_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslHist_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslSort_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslTileBoundary_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslRenderSet0_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslRenderSet1_ = VK_NULL_HANDLE;

    // ── Descriptor pool & sets ───────────────────────────────────────────
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    VkDescriptorSet dsPrecompCov3d_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPreprocessSet0_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPreprocessSet1_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPrefixSum_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPreprocessSort_ = VK_NULL_HANDLE;
    VkDescriptorSet dsHistEven_ = VK_NULL_HANDLE;
    VkDescriptorSet dsHistOdd_ = VK_NULL_HANDLE;
    VkDescriptorSet dsSortEvenToOdd_ = VK_NULL_HANDLE;
    VkDescriptorSet dsSortOddToEven_ = VK_NULL_HANDLE;
    VkDescriptorSet dsTileBoundary_ = VK_NULL_HANDLE;
    VkDescriptorSet dsRenderSet0_ = VK_NULL_HANDLE;
    VkDescriptorSet dsRenderSet1_ = VK_NULL_HANDLE;

    // ── Radix sort sizing ────────────────────────────────────────────────
    uint32_t numRadixSortBlocksPerWG_ = 256;  // Apple default
    uint32_t numSortWorkgroups_ = 0;

    // ── CPU pick data (compact: 20 bytes/gaussian) ─────────────────────
    std::vector<GsPickData> pickData_;

    // Outlier-trimmed scene bbox (splat centers), cached at scene load.
    // Used per-eye to compute the linear 16-bit sort-depth quantization range
    // (see renderEye) — floaters outside it clamp to the range ends.
    float sceneBBoxMin_[3] = {0, 0, 0};
    float sceneBBoxMax_[3] = {0, 0, 0};
    bool sceneBBoxValid_ = false;

    // ── Private methods ──────────────────────────────────────────────────
    bool createPipelines();
    bool createBuffers();
    bool createDescriptorSets();
    // Reallocate sort buffers + sort histogram to hold at least requiredCapacity
    // tile fragments, and re-write the descriptor sets that bind them. Caller
    // must have wait-idled the queue. No-op if requiredCapacity fits.
    void growSortBuffers(uint32_t requiredCapacity);
    void dispatchPrecompCov3d();
    void updateUniforms(const float viewMatrix[16], const float projMatrix[16],
                        uint32_t vpWidth, uint32_t vpHeight,
                        float clipNear = 0.0f, float clipFar = 0.0f,
                        float clipFadeFrac = 0.0f);
    void cleanupScene();
};

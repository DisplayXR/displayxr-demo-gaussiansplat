// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Adreno/TBDR-native Gaussian-splat renderer (graphics pipeline).
 *
 * A from-scratch, mobile-specific splat renderer that replaces the desktop
 * compute compositor (8 compute shaders: expand each gaussian into per-tile
 * fragments → ~1.4M-element global radix sort → per-pixel composite into a
 * storage image, with a GPU→CPU readback between stages). That design is built
 * for an immediate-mode desktop GPU; it is exactly wrong for a tile-based
 * deferred (TBDR) mobile GPU like Adreno, where the expensive parts are the
 * huge global sort and the storage-image scatter/gather.
 *
 * This renderer instead does what the hardware is built for:
 *   1. preprocess (compute, N threads): project each gaussian, compute its 2D
 *      conic, SH colour, view depth and pixel radius.            (adreno_preprocess.comp)
 *   2. keygen + radix sort over N gaussians (NOT ~1.4M fragments) by view
 *      depth, far→near.                              (splat_keys.comp + hist/sort.comp)
 *   3. draw one INSTANCED alpha-blended quad per gaussian in back-to-front
 *      order; the rasterizer + ROP composite in on-chip tile memory (GMEM).
 *                                                            (splat.vert/.frag)
 * No prefix-sum, no per-tile fragment expansion, no tile_boundary, no
 * render.comp, no storage-image composite, no GPU→CPU readback. The radix sort
 * runs over N (sort buffers sized N, not N×8), so it dispatches ~7× fewer
 * workgroups than the desktop path's fragment sort.
 *
 * An internal render-scale (default 0.6 on Android) drives the whole pipeline
 * at a fraction of the eye resolution then linear-upscales the blit — the
 * dominant cost is per-fragment overdraw, so this is a near-linear fps win.
 *
 * The public surface is the subset the Android leg uses; desktop legs keep the
 * shared GsRenderer.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>
#include <vector>
#include "gs_vulkan_utils.h"

struct GsAdrenoRenderer {
    // Initialize with the OpenXR runtime's Vulkan resources. renderWidth/Height
    // are the per-eye swapchain dims (the render-scale is applied internally).
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    // Load a .spz or .ply Gaussian-splat scene: parse, upload, precompute 3D
    // covariance, build the per-scene GPU pipeline.
    bool loadScene(const char* scenePath);

    bool hasScene() const { return sceneLoaded_; }
    const std::string& scenePath() const { return loadedScenePath_; }
    uint32_t gaussianCount() const { return numGaussians_; }

    // Robust scene centroid + per-axis extent (outlier-trimmed): per axis takes
    // the loPct/hiPct quantile positions, center=midpoint, extent=hi-lo. Used by
    // the demo for auto-framing. Returns false if no scene is loaded.
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;

    // Render one eye to a viewport region of a swapchain image. viewMatrix and
    // projMatrix are column-major float[16]. clipNearViewSpace/clipFarViewSpace
    // (>0) cull splats outside the view-space forward-depth window; clipFadeFrac
    // softens the near cut into an opacity rolloff. transparentBg is accepted
    // for API compatibility (the graphics path always emits premultiplied alpha,
    // so background pixels are already 0). Signature matches GsRenderer::renderEye.
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

    void cleanup();
    ~GsAdrenoRenderer();

private:
    // ── Core Vulkan handles (not owned, from the OpenXR runtime) ──
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t width_ = 0;        // full per-eye dims
    uint32_t height_ = 0;
    uint32_t subgroupSize_ = 32;

    bool initialized_ = false;
    bool sceneLoaded_ = false;
    std::string loadedScenePath_;
    uint32_t numGaussians_ = 0;

    // Internal render scale (1.0 = full res). Default 0.6 on Android — the
    // pipeline runs at scale×eye dims then linear-upscales the blit.
    float renderScale_ = 1.0f;
    float keepFrac_ = 1.0f;     // load-time decimation (1.0 = keep all)

    // ── Frame pipelining ring ──
    // Each renderEye submits with a per-slot fence instead of vkQueueWaitIdle,
    // so the app's GPU work no longer blocks the CPU or serializes against the
    // runtime/DP weave — the next eye/frame can be queued while this one runs
    // (the documented #1 gauss lever: a light app hits ~32 fps through the same
    // OOP runtime/DP because it doesn't idle-block). A slot's fence is waited
    // only when that slot is reused (kFrameRing submissions later), so in steady
    // state the wait is already satisfied.
    static constexpr uint32_t kFrameRing = 3;
    VkCommandBuffer ringCmd_[kFrameRing] = {};
    VkFence ringFence_[kFrameRing] = {};
    bool ringReady_ = false;

    // ── GPU timestamp profiling (per-stage, logged every kTsLogPeriod eyes) ──
    static constexpr uint32_t kNumTimestamps = 6;  // top,preproc,keygen,radix,draw,blit
    VkQueryPool tsPool_[kFrameRing] = {};  // per-slot (read when the slot's fence signals)
    float timestampPeriod_ = 0.0f;
    uint32_t tsValidBits_ = 0;
    uint64_t frameCounter_ = 0;

    // ── Radix sort sizing (32-bit keys, no shaderInt64 — Adreno) ──
    uint32_t numRadixBlocksPerWG_ = 256;
    uint32_t numSortWorkgroups_ = 0;

    // ── GPU buffers ──
    GsBuffer vertexBuffer_;     // N × 240
    GsBuffer cov3dBuffer_;      // N × 24
    GsBuffer uniformBuffer_[kFrameRing];  // 176 (host-visible), one per ring slot — eyes
                                          // in flight must not share (else left-eye stutter)
    GsBuffer attrBuffer_;       // N × 64 (VertexAttribute)
    GsBuffer keysEvenBuffer_;   // N × 4
    GsBuffer keysOddBuffer_;    // N × 4
    GsBuffer valsEvenBuffer_;   // N × 4 (gaussian indices)
    GsBuffer valsOddBuffer_;    // N × 4
    GsBuffer histBuffer_;       // numSortWorkgroups_ × 256 × 4

    // ── Internal scaled render target ──
    GsImage renderImage_;       // R8G8B8A8_UNORM, width_ × height_ (full; scaled region used)

    // ── Compute pipelines (5) ──
    VkPipeline pipeCov3d_ = VK_NULL_HANDLE;
    VkPipeline pipePreprocess_ = VK_NULL_HANDLE;
    VkPipeline pipeKeys_ = VK_NULL_HANDLE;
    VkPipeline pipeHist_ = VK_NULL_HANDLE;
    VkPipeline pipeSort_ = VK_NULL_HANDLE;
    // ── Graphics pipeline (instanced splat quads) ──
    VkPipeline pipeSplat_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

    VkPipelineLayout layoutCov3d_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutPreprocess_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutKeys_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutHist_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutSort_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutSplat_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dslCov3d_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPreprocessSet0_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPreprocessSet1_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslKeys_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslHist_ = VK_NULL_HANDLE;    // 2 bindings: keys, histograms
    VkDescriptorSetLayout dslSort_ = VK_NULL_HANDLE;    // 5 bindings: keys in/out, vals in/out, histograms
    VkDescriptorSetLayout dslSplat_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet dsCov3d_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPreprocessSet0_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPreprocessSet1_[kFrameRing] = {};  // per-slot (binds uniformBuffer_[slot])
    VkDescriptorSet dsKeys_ = VK_NULL_HANDLE;
    VkDescriptorSet dsHistEven_ = VK_NULL_HANDLE;   // keysEven→hist
    VkDescriptorSet dsHistOdd_ = VK_NULL_HANDLE;    // keysOdd→hist
    VkDescriptorSet dsSortEvenToOdd_ = VK_NULL_HANDLE;
    VkDescriptorSet dsSortOddToEven_ = VK_NULL_HANDLE;
    VkDescriptorSet dsSplat_ = VK_NULL_HANDLE;

    // ── CPU-side scene data (auto-framing + depth-quant range) ──
    std::vector<float> posX_, posY_, posZ_;  // gaussian centers
    float sceneBBoxMin_[3] = {0, 0, 0};
    float sceneBBoxMax_[3] = {0, 0, 0};
    bool sceneBBoxValid_ = false;

    // ── Private helpers ──
    bool createSceneResources();
    void dispatchCov3d();
    void updateUniforms(uint32_t slot, const float viewMatrix[16], const float projMatrix[16],
                        uint32_t vpWidth, uint32_t vpHeight,
                        float clipNear, float clipFar, float clipFadeFrac);
    void cleanupScene();
};

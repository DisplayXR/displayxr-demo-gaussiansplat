// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
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

#include "gs_scene_loader.h"
#include "gs_camera_rig.h"  // GsSceneMeasurements
#include "gs_scene_fit.h"   // GsFitBounds  // GsVertex, GsSceneCamera
#include "gs_raycast.h"     // GsSplatRayCaster (depth-aware cursor)

#include <vulkan/vulkan.h>
#include <string>
#include <cstdint>
#include <vector>
#include "gs_perf_knobs.h"
#include "gs_vulkan_utils.h"
#include "vk_frame_ring.h"  // displayxr-common (displayxr::rules) >= v2.28.0

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

    //! One-line reason the last loadScene() returned false (empty after a
    //! success). Never a crash substitute for logging: the caller is expected
    //! to surface this to the user, since a chrome-free transparent window has
    //! no other way to say why nothing appeared.
    const std::string& lastLoadError() const { return lastLoadError_; }

    //! The recording camera the CURRENT scene declared, or a default-constructed
    //! (`present == false`) one when it declared none. Only a SOG bundle with a
    //! `camera` block fills it; its presence is what selects the viewer's
    //! camera rig. Valid from the moment loadScene() returns until the next load.
    const GsSceneCamera& sceneCamera() const { return sceneCamera_; }

    //! What the current cloud says about itself — recovered intrinsics and
    //! median-disparity depth — measured before decimation. Feeds the camera
    //! rig's waterfall; `valid == false` when nothing could be measured.
    //! Where the DISPLAY rig should centre, and how big the subject is, from
    //! the shared scene-fit module. Measured once at load (see the note at
    //! the call site); `valid == false` when there was nothing to measure.
    const GsFitBounds& fitBounds() const { return fitBounds_; }

    //! The same, computed the way THIS leg historically did it — the
    //! `--fit=legacy` A/B. Equal to fitBounds() on the compute leg; the
    //! [5%, 95%] percentile box on the graphics leg.
    const GsFitBounds& fitBoundsLegacy() const { return fitBoundsLegacy_; }

    const GsSceneMeasurements& sceneMeasurements() const { return sceneMeasurements_; }

    //! Median-disparity depth in metres, 0 when unknown. Shorthand for the
    //! field of sceneMeasurements() the rig uses most.
    float sceneMedianForwardDepthM() const { return sceneMeasurements_.medianDepthM; }
    uint32_t gaussianCount() const { return numGaussians_; }

    // Robust scene centroid + per-axis extent (outlier-trimmed): per axis takes
    // the loPct/hiPct quantile positions, center=midpoint, extent=hi-lo. Used by
    // the demo for auto-framing. Returns false if no scene is loaded.
    //! Full scene AABB over every gaussian CENTRE (no percentile trim).
    //! Mirrors GsRenderer::getSceneBBox. Used to measure how much the robust
    //! percentile box under-reports the visible extent.
    bool getSceneBBox(float outMin[3], float outMax[3]) const;

    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;

    bool pickGaussian(const float rayOrigin[3], const float rayDir[3],
                      float hitPos[3], float maxDistance = 100.0f,
                      const float viewMatrix[16] = nullptr,
                      float clipNear = 0.0f, float clipFar = 0.0f) const;

    //! Depth-aware cursor (XR_DXR_cursor_depth) — mirrors GsRenderer: arm
    //! BEFORE loadScene() and each load hands its CPU vertex copy to a worker
    //! that builds the splat ray cast (gs_raycast.h). Off (the default, and
    //! always on Android) = no extra memory and no work.
    void setCursorRayCast(bool on) { cursorRayCast_ = on; }
    const GsSplatRayCaster& rayCaster() const { return rayCaster_; }

    // ── Frame bracket (INV-4.10, runtime#1917) ──
    //
    // Bracket every view of one OpenXR frame with beginFrame()/endFrame().
    // Nothing on this path ever blocks on recent GPU work: beginFrame() waits
    // only the fence of the frame that used the same ring slot N frames ago
    // (normally long signaled), command buffers are allocated once, and each
    // view owns its own sort buffers / render target / blit scratch within
    // the slot, so views never wait on each other.
    //
    // Each view is SUBMITTED AS SOON AS IT IS RECORDED (no fence, no wait),
    // and endFrame() makes one more, empty submit carrying the slot fence —
    // so this is N+1 unwaited submits per frame, a deliberate exemption from
    // INV-4.10's "one submit per frame". Measured on Adreno (PR #141 review):
    // batching both views into ONE frame-end submit with a full barrier
    // between them cost 14 % (phone) / 47 % (tablet) fps — the GPU sat idle
    // while the CPU recorded the whole frame, then ran view 1's sort strictly
    // after view 0's draw, and the governor clocked down on the bubbles. The
    // invariant's point is never blocking, which this keeps.
    //
    // Call endFrame() BEFORE xrReleaseSwapchainImage: the runtime composes on
    // the same VkQueue, so the app's work must be queued ahead of it.
    // renderEye() outside a bracket still works (it opens and closes a frame
    // of its own), so an unconverted caller keeps rendering correctly.
    bool beginFrame();
    void endFrame();

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

    // ── Pre-cull silhouette coverage (#112, XR_DXR_depth_budget v4) ──────
    //
    // Mirror of GsRenderer's API, same contract — see gs_renderer.h for the
    // full rationale. A small occupancy raster of where the gaussians land on
    // screen as they would render at UNRESTRICTED rear-depth budget, written
    // by adreno_preprocess.comp BEFORE its far cull, so the depth-budget
    // content mask is not a function of the budget the runtime just published.
    //
    // Windows-on-ARM is the leg that needs it here (this renderer is the TBDR
    // one, and that is a transparent-window Windows target). Android never
    // arms it: there is no background preview there and the budget is pinned
    // clipped, so the Android path is bit-for-bit unchanged.
    void setSilhouetteCoverage(bool on) { silhouetteOn_ = on; }
    bool silhouetteCoverage() const { return silhouetteOn_; }

    //! Make renderEye's `transparentBg` flag decide the output alpha, as it
    //! does in GsRenderer: false -> alpha 1 everywhere (the clear starts at
    //! alpha 1 and the premultiplied blend keeps it there), true -> coverage
    //! alpha 1 - T. Off by default, which keeps the historical behaviour
    //! (coverage alpha whatever the flag) for the legs that rely on it; the
    //! Linux leg turns it on because its session is transparent-capable and
    //! a PRE_MULTIPLIED surface shows alpha < 1 even in opaque mode.
    void setHonorTransparentBg(bool on) { honorTransparentBg_ = on; }
    uint32_t silhouetteCoverageWidth() const { return coverageW_; }
    uint32_t silhouetteCoverageHeight() const { return coverageH_; }

    // Read the accumulated coverage and clear it for the next frame in one
    // submission; `out` becomes width*height bytes, row-major, top-left
    // origin, nonzero = covered. Call once per frame after the last view: the
    // image then holds the union over exactly that frame's views. False when
    // no scene is loaded or the coverage was never armed.
    //
    // Call it AFTER endFrame() (an open frame is ended first, defensively).
    //
    // NOTE: this WAITS FOR EVERY IN-FLIGHT FRAME, which collapses this
    // renderer's frame ring for that frame. That is the price of a synchronous
    // mask and the reason nothing calls it unless the app is actually chaining
    // one (Android never does).
    bool readSilhouetteCoverage(std::vector<uint8_t>& out);

    void cleanup();
    ~GsAdrenoRenderer();

private:
    // ── Core Vulkan handles (not owned, from the OpenXR runtime) ──
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t width_ = 0;        // full per-eye dims
    uint32_t height_ = 0;
    uint32_t subgroupSize_ = 32;

    bool initialized_ = false;
    bool sceneLoaded_ = false;
    std::string loadedScenePath_;
    std::string lastLoadError_;
    GsSceneCamera sceneCamera_;
    GsSceneMeasurements sceneMeasurements_;
    GsFitBounds fitBounds_;
    GsFitBounds fitBoundsLegacy_;
    uint32_t numGaussians_ = 0;

    // Internal render scale (1.0 = full res). Default 1.0 everywhere — the
    // pipeline runs at scale×eye dims then linear-upscales the blit.
    float renderScale_ = 1.0f;
    float keepFrac_ = 1.0f;     // load-time decimation (1.0 = keep all)

    // Resolved DXR_GS_* levers (gs_perf_knobs.h). Read once in init(); the
    // per-frame path only reads the already-resolved fields, never the
    // environment.
    gsperf::Knobs knobs_;

    // ── One-shot DXR_GS_DUMP capture ──
    // A synchronous readback of the internal render target, written as a PNG
    // exactly once (at eye DXR_GS_DUMP_FRAME). It exists to PROVE bit-exactness
    // of the extent/cull levers by diffing two runs, so it deliberately reads
    // the pre-blit, pre-upscale image — the only surface where "same pixels"
    // is a meaningful claim.
    GsBuffer dumpHost_;
    bool dumpDone_ = false;
    uint32_t dumpW_ = 0, dumpH_ = 0;

    // ── Frame ring (dxr::VkFrameRing, displayxr-common) ──
    // One slot per FRAME in flight (default 2 here; DXR_VK_FRAMES_IN_FLIGHT /
    // debug.dxr.gs.fif = 1..8 for A/B). The ring owns the slot fence, the
    // per-view UBO slices and the one-off submitImmediate() path. Per slot,
    // the renderer adds one command buffer per view (allocated once) and
    // kViewSets private buffer sets — the old per-view ring's shape (views
    // independent on the GPU, submitted as recorded) without its CPU waits.
    // Buffer set index = slot * kViewSets + (view % kViewSets); a view beyond
    // kViewSets shares a set with an earlier view of the same frame and is
    // ordered after it by a barrier at the top of its command buffer.
    static constexpr uint32_t kMaxRing = dxr::detail::kMaxFramesInFlight;
    static constexpr uint32_t kViewSets = 2;            // every Android panel; desktop SBS
    static constexpr uint32_t kMaxSets = kMaxRing * kViewSets;
    static constexpr uint32_t kMaxViewsPerFrame = 8;
    static constexpr uint32_t kDefaultFramesInFlight = 2;  // 2 x 2 sets ~= the old 3
    dxr::VkFrameRing ring_;
    uint32_t ringN_ = 0;          // slots actually allocated (ring_.framesInFlight())
    uint32_t numSets_ = 0;        // ringN_ * kViewSets buffer sets per scene
    VkCommandPool viewCmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer viewCmd_[kMaxRing][kMaxViewsPerFrame] = {};  // allocated once in init()
    uint32_t viewInFrame_ = 0;    // views recorded into the open frame
    uint64_t frameIndex_ = 0;     // frames begun
    bool dumpPending_ = false;    // DXR_GS_DUMP copy submitted in the open frame
    bool warnedViewOverflow_ = false;
    bool warnedSubmit_ = false;
    VkImage frameLastImage_ = VK_NULL_HANDLE;  // swapchain image the previous view wrote

    // ── GPU timestamp profiling (per-stage, logged every kTsLogPeriod eyes) ──
    static constexpr uint32_t kNumTimestamps = 6;  // top,preproc,keygen,radix,draw,blit
    VkQueryPool tsPool_[kMaxRing] = {};  // per-slot, view 0 timed (read when the slot's fence signals)
    bool tsWritten_[kMaxRing] = {};      // slot's pool holds a complete view-0 record
    uint32_t tsRw_[kMaxRing] = {}, tsRh_[kMaxRing] = {};
    float timestampPeriod_ = 0.0f;
    uint32_t tsValidBits_ = 0;
    uint64_t frameCounter_ = 0;  // EYES rendered (DXR_GS_DUMP_FRAME counts eyes)

    // ── Radix sort sizing (32-bit keys, no shaderInt64 — Adreno) ──
    uint32_t numRadixBlocksPerWG_ = 256;
    uint32_t numSortWorkgroups_ = 0;

    // ── GPU buffers ──
    // Load-time, read-only per frame → single instance (shared across the ring
    // is safe; no frame ever writes them).
    GsBuffer vertexBuffer_;     // N × 240
    GsBuffer cov3dBuffer_;      // N × 24
    // Per-frame mutated → one instance PER BUFFER SET (slot x view, above).
    // Up to ringN_ frames are GPU-in-flight at once and a frame's views run
    // concurrently, so any buffer written each frame must be private to its
    // set — otherwise a later preprocess/keygen/sort clobbers data an earlier
    // draw is still reading (WAR/RAW).
    // The per-view uniforms live in ring_'s UBO (slot x view slices, bound
    // with a dynamic offset).
    GsBuffer attrBuffer_[kMaxSets];       // N × 64 (VertexAttribute)
    GsBuffer keysEvenBuffer_[kMaxSets];   // N × 4
    GsBuffer keysOddBuffer_[kMaxSets];    // N × 4
    GsBuffer valsEvenBuffer_[kMaxSets];   // N × 4 (gaussian indices)
    GsBuffer valsOddBuffer_[kMaxSets];    // N × 4
    GsBuffer histBuffer_[kMaxSets];       // numSortWorkgroups_ × 256 × 4

    // ── Compacted indirect draw (DXR_GS_COMPACT) ──
    // splat_keys.comp atomically counts the gaussians that survived preprocess
    // into this VkDrawIndirectCommand, and the draw is issued from it. Because
    // the culled key is reserved to sort strictly last, the survivors are
    // exactly the prefix [0, instanceCount) of the sorted payload — so a
    // culled gaussian costs no vertex shader at all, instead of a 64-byte
    // random gather into attr[] followed by a degenerate quad.
    GsBuffer drawCmdBuffer_[kMaxSets];    // 16 B, STORAGE | INDIRECT
    GsBuffer drawCmdHost_[kMaxSets];      // 16 B host-visible, for the GS_TS line
    uint32_t lastDrawnInstances_ = 0;

    // ── Internal scaled render target (per slot: draw target + blit source) ──
    GsImage renderImage_[kMaxSets];       // R8G8B8A8_UNORM, width_ × height_ (full; scaled region used)
    GsImage swapScratch_[kMaxSets];       // UNORM sibling of an sRGB swapchain (gsCmdBlitToSwapchain); empty otherwise

    // ── Pre-cull silhouette coverage (#112) ──
    // Single image, NOT per slot: the readback drains the queue before it
    // copies, so "everything submitted since the last read" is a well-defined
    // accumulation window, and the union across the frame's views is the thing
    // we actually want. ~8 KB.
    GsImage coverageImage_;                 // R8_UINT, coverageW_ × coverageH_
    GsBuffer coverageHost_;                 // host-visible readback staging
    uint32_t coverageW_ = 0;
    uint32_t coverageH_ = 0;
    bool silhouetteOn_ = false;
    bool honorTransparentBg_ = false; //!< see setHonorTransparentBg

    // ── Compute pipelines (5) ──
    VkPipeline pipeCov3d_ = VK_NULL_HANDLE;
    VkPipeline pipePreprocess_ = VK_NULL_HANDLE;
    VkPipeline pipeKeys_ = VK_NULL_HANDLE;
    VkPipeline pipeHist_ = VK_NULL_HANDLE;
    VkPipeline pipeSort_ = VK_NULL_HANDLE;
    // ── Graphics pipeline (instanced splat quads) ──
    VkPipeline pipeSplat_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;            // format-compatible with every slot's framebuffer
    VkFramebuffer framebuffer_[kMaxSets] = {};          // wraps renderImage_[slot].view

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
    // Single sets reference only load-time read-only buffers (vertex, cov3d).
    VkDescriptorSet dsCov3d_ = VK_NULL_HANDLE;
    VkDescriptorSet dsPreprocessSet0_ = VK_NULL_HANDLE;
    // Per-slot sets reference per-slot (uniform/attr/keys/vals/hist) buffers.
    VkDescriptorSet dsPreprocessSet1_[kMaxSets] = {};
    VkDescriptorSet dsKeys_[kMaxSets] = {};
    VkDescriptorSet dsHistEven_[kMaxSets] = {};   // keysEven→hist
    VkDescriptorSet dsHistOdd_[kMaxSets] = {};    // keysOdd→hist
    VkDescriptorSet dsSortEvenToOdd_[kMaxSets] = {};
    VkDescriptorSet dsSortOddToEven_[kMaxSets] = {};
    VkDescriptorSet dsSplat_[kMaxSets] = {};

    // ── CPU-side scene data (auto-framing + depth-quant range) ──
    std::vector<float> posX_, posY_, posZ_;  // gaussian centers
    bool cursorRayCast_ = false;             // see setCursorRayCast()
    GsSplatRayCaster rayCaster_;
    float sceneBBoxMin_[3] = {0, 0, 0};
    float sceneBBoxMax_[3] = {0, 0, 0};
    bool sceneBBoxValid_ = false;

    // ── Private helpers ──
    bool createSceneResources();
    void dispatchCov3d();
    // Record UNDEFINED -> clear-to-zero -> GENERAL for coverageImage_ into an
    // already-begun command buffer.
    void cmdInitCoverageImage(VkCommandBuffer cmd);
    void updateUniforms(void* dst,
                        const float viewMatrix[16], const float projMatrix[16],
                        uint32_t vpWidth, uint32_t vpHeight,
                        float clipNear, float clipFar, float clipFadeFrac);
    void cleanupScene();
};

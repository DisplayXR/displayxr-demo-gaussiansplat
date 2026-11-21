// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Run-time splat-renderer selection: one of the two renderers, chosen
 *         from the ACTUAL GPU once the VkPhysicalDevice is known.
 *
 * gs_renderer_select.h used to pick at COMPILE time from the CPU architecture.
 * That is the wrong key: the shipped Windows build is x64, and an x64 exe runs
 * under emulation on Windows-on-ARM, where the GPU is an Adreno. It got the
 * compute compositor there, the path gs_renderer_select.h itself calls
 * "exactly wrong" for a tile-based GPU. The compute dispatches monopolised the
 * GPU, the runtime's 60 Hz re-weave queued behind them, and head tracking and
 * the transparent-window silhouette fell out of step with the content. A
 * native ARM64 exe is not the way out: the (x64) runtime loads in-process.
 *
 * GsAutoRenderer owns exactly one of GsRenderer / GsAdrenoRenderer and forwards
 * the API the desktop legs use. The choice is made once, in init():
 *
 *   1. DXR_GS_RENDERER=compute|graphics   (environment; A/B and escape hatch)
 *   2. tile-based GPU vendor -> graphics  (Qualcomm 0x5143, Apple 0x106B,
 *                                          ARM Mali 0x13B5, Imagination 0x1010)
 *   3. anything else         -> compute
 *
 * A compile-time pin (GS_RENDERER_GRAPHICS / GS_RENDERER_COMPUTE, i.e. CMake
 * -DGS_RENDERER=GRAPHICS|COMPUTE) bypasses this type altogether (see
 * gs_renderer_select.h), so a pinned build ignores the environment variable.
 *
 * One contract through the wrapper, whichever renderer is behind it:
 * renderEye()'s `transparentBg` decides the output alpha (false -> 1
 * everywhere, true -> coverage 1 - T). That is GsRenderer's native behaviour;
 * the graphics renderer only does it when asked (setHonorTransparentBg), so
 * init() asks. Without that an opaque-mode frame would carry coverage alpha
 * into a transparent-capable session and show the desktop through the gaps.
 *
 * Not used on Android (its harness includes gs_adreno_renderer.h directly).
 */
#pragma once

#include "gs_renderer.h"
#include "gs_adreno_renderer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class GsRendererKind { Compute, Graphics };

//! "compute" / "graphics".
const char* gsRendererKindName(GsRendererKind kind);

//! True for the GPU vendors whose hardware is tile-based (deferred) rendering.
bool gsIsTileBasedGpuVendor(uint32_t vendorID);

//! Pick the renderer for this device. `envOverride` is the value of
//! DXR_GS_RENDERER (nullptr / empty / unrecognised = no override). `outWhy`
//! receives a one-line human-readable reason naming the device.
GsRendererKind gsChooseRendererKind(VkPhysicalDevice physicalDevice,
                                    const char* envOverride,
                                    std::string& outWhy);

struct GsAutoRenderer {
    // Chooses the renderer (see the file comment), then initialises it. Every
    // other method is a harmless no-op / default before it.
    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkQueue queue,
              uint32_t queueFamilyIndex,
              uint32_t renderWidth,
              uint32_t renderHeight);

    //! Which renderer init() chose. Compute before init().
    GsRendererKind kind() const { return kind_; }
    //! "Splat renderer: graphics (...why...)", the line the app logs once.
    //! Empty before init().
    const std::string& selectionSummary() const { return summary_; }

    bool loadScene(const char* scenePath);
    //! Compute renderer only; false (nothing loaded) on the graphics renderer,
    //! which renders from a loaded scene file only.
    bool loadDebugScene(float x, float y, float z, float radius);
    bool hasScene() const;
    const std::string& scenePath() const;
    const std::string& lastLoadError() const;
    const GsSceneCamera& sceneCamera() const;
    const GsFitBounds& fitBounds() const;
    const GsFitBounds& fitBoundsLegacy() const;
    const GsSceneMeasurements& sceneMeasurements() const;
    float sceneMedianForwardDepthM() const { return sceneMeasurements().medianDepthM; }
    uint32_t gaussianCount() const;

    bool getSceneBBox(float outMin[3], float outMax[3]) const;
    bool getRobustSceneBounds(float loPct, float hiPct,
                              float outCenter[3], float outExtent[3]) const;
    bool pickGaussian(const float rayOrigin[3], const float rayDir[3],
                      float hitPos[3], float maxDistance = 100.0f,
                      const float viewMatrix[16] = nullptr,
                      float clipNear = 0.0f, float clipFar = 0.0f) const;

    void setCursorRayCast(bool on);
    //! Valid until cleanup() once init() has run (the caller may keep the
    //! reference). Before init() it is a shared empty caster.
    const GsSplatRayCaster& rayCaster() const;

    // Frame bracket: the graphics renderer's per-frame ring; a no-op on compute.
    bool beginFrame();
    void endFrame();

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

    void setSilhouetteCoverage(bool on);
    bool silhouetteCoverage() const { return silhouetteOn_; }
    //! On by default here (see the file comment); a no-op on compute, which
    //! always honours the flag.
    void setHonorTransparentBg(bool on);
    //! Graphics renderer only (present pass, #143); a no-op on compute.
    void setSwapchainMutableFormat(bool on);
    uint32_t silhouetteCoverageWidth() const;
    uint32_t silhouetteCoverageHeight() const;
    bool readSilhouetteCoverage(std::vector<uint8_t>& out);

    void cleanup();

private:
    GsRendererKind kind_ = GsRendererKind::Compute;
    std::string summary_;
    // Exactly one is non-null after init(); both null before.
    std::unique_ptr<GsRenderer> compute_;
    std::unique_ptr<GsAdrenoRenderer> graphics_;
    // Setters called before init() are replayed onto the chosen renderer.
    bool cursorRayCast_ = false;
    bool silhouetteOn_ = false;
    bool honorTransparentBg_ = true;
    bool swapchainMutable_ = false;
};

// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Runtime splat-renderer selection (GPU-adaptive).
 *
 * gs_renderer_select.h picks ONE renderer at COMPILE time (a typedef + one
 * shader set in the binary). This header is the RUNTIME alternative for a
 * single binary that must serve both discrete and integrated GPUs (issue:
 * runtime GPU detection → renderer selection). Both concrete renderers ship in
 * the gs_renderer lib already, so both shader sets are in the binary; we choose
 * between them after the compositor hands us a physical device.
 *
 *   - GsRenderer         (compute compositor)  — immediate-mode DISCRETE GPUs.
 *   - GsAdrenoRenderer   (graphics pipeline)   — TBDR / INTEGRATED GPUs
 *                                                (Adreno, Apple Silicon, Intel Xe).
 *
 * The two classes already expose an identical method surface by convention, so
 * a single templated adapter (GsRendererAdapter<R>) wraps either behind the
 * abstract IGsRenderer — no change to the renderer classes themselves.
 *
 * Policy (gsPickRendererForDevice): DISCRETE_GPU → compute; everything else
 * (integrated / TBDR / virtual / software) → graphics. Optimus/hybrid note: the
 * caller MUST pass the VkPhysicalDeviceProperties of the device the DisplayXR
 * compositor selected (xrGetVulkanGraphicsDeviceKHR) — we render on THAT device
 * (the external-FD handoff mandates it), we do not shop for the fastest GPU.
 *
 * Compile-time override still wins when set (benchmark / pin a path): predefine
 * GS_RENDERER_GRAPHICS or GS_RENDERER_COMPUTE and the app skips detection.
 */
#pragma once

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

#include "gs_renderer.h"          // GsRenderer         (compute)
#include "gs_adreno_renderer.h"   // GsAdrenoRenderer   (graphics)

// Consumer-facing renderer surface shared by both concrete renderers. Kept to
// exactly what the standalone app drives; loadDebugScene() is intentionally
// absent (compute-only, and the app always ships a bundled scene).
struct IGsRenderer {
    virtual ~IGsRenderer() = default;

    virtual bool init(VkInstance instance,
                      VkPhysicalDevice physicalDevice,
                      VkDevice device,
                      VkQueue queue,
                      uint32_t queueFamilyIndex,
                      uint32_t renderWidth,
                      uint32_t renderHeight) = 0;

    virtual bool loadScene(const char* path) = 0;
    virtual bool hasScene() const = 0;
    virtual uint32_t gaussianCount() const = 0;

    virtual void renderEye(VkImage swapchainImage,
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
                           float clipFadeFrac = 0.0f) = 0;

    virtual void cleanup() = 0;
    virtual const char* name() const = 0;
};

// One template covers both renderers because their signatures are identical.
template <class R>
struct GsRendererAdapter final : IGsRenderer {
    R renderer;
    const char* label;
    explicit GsRendererAdapter(const char* n) : label(n) {}

    bool init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
              VkQueue queue, uint32_t queueFamilyIndex, uint32_t renderWidth,
              uint32_t renderHeight) override {
        return renderer.init(instance, physicalDevice, device, queue, queueFamilyIndex,
                             renderWidth, renderHeight);
    }
    bool loadScene(const char* path) override { return renderer.loadScene(path); }
    bool hasScene() const override { return renderer.hasScene(); }
    uint32_t gaussianCount() const override { return renderer.gaussianCount(); }
    void renderEye(VkImage swapchainImage, VkFormat swapchainFormat, uint32_t imageWidth,
                   uint32_t imageHeight, uint32_t viewportX, uint32_t viewportY,
                   uint32_t viewportWidth, uint32_t viewportHeight, const float viewMatrix[16],
                   const float projMatrix[16], bool transparentBg, float clipNearViewSpace,
                   float clipFarViewSpace, float clipFadeFrac) override {
        renderer.renderEye(swapchainImage, swapchainFormat, imageWidth, imageHeight, viewportX,
                           viewportY, viewportWidth, viewportHeight, viewMatrix, projMatrix,
                           transparentBg, clipNearViewSpace, clipFarViewSpace, clipFadeFrac);
    }
    void cleanup() override { renderer.cleanup(); }
    const char* name() const override { return label; }
};

enum class GsRendererKind { Compute, Graphics };

// Discrete → compute; integrated / TBDR / virtual / software → graphics.
inline GsRendererKind gsPickRendererForDevice(const VkPhysicalDeviceProperties& props) {
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        return GsRendererKind::Compute;
    }
    return GsRendererKind::Graphics;
}

inline std::unique_ptr<IGsRenderer> gsMakeRenderer(GsRendererKind kind) {
    if (kind == GsRendererKind::Compute) {
        return std::make_unique<GsRendererAdapter<GsRenderer>>("compute (GsRenderer)");
    }
    return std::make_unique<GsRendererAdapter<GsAdrenoRenderer>>("graphics (GsAdrenoRenderer)");
}

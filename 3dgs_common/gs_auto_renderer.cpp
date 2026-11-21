// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Run-time splat-renderer selection. See gs_auto_renderer.h.
 */

#include "gs_auto_renderer.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

const char* gsRendererKindName(GsRendererKind kind)
{
    return kind == GsRendererKind::Graphics ? "graphics" : "compute";
}

namespace {

const char* TileBasedVendorName(uint32_t vendorID)
{
    switch (vendorID) {
    case 0x5143: return "Qualcomm";
    case 0x106B: return "Apple";
    case 0x13B5: return "ARM";
    case 0x1010: return "Imagination";
    default:     return nullptr;
    }
}

bool EqualsNoCase(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b) {
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

} // namespace

bool gsIsTileBasedGpuVendor(uint32_t vendorID)
{
    return TileBasedVendorName(vendorID) != nullptr;
}

GsRendererKind gsChooseRendererKind(VkPhysicalDevice physicalDevice,
                                    const char* envOverride,
                                    std::string& outWhy)
{
    VkPhysicalDeviceProperties props = {};
    if (physicalDevice != VK_NULL_HANDLE) vkGetPhysicalDeviceProperties(physicalDevice, &props);
    const char* tbVendor = TileBasedVendorName(props.vendorID);

    char gpu[384];
    snprintf(gpu, sizeof(gpu), "GPU '%s', vendor 0x%04X%s%s", props.deviceName, props.vendorID,
             tbVendor ? " " : "", tbVendor ? tbVendor : "");

    outWhy.clear();
    if (envOverride && *envOverride) {
        const bool wantGraphics = EqualsNoCase(envOverride, "graphics");
        if (wantGraphics || EqualsNoCase(envOverride, "compute")) {
            outWhy = std::string("DXR_GS_RENDERER=") + envOverride + " override; " + gpu;
            return wantGraphics ? GsRendererKind::Graphics : GsRendererKind::Compute;
        }
        // Unrecognised: fall through to the GPU rule, and say so.
        outWhy = std::string("DXR_GS_RENDERER='") + envOverride +
                 "' not recognised (compute|graphics), ignored; ";
    }

    if (tbVendor) {
        outWhy += std::string("tile-based ") + gpu;
        return GsRendererKind::Graphics;
    }
    outWhy += std::string("not a tile-based vendor: ") + gpu;
    return GsRendererKind::Compute;
}

// Forward to whichever renderer exists; `fallback` before init().
#define GS_AUTO_FWD(call, fallback)            \
    do {                                       \
        if (graphics_) return graphics_->call; \
        if (compute_) return compute_->call;   \
        return fallback;                       \
    } while (0)
#define GS_AUTO_FWD_VOID(call)                 \
    do {                                       \
        if (graphics_) graphics_->call;        \
        else if (compute_) compute_->call;     \
    } while (0)

bool GsAutoRenderer::init(VkInstance instance,
                          VkPhysicalDevice physicalDevice,
                          VkDevice device,
                          VkQueue queue,
                          uint32_t queueFamilyIndex,
                          uint32_t renderWidth,
                          uint32_t renderHeight)
{
    cleanup();
    graphics_.reset();
    compute_.reset();

    std::string why;
    kind_ = gsChooseRendererKind(physicalDevice, std::getenv("DXR_GS_RENDERER"), why);
    summary_ = std::string("Splat renderer: ") + gsRendererKindName(kind_) + " (" + why + ")";
    printf("%s\n", summary_.c_str());

    if (kind_ == GsRendererKind::Graphics) {
        graphics_ = std::make_unique<GsAdrenoRenderer>();
        graphics_->setCursorRayCast(cursorRayCast_);
        graphics_->setSilhouetteCoverage(silhouetteOn_);
        graphics_->setHonorTransparentBg(honorTransparentBg_);
        graphics_->setSwapchainMutableFormat(swapchainMutable_);
        return graphics_->init(instance, physicalDevice, device, queue, queueFamilyIndex,
                               renderWidth, renderHeight);
    }
    compute_ = std::make_unique<GsRenderer>();
    compute_->setCursorRayCast(cursorRayCast_);
    compute_->setSilhouetteCoverage(silhouetteOn_);
    return compute_->init(instance, physicalDevice, device, queue, queueFamilyIndex,
                          renderWidth, renderHeight);
}

bool GsAutoRenderer::loadScene(const char* scenePath) { GS_AUTO_FWD(loadScene(scenePath), false); }

bool GsAutoRenderer::loadDebugScene(float x, float y, float z, float radius)
{
    return compute_ ? compute_->loadDebugScene(x, y, z, radius) : false;
}

bool GsAutoRenderer::hasScene() const { GS_AUTO_FWD(hasScene(), false); }

const std::string& GsAutoRenderer::scenePath() const
{
    static const std::string kEmpty;
    GS_AUTO_FWD(scenePath(), kEmpty);
}

const std::string& GsAutoRenderer::lastLoadError() const
{
    static const std::string kNotInit = "splat renderer not initialised";
    GS_AUTO_FWD(lastLoadError(), kNotInit);
}

const GsSceneCamera& GsAutoRenderer::sceneCamera() const
{
    static const GsSceneCamera kNone{};
    GS_AUTO_FWD(sceneCamera(), kNone);
}

const GsFitBounds& GsAutoRenderer::fitBounds() const
{
    static const GsFitBounds kNone{};
    GS_AUTO_FWD(fitBounds(), kNone);
}

const GsFitBounds& GsAutoRenderer::fitBoundsLegacy() const
{
    static const GsFitBounds kNone{};
    GS_AUTO_FWD(fitBoundsLegacy(), kNone);
}

const GsSceneMeasurements& GsAutoRenderer::sceneMeasurements() const
{
    static const GsSceneMeasurements kNone{};
    GS_AUTO_FWD(sceneMeasurements(), kNone);
}

uint32_t GsAutoRenderer::gaussianCount() const { GS_AUTO_FWD(gaussianCount(), 0u); }

bool GsAutoRenderer::getSceneBBox(float outMin[3], float outMax[3]) const
{
    GS_AUTO_FWD(getSceneBBox(outMin, outMax), false);
}

bool GsAutoRenderer::getRobustSceneBounds(float loPct, float hiPct,
                                          float outCenter[3], float outExtent[3]) const
{
    GS_AUTO_FWD(getRobustSceneBounds(loPct, hiPct, outCenter, outExtent), false);
}

bool GsAutoRenderer::pickGaussian(const float rayOrigin[3], const float rayDir[3],
                                  float hitPos[3], float maxDistance,
                                  const float viewMatrix[16],
                                  float clipNear, float clipFar) const
{
    GS_AUTO_FWD(pickGaussian(rayOrigin, rayDir, hitPos, maxDistance, viewMatrix, clipNear, clipFar),
                false);
}

void GsAutoRenderer::setCursorRayCast(bool on)
{
    cursorRayCast_ = on;
    GS_AUTO_FWD_VOID(setCursorRayCast(on));
}

const GsSplatRayCaster& GsAutoRenderer::rayCaster() const
{
    static const GsSplatRayCaster kEmpty;
    GS_AUTO_FWD(rayCaster(), kEmpty);
}

bool GsAutoRenderer::beginFrame() { GS_AUTO_FWD(beginFrame(), false); }
void GsAutoRenderer::endFrame() { GS_AUTO_FWD_VOID(endFrame()); }

void GsAutoRenderer::renderEye(VkImage swapchainImage, VkFormat swapchainFormat,
                               uint32_t imageWidth, uint32_t imageHeight,
                               uint32_t viewportX, uint32_t viewportY,
                               uint32_t viewportWidth, uint32_t viewportHeight,
                               const float viewMatrix[16], const float projMatrix[16],
                               bool transparentBg, float clipNearViewSpace,
                               float clipFarViewSpace, float clipFadeFrac)
{
    GS_AUTO_FWD_VOID(renderEye(swapchainImage, swapchainFormat, imageWidth, imageHeight,
                               viewportX, viewportY, viewportWidth, viewportHeight,
                               viewMatrix, projMatrix, transparentBg,
                               clipNearViewSpace, clipFarViewSpace, clipFadeFrac));
}

void GsAutoRenderer::setSilhouetteCoverage(bool on)
{
    silhouetteOn_ = on;
    GS_AUTO_FWD_VOID(setSilhouetteCoverage(on));
}

void GsAutoRenderer::setHonorTransparentBg(bool on)
{
    honorTransparentBg_ = on;
    if (graphics_) graphics_->setHonorTransparentBg(on);
}

void GsAutoRenderer::setSwapchainMutableFormat(bool on)
{
    swapchainMutable_ = on;
    if (graphics_) graphics_->setSwapchainMutableFormat(on);
}

uint32_t GsAutoRenderer::silhouetteCoverageWidth() const { GS_AUTO_FWD(silhouetteCoverageWidth(), 0u); }
uint32_t GsAutoRenderer::silhouetteCoverageHeight() const { GS_AUTO_FWD(silhouetteCoverageHeight(), 0u); }

bool GsAutoRenderer::readSilhouetteCoverage(std::vector<uint8_t>& out)
{
    GS_AUTO_FWD(readSilhouetteCoverage(out), false);
}

void GsAutoRenderer::cleanup()
{
    // The renderer object itself is kept: callers hold references into it
    // (rayCaster()), exactly as they did when it was a plain global.
    GS_AUTO_FWD_VOID(cleanup());
}

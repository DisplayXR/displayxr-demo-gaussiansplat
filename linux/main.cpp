// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Linux Vulkan OpenXR 3D Gaussian Splatting viewer (build-green port).
 *
 * Linux leg of displayxr-demo-gaussiansplat, issue #60 (M8 Linux epic,
 * meta runtime #699). Mirrors the macOS/Windows legs' OpenXR + Vulkan +
 * gs_renderer flow, but is deliberately a headless-driver harness:
 *
 *   * Window arm = HOSTED-NULL. The app passes NO window binding, so the
 *     runtime self-creates a window at native resolution. Desktop Linux
 *     app-provided-window support (XR_DXR_xlib_window_binding, runtime
 *     Phase 3a) is intentionally NOT wired here — see TODO(Phase 3) below.
 *     Faithful app-owned windowing + input + HUD is the on-screen pass,
 *     gated on the Linux runtime + a GPU + an X server (build-green only
 *     here — see docs/guides/linux-demo-port.md in the runtime repo).
 *
 *   * Renderer = the generic COMPUTE splat path (GsRenderer), selected on
 *     desktop x86_64 by gs_renderer_select.h. The Adreno/TBDR graphics path
 *     (gs_adreno_renderer) is the Android/Apple-Silicon default and is not
 *     used on Linux desktop.
 *
 * The frame loop is real (locate views → renderEye per tile → EndFrame) so
 * the build is not hollow: it exercises the whole gs_renderer link surface.
 * An auto-orbit camera stands in for the mouse/keyboard input the desktop
 * legs carry. ESC/window controls are the on-screen pass's job.
 */

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>

// TODO(Phase 3): swap the hosted-NULL window arm for the real desktop-Linux
// app-provided-window binding — XR_DXR_xlib_window_binding has landed in the
// runtime (Phase 3a, #660) with a working example at
// test_apps/cube_handle_vk_linux. Faithful ports pass a Display*/Window at
// xrCreateSession (as the cocoa/win32 legs pass their handles). Until the
// on-screen pass, hosted-NULL keeps the build-green harness runtime-agnostic
// and hardware-free. NOTE: the xlib binding header is not yet vendored in this
// repo's openxr_includes/ — vendor it (from displayxr-extensions) when wiring.

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

#include <unistd.h>
#include <limits.h>
#include <time.h>

#include "gs_renderer_select.h"   // GsActiveRenderer = compute path on desktop x86_64

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static GsActiveRenderer g_gsRenderer;

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// ============================================================================
// Inline math — column-major float[16] matrices (mirrors the macOS leg)
// ============================================================================

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];
    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye Z for the ZDP-anchored clip (mirrors macOS RigLocalEyeZ).
static float RigLocalEyeZ(const XrPosef& rig, const XrVector3f& eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// ============================================================================
// Executable-relative path (for the bundled scene)
// ============================================================================

static std::string ExecutableDir() {
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char* slash = strrchr(buf, '/');
        if (slash) *slash = '\0';
        return std::string(buf);
    }
    return ".";
}

// ============================================================================
// OpenXR session (hosted-NULL — no window binding on Linux build-green)
// ============================================================================

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    char systemName[256] = {};

    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    bool hasDisplayInfoExt = false;
    bool hasViewRigExt = false;
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerZ = 0.5f;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;

    PFN_xrEnumerateDisplayRenderingModesDXR pfnEnumerateDisplayRenderingModesEXT = nullptr;

    uint32_t renderingModeCount = 0;
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};
    uint32_t renderingModeTileColumns[8] = {};
    uint32_t renderingModeTileRows[8] = {};
    uint32_t currentRenderingMode = 1;   // default: first 3D mode

    uint32_t maxViewCount = 2;
};

static bool InitializeOpenXR(AppXrSession& xr) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& ext : exts) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) xr.hasViewRigExt = true;
    }
    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    if (xr.hasViewRigExt) enabled.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "SR3DGSOpenXRExtLinux",
            sizeof(ci.applicationInfo.applicationName) - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName) - 1);
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &xr.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

    { XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
      xrGetSystemProperties(xr.instance, xr.systemId, &sp);
      memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName)); }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
        sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
        }
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    LOG_INFO("OpenXR initialized: %s", xr.systemName);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static std::vector<std::string> SplitSpaceSeparated(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(' ', i);
        if (e == std::string::npos) e = s.size();
        std::string n = s.substr(i, e - i);
        if (!n.empty() && n[0] != '\0') out.push_back(n);
        i = e + 1;
    }
    return out;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames = SplitSpaceSeparated(extStr);
    std::vector<const char*> extPtrs;
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "SR3DGSOpenXRExtLinux";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)extPtrs.size();
    ci.ppEnabledExtensionNames = extPtrs.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& pd) {
    PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    LOG_INFO("GPU: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    storage = SplitSpaceSeparated(extStr);
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++)
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
    const std::vector<const char*>& exts, VkDevice& dev, VkQueue& queue) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = qfi; qi.queueCount = 1; qi.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures features = {};
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
    VkDevice dev, uint32_t qfi) {
    // Hosted-NULL: Vulkan binding only, no window-binding struct chained → the
    // runtime self-creates a window at native resolution.
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = &vkBinding; si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));

    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        if (XR_SUCCEEDED(xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr))
            && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (auto& m : modes) { m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR; m.next = nullptr; }
            if (XR_SUCCEEDED(xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data()))) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeTileColumns[i] = modes[i].tileColumns ? modes[i].tileColumns : 1;
                    xr.renderingModeTileRows[i] = modes[i].tileRows ? modes[i].tileRows : 1;
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, tiles=%ux%u, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY,
                        xr.renderingModeTileColumns[i], xr.renderingModeTileRows[i],
                        modes[i].hardwareDisplay3D);
                }
            }
        }
    }
    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));
    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, views.data());
    xr.maxViewCount = viewCount;
    LOG_INFO("View config: %u views reported by runtime", viewCount);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());

    int64_t selectedFmt = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0];
    for (auto f : fmts) {
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
    }

    // Worst-case-size across advertised modes (see swapchain-model.md).
    uint32_t w = views[0].recommendedImageRectWidth * 2;
    uint32_t h = views[0].recommendedImageRectHeight;
    if (xr.displayPixelWidth > 0 && xr.displayPixelHeight > 0) {
        w = xr.displayPixelWidth;
        h = xr.displayPixelHeight;
        if (xr.renderingModeCount > 0) {
            uint32_t maxW = 0, maxH = 0;
            for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                uint32_t aw = (uint32_t)((double)xr.renderingModeTileColumns[i] * xr.renderingModeScaleX[i] * (double)xr.displayPixelWidth);
                uint32_t ah = (uint32_t)((double)xr.renderingModeTileRows[i] * xr.renderingModeScaleY[i] * (double)xr.displayPixelHeight);
                if (aw > maxW) maxW = aw;
                if (ah > maxH) maxH = ah;
            }
            if (maxW > w) w = maxW;
            if (maxH > h) h = maxH;
        }
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;

    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, (long long)selectedFmt);
    return true;
}

static void PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = ssc->state;
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = xr.viewConfigType;
                xrBeginSession(xr.session, &bi);
                xr.sessionRunning = true;
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING ||
                       ssc->state == XR_SESSION_STATE_LOSS_PENDING) {
                xr.exitRequested = true;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(xr.session, nullptr, &fs))) return false;
    return XR_SUCCEEDED(xrBeginFrame(xr.session, nullptr));
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 1000000000;
    return XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &wi));
}

static void ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
}

static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = (viewCount > 0) ? 1 : 0;
    ei.layers = layers;
    xrEndFrame(xr.session, &ei);
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.localSpace) xrDestroySpace(xr.localSpace);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("DisplayXR Gaussian Splat Viewer (Linux build-green harness)");

    // Default rendering mode from SIM_DISPLAY_OUTPUT (matches the macOS leg).
    AppXrSession xr = {};
    { const char* mode = getenv("SIM_DISPLAY_OUTPUT");
      if (mode) {
          if (strcmp(mode, "sbs") == 0) xr.currentRenderingMode = 2;
          else if (strcmp(mode, "blend") == 0) xr.currentRenderingMode = 3;
          else xr.currentRenderingMode = 1; // anaglyph / first 3D mode
      } }

    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }
    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    { uint32_t count = xr.swapchain.imageCount;
      swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    if (!g_gsRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue, queueFamilyIndex,
                           xr.swapchain.width, xr.swapchain.height))
        LOG_WARN("3DGS renderer init failed");

    // Auto-load bundled butterfly.spz (copied next to the exe by CMake).
    { std::string scene = ExecutableDir() + "/butterfly.spz";
      if (argc > 1) scene = argv[1];
      if (g_gsRenderer.loadScene(scene.c_str()))
          LOG_INFO("Loaded scene: %s (%u gaussians)", scene.c_str(), g_gsRenderer.gaussianCount());
      else {
          LOG_WARN("No scene at %s — loading debug splat", scene.c_str());
          g_gsRenderer.loadDebugScene(0.0f, 0.0f, 0.0f, 0.1f);
      } }

    LOG_INFO("=== Entering main loop (auto-orbit; Ctrl+C to quit) ===");

    float yaw = 0.0f;
    const float virtualDisplayHeight = 1.5f;
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PollEvents(xr);
        if (!xr.sessionRunning) {
            struct timespec ts = {0, 10 * 1000 * 1000}; nanosleep(&ts, nullptr);
            continue;
        }

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        yaw += (6.2831853f / 20.0f) * dt;   // one revolution / 20 s

        XrFrameState fs;
        if (!BeginFrame(xr, fs)) continue;

        std::vector<XrCompositionLayerProjectionView> projectionViews;
        if (fs.shouldRender) {
            XrPosef cameraPose;
            quat_from_yaw_pitch(yaw, 0.0f, &cameraPose.orientation);
            cameraPose.position = {0, 0, 0};

            const bool useRig = xr.hasViewRigExt && xr.displayWidthM > 0 && xr.displayHeightM > 0;

            XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = xr.viewConfigType;
            locateInfo.displayTime = fs.predictedDisplayTime;
            locateInfo.space = xr.localSpace;

            XrDisplayRigDXR displayRig = {XR_TYPE_DISPLAY_RIG_DXR};
            if (useRig) {
                displayRig.pose = cameraPose;
                displayRig.virtualDisplayHeight = virtualDisplayHeight;
                displayRig.ipdFactor = 1.0f;
                displayRig.parallaxFactor = 1.0f;
                displayRig.perspectiveFactor = 1.0f;
                locateInfo.next = &displayRig;
            }

            XrViewState viewState = {XR_TYPE_VIEW_STATE};
            uint32_t runtimeViewCount = xr.maxViewCount > 8 ? 8 : (xr.maxViewCount ? xr.maxViewCount : 2);
            XrView views[8] = {};
            for (uint32_t v = 0; v < runtimeViewCount; v++) views[v].type = XR_TYPE_VIEW;

            XrResult locRes = xrLocateViews(xr.session, &locateInfo, &viewState,
                runtimeViewCount, &runtimeViewCount, views);

            if (XR_SUCCEEDED(locRes) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                uint32_t mode = xr.currentRenderingMode < xr.renderingModeCount ? xr.currentRenderingMode : 0;
                uint32_t modeViewCount = xr.renderingModeCount > 0 ? xr.renderingModeViewCounts[mode] : 2u;
                if (modeViewCount < 1) modeViewCount = 1;
                if (modeViewCount > runtimeViewCount) modeViewCount = runtimeViewCount;
                bool display3D = xr.renderingModeCount > 0 ? xr.renderingModeDisplay3D[mode] : true;
                bool monoMode = !display3D;
                uint32_t tileColumns = xr.renderingModeCount > 0 && xr.renderingModeTileColumns[mode] > 0
                    ? xr.renderingModeTileColumns[mode] : (monoMode ? 1u : 2u);
                int eyeCount = monoMode ? 1 : (int)modeViewCount;

                float scaleX = xr.renderingModeCount > 0 ? xr.renderingModeScaleX[mode] : 1.0f;
                float scaleY = xr.renderingModeCount > 0 ? xr.renderingModeScaleY[mode] : 1.0f;
                uint32_t renderW = (uint32_t)((double)xr.swapchain.width * (tileColumns > 0 ? scaleX : 1.0));
                uint32_t renderH = (uint32_t)((double)xr.swapchain.height * scaleY);
                if (tileColumns > 0) renderW = (uint32_t)((double)xr.swapchain.width / (double)tileColumns);
                if (renderW == 0) renderW = 1;
                if (renderH == 0) renderH = 1;

                uint32_t imageIndex;
                if (AcquireSwapchainImage(xr, imageIndex)) {
                    projectionViews.assign((size_t)eyeCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                    VkImage targetImage = swapchainImages[imageIndex].image;
                    VkFormat swapFormat = (VkFormat)xr.swapchain.format;

                    for (int eye = 0; eye < eyeCount; eye++) {
                        int srcView = eye < (int)runtimeViewCount ? eye : 0;
                        float viewMat[16], projMat[16];
                        float clipNear = 0.0f, clipFar = 0.0f;
                        if (useRig) {
                            float ez = RigLocalEyeZ(cameraPose, views[srcView].pose.position);
                            clipNear = (ez - virtualDisplayHeight > 1e-4f) ? (ez - virtualDisplayHeight) : 1e-4f;
                            clipFar = ez + 1000.0f * virtualDisplayHeight;
                            if (clipFar < clipNear + 1e-4f) clipFar = clipNear + 1e-4f;
                            mat4_view_from_xr_pose(viewMat, views[srcView].pose);
                            mat4_from_xr_fov(projMat, views[srcView].fov, clipNear, clipFar);
                        } else {
                            mat4_view_from_xr_pose(viewMat, views[srcView].pose);
                            mat4_from_xr_fov(projMat, views[srcView].fov, 0.01f, 100.0f);
                        }

                        uint32_t tileX = (uint32_t)(eye % (int)tileColumns);
                        uint32_t tileY = (uint32_t)(eye / (int)tileColumns);
                        uint32_t vpX = tileX * renderW;
                        uint32_t vpY = tileY * renderH;

                        projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                        projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                        projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, (int32_t)vpY};
                        projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                        projectionViews[eye].subImage.imageArrayIndex = 0;
                        projectionViews[eye].pose = views[srcView].pose;
                        projectionViews[eye].fov = views[srcView].fov;

                        if (g_gsRenderer.hasScene()) {
                            g_gsRenderer.renderEye(
                                targetImage, swapFormat,
                                xr.swapchain.width, xr.swapchain.height,
                                vpX, vpY, renderW, renderH,
                                viewMat, projMat,
                                /*transparentBg=*/false,
                                clipNear, clipFar, /*clipFadeFrac=*/0.15f);
                        }
                    }
                    ReleaseSwapchainImage(xr);
                }
            }
        }

        EndFrame(xr, fs.predictedDisplayTime,
            projectionViews.empty() ? nullptr : projectionViews.data(),
            (uint32_t)projectionViews.size());
    }

    LOG_INFO("Shutting down");
    if (xr.session) xrRequestExitSession(xr.session);
    vkDeviceWaitIdle(vkDevice);
    g_gsRenderer.cleanup();
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    return 0;
}

// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_DXR_win32_window_binding
 */

#include "xr_session.h"
#include "logging.h"
#include <openxr/XR_DXR_view_rig.h>
#include <cstring>

// XR_DXR_view_rig (W7 of #396): the runtime owns the off-axis Kooima math and
// returns render-ready XrView{pose, fov}; the app deletes its own. The flag
// lives demo-side (file-static + accessor) because XrSessionManager comes from
// displayxr::common, which doesn't carry view-rig state yet — upstream it there
// and re-pin when the package adopts the extension.
static bool s_hasViewRigExt = false;

bool XrViewRigExtAvailable() { return s_hasViewRigExt; }

// INV-1.3 / runtime#715: 3D panel top-left in OS virtual-desktop pixels
// (top-down, origin = primary monitor top-left), from
// XrDisplayDesktopPositionDXR (XR_DXR_display_info spec v16). (0,0) =
// primary/unknown — a safe default, and what an older runtime (which
// ignores the unknown chain entry) yields via the zero-init below. Also
// demo-side statics because XrSessionManager comes from displayxr::common.
static int32_t s_displayDesktopLeft = 0;
static int32_t s_displayDesktopTop = 0;

void GetDisplayDesktopPosition(int32_t& left, int32_t& top)
{
    left = s_displayDesktopLeft;
    top = s_displayDesktopTop;
}

#define XR_CHECK(call) \
    do { \
        XrResult result = (call); \
        if (XR_FAILED(result)) { \
            LogXrResult(#call, result); \
            return false; \
        } \
    } while (0)

#define XR_CHECK_LOG(call) \
    do { \
        XrResult result = (call); \
        LogXrResult(#call, result); \
        if (XR_FAILED(result)) { \
            return false; \
        } \
    } while (0)

bool InitializeOpenXR(XrSessionManager& xr) {
    LOG_INFO("Querying OpenXR instance extension properties...");

    uint32_t extensionCount = 0;
    XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    xr.hasWin32WindowBindingExt = false;

    for (const auto& ext : extensions) {
        LOG_DEBUG("  %s (v%u)", ext.extensionName, ext.extensionVersion);
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            xr.hasWin32WindowBindingExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            xr.hasDisplayInfoExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_WORKSPACE_FILE_DIALOG_EXTENSION_NAME) == 0) {
            xr.hasFileDialogExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME) == 0) {
            xr.hasAtlasCaptureExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) {
            s_hasViewRigExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_MCP_TOOLS_EXTENSION_NAME) == 0) {
            xr.hasMcpToolsExt = true;
        }
    }

    LOG_INFO("XR_KHR_vulkan_enable2: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_display_info: %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_workspace_file_dialog: %s", xr.hasFileDialogExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_atlas_capture: %s", xr.hasAtlasCaptureExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_view_rig: %s", s_hasViewRigExt ? "AVAILABLE" : "NOT FOUND");
    LOG_INFO("XR_DXR_mcp_tools: %s", xr.hasMcpToolsExt ? "AVAILABLE" : "NOT FOUND");

    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable2 extension not available");
        return false;
    }

    // vulkan_enable2: the RUNTIME creates the VkInstance/VkDevice on our
    // behalf (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR), letting it
    // append the instance/device extensions AND enable the device features it
    // needs — notably VK_KHR_present_id/present_wait for late-weave pacing
    // (INV-5.9), with zero app-side feature code.
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (xr.hasWin32WindowBindingExt) {
        enabledExtensions.push_back(XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME);
    }
    if (xr.hasDisplayInfoExt) {
        enabledExtensions.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
    }
    if (xr.hasFileDialogExt) {
        enabledExtensions.push_back(XR_DXR_WORKSPACE_FILE_DIALOG_EXTENSION_NAME);
    }
    if (xr.hasAtlasCaptureExt) {
        enabledExtensions.push_back(XR_DXR_ATLAS_CAPTURE_EXTENSION_NAME);
    }
    if (s_hasViewRigExt) {
        enabledExtensions.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    }
    if (xr.hasMcpToolsExt) {
        enabledExtensions.push_back(XR_DXR_MCP_TOOLS_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "SR3DGSOpenXRExtVK");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "None");
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));
    LOG_INFO("System ID: %llu", (unsigned long long)xr.systemId);

    // Get system name
    {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
            LOG_INFO("System name: %s", xr.systemName);
        }
    }

    // Query display info via XR_DXR_display_info
    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoDXR displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
        XrEyeTrackingModeCapabilitiesDXR eyeCaps = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_DXR};
        // INV-1.3: panel desktop position (display_info v16, runtime#715),
        // consumed by CreateAppWindow so the window opens on the 3D panel.
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        eyeCaps.next = &desktopPos;
        displayInfo.next = &eyeCaps;
        sysProps.next = &displayInfo;
        XrResult diResult = xrGetSystemProperties(xr.instance, xr.systemId, &sysProps);
        if (XR_SUCCEEDED(diResult)) {
            xr.recommendedViewScaleX = displayInfo.recommendedViewScaleX;
            xr.recommendedViewScaleY = displayInfo.recommendedViewScaleY;
            xr.displayWidthM = displayInfo.displaySizeMeters.width;
            xr.displayHeightM = displayInfo.displaySizeMeters.height;
            xr.nominalViewerX = displayInfo.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = displayInfo.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = displayInfo.nominalViewerPositionInDisplaySpace.z;
            xr.displayPixelWidth = displayInfo.displayPixelWidth;
            xr.displayPixelHeight = displayInfo.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)eyeCaps.supportedModes;
            xr.defaultEyeTrackingMode = (uint32_t)eyeCaps.defaultMode;
            s_displayDesktopLeft = desktopPos.left;
            s_displayDesktopTop = desktopPos.top;
            LOG_INFO("Display desktop position: (%d, %d)", s_displayDesktopLeft, s_displayDesktopTop);
            LOG_INFO("Display info: scale=%.3fx%.3f, size=%.3fx%.3fm, pixels=%ux%u, nominal=(%.0f,%.0f,%.0f)mm",
                xr.recommendedViewScaleX, xr.recommendedViewScaleY,
                xr.displayWidthM, xr.displayHeightM,
                xr.displayPixelWidth, xr.displayPixelHeight,
                xr.nominalViewerX * 1000.0f, xr.nominalViewerY * 1000.0f, xr.nominalViewerZ * 1000.0f);
            LOG_INFO("Eye tracking: supported=0x%x, default=%u",
                xr.supportedEyeTrackingModes, xr.defaultEyeTrackingMode);
        }

        // Load xrRequestDisplayModeDXR function pointer
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);

        // Load xrRequestEyeTrackingModeDXR function pointer
        if (xr.supportedEyeTrackingModes != 0) {
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeDXR",
                (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);
        }

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
            (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    // #228 Tier 1 spatial file picker — resolve the app-side entrypoint
    // when the extension is enabled. Resolution failure is non-fatal: we
    // just fall through to the Win32 GetOpenFileNameA path at call time.
    if (xr.hasFileDialogExt) {
        xrGetInstanceProcAddr(xr.instance, "xrRequestFilePickerDXR",
            (PFN_xrVoidFunction*)&xr.pfnRequestFilePickerEXT);
        LOG_INFO("xrRequestFilePickerDXR: %s",
            xr.pfnRequestFilePickerEXT ? "resolved" : "NULL");
    }

    // XR_DXR_atlas_capture (W6 of #396): resolve the runtime-owned capture entry.
    if (xr.hasAtlasCaptureExt) {
        xrGetInstanceProcAddr(xr.instance, "xrCaptureAtlasDXR",
            (PFN_xrVoidFunction*)&xr.pfnCaptureAtlasEXT);
        LOG_INFO("xrCaptureAtlasDXR: %s", xr.pfnCaptureAtlasEXT ? "resolved" : "NULL");
    }

    // XR_DXR_mcp_tools (#66): resolve the agent-tool entry points. Defensive —
    // any NULL leaves RegisterAgentTools() (and thus the shared PollEvents' MCP
    // dispatch) inert, so the viewer runs identically when the extension or the
    // MCP capability gate is absent.
    // (The struct's PFN fields keep their historical EXT-suffixed NAMES; the
    // TYPES + entry-point strings are the current DXR ones.)
    if (xr.hasMcpToolsExt) {
        xrGetInstanceProcAddr(xr.instance, "xrSetMCPAppInfoDXR",
            (PFN_xrVoidFunction*)&xr.pfnSetMCPAppInfoEXT);
        xrGetInstanceProcAddr(xr.instance, "xrRegisterMCPToolDXR",
            (PFN_xrVoidFunction*)&xr.pfnRegisterMCPToolEXT);
        xrGetInstanceProcAddr(xr.instance, "xrGetMCPToolCallArgsDXR",
            (PFN_xrVoidFunction*)&xr.pfnGetMCPToolCallArgsEXT);
        xrGetInstanceProcAddr(xr.instance, "xrSubmitMCPToolResultDXR",
            (PFN_xrVoidFunction*)&xr.pfnSubmitMCPToolResultEXT);
        LOG_INFO("XR_DXR_mcp_tools entry points: %s",
            (xr.pfnSetMCPAppInfoEXT && xr.pfnRegisterMCPToolEXT &&
             xr.pfnGetMCPToolCallArgsEXT && xr.pfnSubmitMCPToolResultEXT)
                ? "resolved" : "NULL");
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    return true;
}

bool GetVulkanGraphicsRequirements(XrSessionManager& xr) {
    LOG_INFO("Getting Vulkan graphics requirements...");

    // vulkan_enable2 alias — the un-suffixed xrGetVulkanGraphicsRequirementsKHR
    // entry point is gated on XR_KHR_vulkan_enable (v1) being enabled, so
    // xrGetInstanceProcAddr rejects it on an enable2-only instance.
    PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanGraphicsRequirements2KHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirements2KHR",
        (PFN_xrVoidFunction*)&xrGetVulkanGraphicsRequirements2KHR);
    if (XR_FAILED(result) || !xrGetVulkanGraphicsRequirements2KHR) {
        LOG_ERROR("Failed to get xrGetVulkanGraphicsRequirements2KHR function pointer");
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    result = xrGetVulkanGraphicsRequirements2KHR(xr.instance, xr.systemId, &graphicsReq);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetVulkanGraphicsRequirements2KHR", result);
        return false;
    }

    LOG_INFO("Vulkan graphics requirements:");
    LOG_INFO("  Min API version: %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.minApiVersionSupported));
    LOG_INFO("  Max API version: %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

    return true;
}

bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance) {
    LOG_INFO("Creating Vulkan instance via xrCreateVulkanInstanceKHR (vulkan_enable2)...");

    PFN_xrCreateVulkanInstanceKHR xrCreateVulkanInstanceKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrCreateVulkanInstanceKHR",
        (PFN_xrVoidFunction*)&xrCreateVulkanInstanceKHR);
    if (XR_FAILED(result) || !xrCreateVulkanInstanceKHR) {
        LOG_ERROR("Failed to get xrCreateVulkanInstanceKHR");
        return false;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SR3DGSOpenXRExtVK";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;  // 3DGS.cpp needs Vulkan 1.2+

    // The runtime appends whatever instance extensions it needs.
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    XrVulkanInstanceCreateInfoKHR xrCreateInfo = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrCreateInfo.systemId = xr.systemId;
    xrCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrCreateInfo.vulkanCreateInfo = &createInfo;

    VkResult vkResult = VK_SUCCESS;
    result = xrCreateVulkanInstanceKHR(xr.instance, &xrCreateInfo, &vkInstance, &vkResult);
    if (XR_FAILED(result) || vkResult != VK_SUCCESS) {
        LOG_ERROR("xrCreateVulkanInstanceKHR failed: xr=%d vk=%d", result, vkResult);
        return false;
    }

    LOG_INFO("Vulkan instance created (runtime-managed extensions)");
    return true;
}

bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    LOG_INFO("Getting Vulkan physical device via xrGetVulkanGraphicsDevice2KHR...");

    PFN_xrGetVulkanGraphicsDevice2KHR xrGetVulkanGraphicsDevice2KHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDevice2KHR",
        (PFN_xrVoidFunction*)&xrGetVulkanGraphicsDevice2KHR);
    if (XR_FAILED(result) || !xrGetVulkanGraphicsDevice2KHR) {
        LOG_ERROR("Failed to get xrGetVulkanGraphicsDevice2KHR");
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR getInfo = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    getInfo.systemId = xr.systemId;
    getInfo.vulkanInstance = vkInstance;

    result = xrGetVulkanGraphicsDevice2KHR(xr.instance, &getInfo, &physDevice);
    if (XR_FAILED(result)) {
        LogXrResult("xrGetVulkanGraphicsDevice2KHR", result);
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);
    LOG_INFO("  API version: %d.%d.%d",
        VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion),
        VK_VERSION_PATCH(props.apiVersion));

    return true;
}

bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            LOG_INFO("Graphics queue family: %u", i);
            return true;
        }
    }

    LOG_ERROR("No graphics queue family found");
    return false;
}

bool CreateVulkanDevice(XrSessionManager& xr, VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    VkDevice& device, VkQueue& graphicsQueue)
{
    LOG_INFO("Creating Vulkan device via xrCreateVulkanDeviceKHR (vulkan_enable2)...");

    PFN_xrCreateVulkanDeviceKHR xrCreateVulkanDeviceKHR = nullptr;
    XrResult result = xrGetInstanceProcAddr(xr.instance, "xrCreateVulkanDeviceKHR",
        (PFN_xrVoidFunction*)&xrCreateVulkanDeviceKHR);
    if (XR_FAILED(result) || !xrCreateVulkanDeviceKHR) {
        LOG_ERROR("Failed to get xrCreateVulkanDeviceKHR");
        return false;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // Feature required by the 3DGS compute shaders. pEnabledFeatures passes
    // through xrCreateVulkanDeviceKHR untouched. (shaderInt64 / Int64Atomics
    // no longer needed — the 3dgs sort uses 32-bit packed keys.)
    VkPhysicalDeviceFeatures features = {};
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    // The runtime appends its required device extensions AND enables the
    // features it needs — timelineSemaphore (the shell IPC path's
    // cross-process workspace_sync_fence import) and present_id/present_wait
    // for late-weave pacing (INV-5.9). No app-side extension/feature ceremony:
    // in particular do NOT chain VkPhysicalDeviceVulkan12Features here — the
    // runtime chains its own VkPhysicalDeviceTimelineSemaphoreFeatures, and
    // mixing the two structs in one chain is invalid (VUID 02830).
    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.pEnabledFeatures = &features;

    XrVulkanDeviceCreateInfoKHR xrCreateInfo = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrCreateInfo.systemId = xr.systemId;
    xrCreateInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrCreateInfo.vulkanPhysicalDevice = physDevice;
    xrCreateInfo.vulkanCreateInfo = &createInfo;

    VkResult vkResult = VK_SUCCESS;
    result = xrCreateVulkanDeviceKHR(xr.instance, &xrCreateInfo, &device, &vkResult);
    if (XR_FAILED(result) || vkResult != VK_SUCCESS) {
        LOG_ERROR("xrCreateVulkanDeviceKHR failed: xr=%d vk=%d", result, vkResult);
        return false;
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    LOG_INFO("Vulkan device and graphics queue created");
    return true;
}

bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd)
{
    LOG_INFO("Creating OpenXR session with Vulkan + XR_DXR_win32_window_binding...");

    xr.windowHandle = hwnd;

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = queueIndex;

    XrWin32WindowBindingCreateInfoDXR sessionTarget = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR};
    sessionTarget.windowHandle = hwnd;
    // Always-on transparent-window support. The runtime wires DComp + the
    // KMT-shared-texture bridge based on these fields at xrCreateSession;
    // they cannot be flipped at runtime. Ctrl+T at the app level only
    // changes the renderer's output alpha — the chroma-key strip pass is
    // a no-op when alpha == 1 throughout, so opaque mode looks identical
    // to a non-transparent session. Requires runtime ≥ v1.3.0.
    sessionTarget.transparentBackgroundEnabled = XR_TRUE;

    if (xr.hasWin32WindowBindingExt && hwnd) {
        vkBinding.next = &sessionTarget;
        LOG_INFO("Using XR_DXR_win32_window_binding with window handle (transparent-bg ENABLED)");
    }

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &vkBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created: 0x%p", (void*)xr.session);

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoDXR> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].hardwareDisplay3D == XR_TRUE);
                    xr.renderingModeIsRequestable[i] = modes[i].isRequestable ? true : false;
                    // v13 initial-mode-sync: trust runtime-reported active mode.
                    if (modes[i].isActive) {
                        xr.currentModeIndex = modes[i].modeIndex;
                    }
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

    // XR_DXR_mcp_tools (#66): declare app identity + register the agent tools
    // now that the session exists. Inert (logs + returns) when the extension or
    // MCP capability gate is absent.
    RegisterAgentTools(xr);

    return true;
}

// ============================================================================
// Agent tools (XR_DXR_mcp_tools, #66) — Windows port of the macOS integration
// ============================================================================
//
// The viewer's controls are registered as MCP tools on the per-process MCP
// server the runtime hosts; agents reach them via the displayxr-mcp adapter.
// Registration is declared here (per-app appId + static tool schemas); the
// per-call dispatch lives in main.cpp (HandleAgentToolCall), where the app's
// live viewer state — renderer, camera rig, fit pose — is reachable.
//
// NOTE ON THE EVENT PUMP: displayxr-common v2.1.0 (#18) added an app-supplied
// hook — XrSessionManager::mcpToolHandler — that the shared PollEvents invokes
// for every XrEventDataMCPToolCallDXR (PollEvents fetches the args and submits
// the result; the handler just maps name+args -> JSON). RegisterAgentTools()
// installs HandleAgentToolCall() there, so this demo no longer forks PollEvents
// (the old PollEventsGs copy is gone) and the main loop drives the shared
// PollEvents() directly.

// The appId MUST equal the manifest `id` (linter INV-10.1):
// windows/displayxr/gaussian_splatting_handle_vk_win.displayxr.json → "gaussiansplat".
static const char* kMcpAppId = "gaussiansplat";

void RegisterAgentTools(XrSessionManager& xr) {
    if (!xr.hasMcpToolsExt || !xr.pfnSetMCPAppInfoEXT || !xr.pfnRegisterMCPToolEXT ||
        !xr.pfnGetMCPToolCallArgsEXT || !xr.pfnSubmitMCPToolResultEXT) {
        return; // no agent surface — viewer runs identically
    }

    XrMCPAppInfoDXR appInfo = {XR_TYPE_MCP_APP_INFO_DXR};
    strncpy(appInfo.appId, kMcpAppId, sizeof(appInfo.appId) - 1);
    XrResult ar = xr.pfnSetMCPAppInfoEXT(xr.session, &appInfo);
    if (XR_FAILED(ar)) {
        // XR_ERROR_FEATURE_UNSUPPORTED = MCP capability gate off — expected.
        LOG_INFO("xrSetMCPAppInfoDXR('%s'): %d — no agent surface", kMcpAppId, (int)ar);
        return;
    }

    auto reg = [&](const char* name, const char* desc, const char* schema) {
        XrMCPToolInfoDXR tool = {XR_TYPE_MCP_TOOL_INFO_DXR};
        tool.name = name;
        tool.description = desc;
        tool.inputSchemaJson = schema;
        XrResult tr = xr.pfnRegisterMCPToolEXT(xr.session, &tool);
        if (XR_FAILED(tr)) LOG_WARN("xrRegisterMCPToolDXR('%s') failed: %d", name, (int)tr);
    };

    // Descriptions + schemas are copied verbatim from the macOS build so the
    // agent-facing contract is identical across platforms.
    reg("load_splat",
        "Load a Gaussian-splat scene from a file path (.ply or .spz), replacing the "
        "currently loaded scene and auto-framing the camera on the main object. "
        "Returns the loaded file and its splat count; an error result if the file "
        "cannot be read or parsed.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path to the .ply or .spz scene file (absolute recommended).\"}},"
        "\"required\":[\"path\"]}");

    reg("get_status",
        "Read the viewer's live state: loaded scene file and splat count, frames per "
        "second, camera pose (world position in meters, yaw/pitch in degrees, zoom), "
        "virtual display height in meters, active rendering mode, transparent-background "
        "and auto-orbit flags, and whether the XR session is running. Requires no "
        "arguments.",
        "{\"type\":\"object\"}");

    reg("set_camera",
        "Set camera pose components absolutely; give any subset of the arguments and "
        "the rest stay unchanged. position_x/y/z move the camera rig in world meters; "
        "yaw_deg/pitch_deg aim it (pitch clamps to about +/-86 deg); zoom scales the "
        "scene (1 = default, larger zooms in, minimum 0.1). Takes effect on the next "
        "frame and is visually verifiable via capture_frame. Returns the applied "
        "camera state.",
        "{\"type\":\"object\",\"properties\":{"
        "\"position_x\":{\"type\":\"number\"},"
        "\"position_y\":{\"type\":\"number\"},"
        "\"position_z\":{\"type\":\"number\"},"
        "\"yaw_deg\":{\"type\":\"number\"},"
        "\"pitch_deg\":{\"type\":\"number\"},"
        "\"zoom\":{\"type\":\"number\",\"minimum\":0.1}}}");

    reg("orbit",
        "Rotate the camera by a relative amount: azimuth_deg yaws around the vertical "
        "axis (positive matches the idle auto-orbit direction); elevation_deg tilts the "
        "view up (positive) or down, clamped to about +/-86 deg. At least one argument "
        "is required. Returns the new absolute yaw/pitch in degrees.",
        "{\"type\":\"object\",\"properties\":{"
        "\"azimuth_deg\":{\"type\":\"number\",\"description\":\"Relative yaw in degrees.\"},"
        "\"elevation_deg\":{\"type\":\"number\",\"description\":\"Relative pitch in degrees.\"}}}");

    reg("reset_camera",
        "Reset the camera to the auto-framed pose of the loaded scene (or the world "
        "origin if none is loaded): recenters position, levels pitch, restores default "
        "zoom and depth. Applied on the next frame. Returns the pose being restored.",
        "{\"type\":\"object\"}");

    reg("set_auto_orbit",
        "Enable or disable the idle auto-orbit turntable (when enabled, the view slowly "
        "yaws after 10 seconds without input). Disable it before scripted camera moves "
        "or captures that must hold still. Returns the new state.",
        "{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}},"
        "\"required\":[\"enabled\"]}");

    // Install the app's tool-call handler on the shared PollEvents hook
    // (displayxr-common v2.1.0 / #18). PollEvents fetches args + submits the
    // result; HandleAgentToolCall only maps (toolName, argsJson) -> resultJson.
    xr.mcpToolHandler = [&xr](const std::string& toolName,
                              const std::string& argsJson, bool& success) {
        return HandleAgentToolCall(xr, toolName, argsJson, success);
    };

    LOG_INFO("Agent tools registered (appId=%s)", kMcpAppId);
}

// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for Vulkan with XR_DXR_win32_window_binding
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr_session_common.h"

// Initialize OpenXR instance with Vulkan + win32_window_binding extensions
bool InitializeOpenXR(XrSessionManager& xr);

// XR_DXR_view_rig (W7 of #396) detected + enabled by InitializeOpenXR. Demo-side
// because displayxr::common's XrSessionManager doesn't carry view-rig state yet.
bool XrViewRigExtAvailable();

// INV-1.3 / runtime#715: 3D panel top-left in virtual-desktop pixels from
// XrDisplayDesktopPositionDXR (display_info v16), filled by InitializeOpenXR.
// (0,0) = primary/unknown (safe default, incl. on older runtimes). Demo-side
// for the same reason as XrViewRigExtAvailable().
void GetDisplayDesktopPosition(int32_t& left, int32_t& top);

// Get Vulkan graphics requirements and set up Vulkan instance/device per OpenXR spec
bool GetVulkanGraphicsRequirements(XrSessionManager& xr);

// Create Vulkan instance with required extensions from the runtime
bool CreateVulkanInstance(XrSessionManager& xr, VkInstance& vkInstance);

// Get the physical device selected by the runtime
bool GetVulkanPhysicalDevice(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);

// Get required device extensions from the runtime
bool GetVulkanDeviceExtensions(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage);

// Find a graphics queue family
bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);

// Create Vulkan logical device with required extensions
bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue);

// Create OpenXR session with Vulkan binding + win32_window_binding
bool CreateSession(XrSessionManager& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, HWND hwnd);

// XR_DXR_mcp_tools (#66): declare the app's identity + register its agent tools
// on the per-process MCP server the runtime hosts. Called by CreateSession once
// the session exists; inert when the extension / MCP capability gate is absent.
void RegisterAgentTools(XrSessionManager& xr);

// Demo-owned event pump — a faithful copy of displayxr-common's PollEvents that
// delegates the XR_DXR_mcp_tools tool-call event to HandleAgentToolCall (the
// shared PollEvents hardcodes the cube tool set and can't dispatch app tools).
// The main render loop calls this INSTEAD of the shared PollEvents.
bool PollEventsGs(XrSessionManager& xr);

// Dispatch one agent tool call and answer it. Defined in windows/main.cpp, where
// the app's live viewer state (renderer, camera rig, fit pose) is reachable.
// Runs on the render thread (from PollEventsGs), so it touches shared state under
// the same mutexes the render loop uses.
void HandleAgentToolCall(XrSessionManager& xr, const XrEventDataMCPToolCallDXR* call);

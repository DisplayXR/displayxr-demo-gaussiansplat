// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Click-through for the transparent splat overlay (X11 and Wayland).
 *
 * The Linux analogue of the Windows leg's `dxr::ClickThroughRegion`
 * (displayxr-common/common/vk_clickthrough_region.h, runtime#833/#837), which
 * punches the window through to the desktop with SetWindowRgn. Here the region
 * goes through displayxr::linux_window's set_input_region(): an XShape
 * **ShapeInput** region on X11, the surface's input region on Wayland. Clicks
 * land on the splats and pass through the transparent surround to whatever is
 * underneath. This file owns only the COVERAGE (the Vulkan readback and the
 * runs); the window system is the helper's.
 *
 * Carried over from displayxr-demo-modelviewer's linux/clickthrough.{h,cpp},
 * with its buffer helpers swapped for gs_vulkan_utils'.
 *
 * WHERE THE COVERAGE COMES FROM. Like the Windows leg, the region is derived
 * from the frame's OWN rendered views: a downscale blit of the swapchain
 * tiles' alpha, which GsRenderer::renderEye writes as 1 - T in transparent
 * mode. No second scene pass, so it describes exactly what the user sees, and
 * there is no scratch render target — only transfer-only coverage images.
 *
 * Everything downstream of the coverage IS the avatar's design, and it is
 * hardware-verified on X11:
 *
 *   - the raster SCALES with the window (kTexelPxDefault window px per texel,
 *     capped), so precision does not degrade as the window grows;
 *   - the coverage is the UNION of the FIRST and LAST view, from the SAME
 *     frame. The weave shows every view at once with horizontal disparity that
 *     grows with window size, so a single-view mask visibly clips the other
 *     view's content;
 *   - the alpha threshold is LOW (8) and the result is DILATED by one texel.
 *     The error is asymmetric: an over-large region leaves a few transparent
 *     pixels clickable, an under-large one makes content unreachable;
 *   - the readback is PIPELINED behind a fence and HOST_CACHED. A synchronous
 *     vkQueueWaitIdle on a write-combined buffer is what made this pass look
 *     too expensive to run at frame rate (~127 ms vs ~0.86 ms, measured on the
 *     Windows leg);
 *   - rects are one per horizontal run, with identical consecutive rows folded
 *     into a band, so a 4K window does not hand the X server thousands of
 *     rectangles per frame.
 *
 * CHROME IS UNIONED IN, NOT PUNCHED OUT. Anything drawn that is not the
 * model — the client-side title bar — must be part of the region or the
 * shaped window makes it unclickable. (That is the avatar's speech-bubble bug:
 * an un-unioned band is invisible to the pointer.) The window helper unions
 * its own header bar in, on both backends.
 *
 * REACHABILITY SAFETY NET. With the title bar unioned in the window is always
 * grabbable, but the shape is also applied when decorations are off, and a
 * silhouette that shrinks to almost nothing (model panned off screen, nothing
 * loaded) would leave a window that cannot be clicked or focused — you could
 * no longer press Ctrl+T to get it back. Below kMinCoveredPx of covered
 * window area the shape is therefore DROPPED rather than applied, and the
 * fall back is logged once per transition.
 *
 * No-op when there is no window (hosted-NULL fallback); the helper reports
 * once when the window system cannot express an input region.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

class DxrLinuxWindow;

//! Everything one click-through update needs.
struct ClickthroughParams {
	VkDevice dev = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;

	//! The swapchain atlas image the frame's views were just rendered into.
	//! Must be in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL on entry; it is
	//! handed back in the same layout, so this may be called any time between
	//! the last renderEye() and xrReleaseSwapchainImage().
	VkImage viewImage = VK_NULL_HANDLE;
	VkFormat viewFormat = VK_FORMAT_UNDEFINED;
	//! Per-view tile size in atlas px (the mode's renderW x renderH).
	uint32_t tileW = 0, tileH = 0;
	//! Atlas offset of the FIRST and LAST view's tile. Equal when mono.
	uint32_t firstTileX = 0, firstTileY = 0;
	uint32_t lastTileX = 0, lastTileY = 0;
	bool twoViews = false;

	//! The window (displayxr::linux_window). It applies the region to the
	//! surface that is hit-tested (the X11 top-level, offsetting past the
	//! header bar and unioning it in; the Wayland content surface).
	DxrLinuxWindow *window = nullptr;
	//! Live CONTENT size in px — the area the view tiles map onto
	//! (DxrLinuxWindow::current_size, not a cached create size).
	uint32_t winW = 0;
	uint32_t winH = 0;

	//! Ctrl+T state. An opaque frame covers every pixel of the window, so the
	//! whole window must stay interactive.
	bool transparentBg = false;
	//! Decorated windows (DXR_X11_WM_DECORATIONS=1) and fullscreen ones drop
	//! the shape entirely: a WM frame needs the whole window for move/resize.
	bool decorated = false;

	//! A header bar is shown (DxrLinuxWindow::header_bar_visible). The helper
	//! keeps it clickable; here it only means the window stays reachable even
	//! when the silhouette is tiny.
	bool chromeVisible = false;
};

//! Read back the frame's silhouette and set the window's XShape input region
//! from it. Call once per frame, after the eye render, before releasing the
//! swapchain image.
void ClickthroughUpdate(const ClickthroughParams &p);

//! Free the coverage images / readback buffer / command pool / fence. Device
//! must be idle.
void ClickthroughDestroy(VkDevice dev);

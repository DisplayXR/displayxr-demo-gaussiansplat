// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Click-through from the frame's own rendered alpha (X11 + Wayland).
 *
 * See clickthrough.h for the design and for why the coverage comes from a
 * downscale blit of the swapchain atlas rather than a second scene pass.
 */

#include "clickthrough.h"

#include "gs_vulkan_utils.h"

#include "dxr_linux_window.h" // DxrLinuxWindow::set_input_region / clear_input_region

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ── Tunables. Deliberately the SAME env names the Windows leg uses ──────────
// (displayxr-common/common/vk_clickthrough_region.h), not the avatar's
// DXR_AVATAR_SIL_*: one knob per behaviour across both platforms.
//   DXR_CLICKTHROUGH_TEXEL_PX  window px per coverage texel (default 4)
//   DXR_CLICKTHROUGH_DILATE    dilation radius in texels    (default 1)
//   DXR_CLICKTHROUGH_ALPHA     region alpha threshold 1..255 (default 8)
//   DXR_VK_NO_HOST_CACHED=1    force the write-combined readback (repro lever)
static constexpr uint32_t kCovMaxW = 1024, kCovMaxH = 576;
static constexpr uint32_t kCovMinW = 64, kCovMinH = 64;
static constexpr uint32_t kTexelPxDefault = 4;

//! Below this much covered CLIENT area the shape is dropped instead of
//! applied — see the reachability note in clickthrough.h. 64x64 is small
//! enough that any actually-visible splat clears it and large enough that a
//! few stray dilated texels do not.
static constexpr uint32_t kMinCoveredPx = 64 * 64;

static uint32_t
CtEnvUInt(const char *name, uint32_t def, uint32_t lo, uint32_t hi)
{
	const char *e = getenv(name);
	if (e == nullptr || e[0] == '\0') {
		return def;
	}
	const long v = strtol(e, nullptr, 10);
	if (v < (long)lo || v > (long)hi) {
		return def;
	}
	return (uint32_t)v;
}

// ── Device resources ────────────────────────────────────────────────────────
// The coverage images are allocated ONCE at the ceiling and used as a
// sub-rect, so a window resize never recreates them under an in-flight copy
// (the Windows leg's rule). They are pure transfer targets: no VkImageView,
// therefore no VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT needed — which is the whole
// class of bug avatar#99 is about, avoided by construction rather than fixed.
struct CovImage {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

static CovImage g_cov[2];
static VkFormat g_covFormat = VK_FORMAT_UNDEFINED;
static GsBuffer g_readback = {};
static void *g_mapped = nullptr;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkFence g_fence = VK_NULL_HANDLE;
static VkCommandBuffer g_prevCmd = VK_NULL_HANDLE;
static bool g_initFailed = false;

// Pipelined readback: each call submits its blits + copies behind a fence and
// does NOT wait; the NEXT call consumes the bytes. Staleness is exactly one
// frame, against the ~16 ms synchronous stall a vkQueueWaitIdle would cost.
static bool g_pending = false;
static uint32_t g_pendingW = 0, g_pendingH = 0;
static bool g_pendingTwo = false;

// Published coverage: one byte per texel, 1 = content present.
static std::vector<uint8_t> g_covBits;
static uint32_t g_covW = 0, g_covH = 0;
static bool g_covReady = false;

//! Latches so a state change is applied (and logged) exactly once.
static bool g_shapeCleared = false;
static bool g_shaped = false;
static int g_lastReachableState = -1; // -1 unknown, 0 dropped, 1 applied

static bool
CreateCovImage(VkDevice dev, VkPhysicalDevice phys, VkFormat fmt, CovImage &out)
{
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {kCovMaxW, kCovMaxH, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(dev, &ici, nullptr, &out.image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, out.image, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = gsFindMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (vkAllocateMemory(dev, &ai, nullptr, &out.memory) != VK_SUCCESS) {
		vkDestroyImage(dev, out.image, nullptr);
		out.image = VK_NULL_HANDLE;
		return false;
	}
	vkBindImageMemory(dev, out.image, out.memory, 0);
	return true;
}

//! One-time device setup. Returns false (permanently, via g_initFailed) when
//! the driver cannot blit this format or an allocation fails — click-through
//! then degrades to "not shaped", never to a crash.
static bool
EnsureDevice(const ClickthroughParams &p)
{
	if (g_initFailed) {
		return false;
	}
	if (g_cov[0].image != VK_NULL_HANDLE && g_covFormat == p.viewFormat) {
		return true;
	}
	if (g_cov[0].image != VK_NULL_HANDLE) {
		// Format changed (a swapchain re-create with a different format).
		// Rare enough to just tear down and rebuild.
		ClickthroughDestroy(p.dev);
		g_initFailed = false;
	}

	// The blit is the whole mechanism; a format that cannot be blitted must
	// disable the pass rather than generate validation errors every frame.
	VkFormatProperties fp = {};
	vkGetPhysicalDeviceFormatProperties(p.phys, p.viewFormat, &fp);
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if ((fp.optimalTilingFeatures & need) != need) {
		fprintf(stderr,
		        "[WARN]  clickthrough: swapchain format %d cannot be blitted "
		        "(optimalTilingFeatures=0x%x) — click-through disabled.\n",
		        (int)p.viewFormat, (unsigned)fp.optimalTilingFeatures);
		g_initFailed = true;
		return false;
	}

	if (g_pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = p.queueFamily;
		if (vkCreateCommandPool(p.dev, &pci, nullptr, &g_pool) != VK_SUCCESS) {
			g_initFailed = true;
			return false;
		}
	}
	if (g_fence == VK_NULL_HANDLE) {
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(p.dev, &fci, nullptr, &g_fence) != VK_SUCCESS) {
			g_initFailed = true;
			return false;
		}
	}
	if (!CreateCovImage(p.dev, p.phys, p.viewFormat, g_cov[0]) ||
	    !CreateCovImage(p.dev, p.phys, p.viewFormat, g_cov[1])) {
		g_initFailed = true;
		return false;
	}
	g_covFormat = p.viewFormat;

	// HOST_CACHED: this buffer is READ every frame, and HOST_VISIBLE|COHERENT
	// alone is typically WRITE-COMBINED on a discrete GPU — fast to write,
	// pathologically slow to read (measured on the Windows leg: ~127 ms/frame
	// write-combined against ~0.86 ms cached). DXR_VK_NO_HOST_CACHED=1 forces
	// the legacy allocation so the pathology stays reproducible.
	const VkDeviceSize bytes = (VkDeviceSize)kCovMaxW * kCovMaxH * 4 * 2;
	static const bool s_noCached = getenv("DXR_VK_NO_HOST_CACHED") != nullptr;
	g_readback = gsCreateBuffer(
	    p.dev, p.phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    s_noCached ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	               : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
	if (g_readback.buffer == VK_NULL_HANDLE) {
		fprintf(stderr, "[WARN]  clickthrough: no HOST_CACHED memory type — using write-combined; "
		                "the per-frame coverage read will be slower on this driver.\n");
		g_readback = gsCreateBuffer(p.dev, p.phys, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
	if (g_readback.buffer == VK_NULL_HANDLE) {
		g_initFailed = true;
		return false;
	}
	if (vkMapMemory(p.dev, g_readback.memory, 0, bytes, 0, &g_mapped) != VK_SUCCESS) {
		g_mapped = nullptr;
		g_initFailed = true;
		return false;
	}
	return true;
}

//! Fold the previous call's readback into the published coverage: union both
//! planes' alpha, threshold, dilate.
static void
ConsumePendingReadback(VkDevice dev, uint32_t dilate, uint8_t alphaMin)
{
	if (!g_pending || g_fence == VK_NULL_HANDLE) {
		return;
	}
	vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
	g_pending = false;

	const uint8_t *px = (const uint8_t *)g_mapped;
	const uint32_t cw = g_pendingW, ch = g_pendingH;
	const size_t n = (size_t)cw * ch;
	if (px == nullptr || n == 0) {
		return;
	}
	const size_t plane1 = (size_t)kCovMaxW * kCovMaxH * 4;

	// Row-wise memcpy BEFORE touching the bytes: memcpy uses wide streaming
	// loads, which survive write-combined memory; a per-texel loop does not.
	// Insurance for drivers with no HOST_CACHED type. Alpha is byte 3 in both
	// R8G8B8A8 and B8G8R8A8, and sRGB never encodes the alpha channel, so the
	// swapchain's exact format does not matter here.
	std::vector<uint8_t> unionAlpha(n, 0);
	std::vector<uint8_t> rowBuf((size_t)cw * 4);
	for (uint32_t y = 0; y < ch; ++y) {
		uint8_t *out = unionAlpha.data() + (size_t)y * cw;
		memcpy(rowBuf.data(), px + (size_t)y * cw * 4, (size_t)cw * 4);
		for (uint32_t x = 0; x < cw; ++x) {
			out[x] = rowBuf[(size_t)x * 4 + 3];
		}
		if (g_pendingTwo) {
			memcpy(rowBuf.data(), px + plane1 + (size_t)y * cw * 4, (size_t)cw * 4);
			for (uint32_t x = 0; x < cw; ++x) {
				const uint8_t a = rowBuf[(size_t)x * 4 + 3];
				if (a > out[x]) {
					out[x] = a;
				}
			}
		}
	}

	std::vector<uint8_t> bits(n, 0);
	for (size_t i = 0; i < n; ++i) {
		bits[i] = (unionAlpha[i] > alphaMin) ? 1 : 0;
	}
	// Separable dilation, both passes biased outward: the region is what makes
	// the window reachable at all, so an under-large mask loses content the
	// user can see, while an over-large one only leaves a few transparent
	// pixels clickable.
	if (dilate > 0) {
		const uint32_t R = dilate;
		std::vector<uint8_t> tmp(n, 0);
		for (uint32_t y = 0; y < ch; ++y) {
			const uint8_t *src = bits.data() + (size_t)y * cw;
			uint8_t *dst = tmp.data() + (size_t)y * cw;
			for (uint32_t x = 0; x < cw; ++x) {
				if (!src[x]) {
					continue;
				}
				const uint32_t x0 = (x > R) ? x - R : 0;
				const uint32_t x1 = (x + R + 1 < cw) ? x + R + 1 : cw;
				memset(dst + x0, 1, x1 - x0);
			}
		}
		std::vector<uint8_t> out(n, 0);
		for (uint32_t y = 0; y < ch; ++y) {
			const uint8_t *src = tmp.data() + (size_t)y * cw;
			const uint32_t y0 = (y > R) ? y - R : 0;
			const uint32_t y1 = (y + R + 1 < ch) ? y + R + 1 : ch;
			for (uint32_t yy = y0; yy < y1; ++yy) {
				uint8_t *d = out.data() + (size_t)yy * cw;
				for (uint32_t x = 0; x < cw; ++x) {
					d[x] |= src[x];
				}
			}
		}
		bits.swap(out);
	}

	g_covBits.swap(bits);
	g_covW = cw;
	g_covH = ch;
	g_covReady = true;
}

//! Drop the shape: the whole window becomes interactive again.
static void
ClearShape(DxrLinuxWindow *window)
{
	window->clear_input_region();
	g_shaped = false;
}

//! Turn the published coverage into the window's input region (the helper
//! adds the header bar).
static void
ApplyRegion(const ClickthroughParams &p)
{
	if (!g_covReady || g_covW == 0 || g_covH == 0 || p.winW == 0 || p.winH == 0) {
		return;
	}
	std::vector<DxrWindowRect> rects;

	// Scale against the LIVE window, not the size the coverage was captured
	// at. The coverage is a normalised silhouette, so mapping it onto the
	// current rect is the right thing during a resize — and it keeps the runs
	// in the same frame as the chrome rects below, which are always current.
	const int64_t cw = (int64_t)g_covW, ch = (int64_t)g_covH;
	const int64_t winW = (int64_t)p.winW, winH = (int64_t)p.winH;

	// One client-space rect per horizontal run, with identical consecutive
	// rows folded into a single band — the raster scales with the window (540
	// rows on a 4K panel) and would otherwise hand the server thousands of
	// rectangles per frame for a shape that is mostly vertical edges.
	std::vector<int> runs, prevRuns;
	size_t bandFirst = (size_t)-1;
	for (int64_t y = 0; y < ch; ++y) {
		runs.clear();
		int64_t x = 0;
		while (x < cw) {
			if (!g_covBits[(size_t)(y * cw + x)]) {
				++x;
				continue;
			}
			const int64_t xs = x;
			while (x < cw && g_covBits[(size_t)(y * cw + x)]) {
				++x;
			}
			runs.push_back((int)xs);
			runs.push_back((int)x);
		}
		const int64_t bottom = (y + 1) * winH / ch;
		if (runs.empty()) {
			bandFirst = (size_t)-1;
			prevRuns.clear();
			continue;
		}
		if (bandFirst != (size_t)-1 && runs == prevRuns) {
			for (size_t i = bandFirst; i < rects.size(); ++i) {
				rects[i].height = (uint32_t)(bottom - (int64_t)rects[i].y);
			}
			continue;
		}
		bandFirst = rects.size();
		const int64_t top = y * winH / ch;
		for (size_t i = 0; i + 1 < runs.size(); i += 2) {
			const int64_t left = (int64_t)runs[i] * winW / cw;
			const int64_t right = (int64_t)runs[i + 1] * winW / cw;
			DxrWindowRect r;
			r.x = (int32_t)left;
			r.y = (int32_t)top;
			r.width = (uint32_t)(right > left ? right - left : 1);
			r.height = (uint32_t)(bottom > top ? bottom - top : 1);
			rects.push_back(r);
		}
		prevRuns = runs;
	}

	uint32_t covered = 0;
	for (const auto &r : rects) {
		covered += (uint32_t)r.width * r.height;
	}

	// The reachability net (clickthrough.h). Measured on the SILHOUETTE only:
	// the chrome below is by definition reachable, but it is also optional
	// (DXR_X11_WM_DECORATIONS, a future chrome-less mode), so the silhouette
	// has to stand on its own.
	const bool reachable = covered >= kMinCoveredPx || p.chromeVisible;
	if (!reachable) {
		if (g_lastReachableState != 0) {
			g_lastReachableState = 0;
			fprintf(stderr, "[WARN]  clickthrough: silhouette covers only ~%u px of %lldx%lld — "
			                "dropping the input shape so the window stays clickable "
			                "(Ctrl+T still works). Bring content back on screen to restore "
			                "click-through.\n",
			        covered, (long long)winW, (long long)winH);
		}
		if (g_shaped) {
			ClearShape(p.window);
		}
		return;
	}
	if (g_lastReachableState != 1) {
		g_lastReachableState = 1;
	}

	// Diagnostic: the rect count disambiguates "region empty" (no alpha →
	// render/matrix issue) from "region present but clicks still fall through"
	// (coordinate/application issue). Throttled to ~once every 300 updates
	// (~5 s at 60 Hz).
	static int s_diag = 0;
	if ((s_diag++ % 300) == 0) {
		fprintf(stderr,
		        "[INFO]  clickthrough: %zu input-rects (%s), ~%u px covered, "
		        "win %lldx%lld, coverage %ux%u\n",
		        rects.size(), p.chromeVisible ? "+ header bar" : "no header bar", covered, (long long)winW,
		        (long long)winH, g_covW, g_covH);
	}

	// Set the window's INPUT region to the splats (+ the header bar, which the
	// helper adds): clicks land on it, and the transparent rest passes through
	// to the desktop. Input only — the window keeps rendering everywhere, only
	// hit-testing is restricted, which is what lets the compositor blend our
	// alpha. False = the window system cannot express it (reported once by
	// the helper); nothing to undo then.
	g_shaped = p.window->set_input_region(rects.empty() ? nullptr : rects.data(), rects.size());
}

void
ClickthroughUpdate(const ClickthroughParams &p)
{
	if (p.window == nullptr || p.winW == 0 || p.winH == 0 || p.viewImage == VK_NULL_HANDLE || p.tileW == 0 ||
	    p.tileH == 0) {
		return;
	}

	// Decorated (DXR_X11_WM_DECORATIONS=1) or opaque (Ctrl+T off): the whole
	// window is interactive. A WM frame needs it for move/resize, and an
	// opaque frame covers every pixel, so shaping it to the silhouette would
	// make the background it just drew unclickable.
	if (p.decorated || !p.transparentBg) {
		if (!g_shapeCleared) {
			ClearShape(p.window);
			g_shapeCleared = true;
		}
		return;
	}
	// Back to transparent: the region is re-applied below. The latch must be
	// cleared here, not only set above, or a second Ctrl+T would find
	// g_shapeCleared already true and never drop the shape again.
	g_shapeCleared = false;

	static const uint32_t s_texelPx = CtEnvUInt("DXR_CLICKTHROUGH_TEXEL_PX", kTexelPxDefault, 1, 64);
	static const uint32_t s_dilate = CtEnvUInt("DXR_CLICKTHROUGH_DILATE", 1, 0, 16);
	static const uint8_t s_alphaMin = (uint8_t)CtEnvUInt("DXR_CLICKTHROUGH_ALPHA", 8, 1, 255);

	uint32_t w = (p.winW + s_texelPx - 1) / s_texelPx;
	uint32_t h = (p.winH + s_texelPx - 1) / s_texelPx;
	if (w < kCovMinW) {
		w = kCovMinW;
	}
	if (w > kCovMaxW) {
		w = kCovMaxW;
	}
	if (h < kCovMinH) {
		h = kCovMinH;
	}
	if (h > kCovMaxH) {
		h = kCovMaxH;
	}

	// 1. Consume the previous call's readback first — it is the input to the
	//    region published in step 3.
	ConsumePendingReadback(p.dev, s_dilate, s_alphaMin);

	if (!EnsureDevice(p)) {
		return;
	}

	// 2. Publish the region built from the readback we just consumed, then
	//    kick this frame's pair. Applying before the kick keeps the X
	//    round-trip off the critical path between submit and the next wait.
	ApplyRegion(p);

	// 3. Downscale-blit this frame's outermost view tiles into the coverage
	//    images and copy them out behind the fence — no wait. The atlas is
	//    left in COLOR_ATTACHMENT_OPTIMAL, which is what the runtime expects
	//    at xrReleaseSwapchainImage.
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = g_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(p.dev, &ai, &cmd) != VK_SUCCESS) {
		return;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	const VkImageSubresourceRange colorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.subresourceRange = colorRange;

	// Atlas → TRANSFER_SRC.
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.image = p.viewImage;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);

	// Coverage images → TRANSFER_DST. UNDEFINED is correct: every texel that
	// will be read is overwritten by the blit below.
	const uint32_t planes = p.twoViews ? 2u : 1u;
	VkImageMemoryBarrier toDst[2];
	for (uint32_t i = 0; i < planes; ++i) {
		toDst[i] = b;
		toDst[i].srcAccessMask = 0;
		toDst[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst[i].image = g_cov[i].image;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                     nullptr, planes, toDst);

	// BOTH halves of the union come from the SAME frame, in one command
	// buffer. Alternating views instead makes the region trail a moving
	// subject and clip its leading edge: the weave shows every view at once,
	// with horizontal disparity that grows with the window.
	auto blitTile = [&](uint32_t srcX, uint32_t srcY, VkImage dst) {
		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {(int32_t)srcX, (int32_t)srcY, 0};
		blit.srcOffsets[1] = {(int32_t)(srcX + p.tileW), (int32_t)(srcY + p.tileH), 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.dstOffsets[0] = {0, 0, 0};
		blit.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
		vkCmdBlitImage(cmd, p.viewImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	};
	blitTile(p.firstTileX, p.firstTileY, g_cov[0].image);
	if (p.twoViews) {
		blitTile(p.lastTileX, p.lastTileY, g_cov[1].image);
	}

	// Atlas back to COLOR_ATTACHMENT_OPTIMAL for the compositor.
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b.image = p.viewImage;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &b);

	// Coverage → TRANSFER_SRC, then out to the host buffer.
	VkImageMemoryBarrier toSrc[2];
	for (uint32_t i = 0; i < planes; ++i) {
		toSrc[i] = b;
		toSrc[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toSrc[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toSrc[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toSrc[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toSrc[i].image = g_cov[i].image;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                     nullptr, planes, toSrc);

	for (uint32_t i = 0; i < planes; ++i) {
		VkBufferImageCopy region = {};
		region.bufferOffset = (VkDeviceSize)i * kCovMaxW * kCovMaxH * 4;
		region.bufferRowLength = w;
		region.bufferImageHeight = h;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		vkCmdCopyImageToBuffer(cmd, g_cov[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer,
		                       1, &region);
	}
	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkResetFences(p.dev, 1, &g_fence);
	if (vkQueueSubmit(p.queue, 1, &si, g_fence) == VK_SUCCESS) {
		g_pending = true;
		g_pendingTwo = p.twoViews;
		g_pendingW = w;
		g_pendingH = h;
	}
	// `cmd` is consumed on the NEXT call's fence wait; freeing it here while
	// in flight would be invalid. Free the previous one instead.
	if (g_prevCmd != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(p.dev, g_pool, 1, &g_prevCmd);
	}
	g_prevCmd = cmd;
}

void
ClickthroughDestroy(VkDevice dev)
{
	if (g_fence != VK_NULL_HANDLE) {
		if (g_pending) {
			vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
			g_pending = false;
		}
		vkDestroyFence(dev, g_fence, nullptr);
		g_fence = VK_NULL_HANDLE;
	}
	if (g_prevCmd != VK_NULL_HANDLE && g_pool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(dev, g_pool, 1, &g_prevCmd);
		g_prevCmd = VK_NULL_HANDLE;
	}
	if (g_mapped != nullptr) {
		vkUnmapMemory(dev, g_readback.memory);
		g_mapped = nullptr;
	}
	if (g_readback.buffer != VK_NULL_HANDLE) {
		gsDestroyBuffer(dev, g_readback);
	}
	for (auto &c : g_cov) {
		if (c.image != VK_NULL_HANDLE) {
			vkDestroyImage(dev, c.image, nullptr);
			c.image = VK_NULL_HANDLE;
		}
		if (c.memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev, c.memory, nullptr);
			c.memory = VK_NULL_HANDLE;
		}
	}
	g_covFormat = VK_FORMAT_UNDEFINED;
	if (g_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(dev, g_pool, nullptr);
		g_pool = VK_NULL_HANDLE;
	}
	g_covReady = false;
	g_covBits.clear();
	g_covW = g_covH = 0;
}

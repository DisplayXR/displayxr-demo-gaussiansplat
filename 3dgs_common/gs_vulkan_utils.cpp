// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Vulkan buffer/image creation helpers implementation
 */

#include "gs_vulkan_utils.h"
#include <cstdio>
#include <cstring>

uint32_t gsFindMemoryType(VkPhysicalDevice physDevice,
                          uint32_t typeFilter,
                          VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fprintf(stderr, "gs_vulkan_utils: failed to find suitable memory type\n");
    return 0;
}

GsBuffer gsCreateBuffer(VkDevice device,
                        VkPhysicalDevice physDevice,
                        VkDeviceSize size,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags memProps)
{
    GsBuffer buf;
    buf.size = size;

    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &ci, nullptr, &buf.buffer) != VK_SUCCESS) {
        fprintf(stderr, "gs_vulkan_utils: failed to create buffer (%llu bytes)\n",
                (unsigned long long)size);
        return buf;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buf.buffer, &memReq);

    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = memReq.size;
    ai.memoryTypeIndex = gsFindMemoryType(physDevice, memReq.memoryTypeBits, memProps);

    if (vkAllocateMemory(device, &ai, nullptr, &buf.memory) != VK_SUCCESS) {
        fprintf(stderr, "gs_vulkan_utils: failed to allocate buffer memory (%llu bytes)\n",
                (unsigned long long)memReq.size);
        vkDestroyBuffer(device, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
        return buf;
    }

    vkBindBufferMemory(device, buf.buffer, buf.memory, 0);
    return buf;
}

void gsDestroyBuffer(VkDevice device, GsBuffer& buf)
{
    if (buf.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
    }
    if (buf.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
    }
    buf.size = 0;
}

GsImage gsCreateImage2D(VkDevice device,
                        VkPhysicalDevice physDevice,
                        uint32_t width,
                        uint32_t height,
                        VkFormat format,
                        VkImageUsageFlags usage)
{
    GsImage img;
    img.width = width;
    img.height = height;
    img.format = format;

    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &ici, nullptr, &img.image) != VK_SUCCESS) {
        fprintf(stderr, "gs_vulkan_utils: failed to create image (%ux%u)\n", width, height);
        return img;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, img.image, &memReq);

    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = memReq.size;
    ai.memoryTypeIndex = gsFindMemoryType(physDevice, memReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &ai, nullptr, &img.memory) != VK_SUCCESS) {
        fprintf(stderr, "gs_vulkan_utils: failed to allocate image memory\n");
        vkDestroyImage(device, img.image, nullptr);
        img.image = VK_NULL_HANDLE;
        return img;
    }
    vkBindImageMemory(device, img.image, img.memory, 0);

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &vci, nullptr, &img.view) != VK_SUCCESS) {
        fprintf(stderr, "gs_vulkan_utils: failed to create image view\n");
    }

    return img;
}

void gsDestroyImage(VkDevice device, GsImage& img)
{
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
        img.view = VK_NULL_HANDLE;
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, img.image, nullptr);
        img.image = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, img.memory, nullptr);
        img.memory = VK_NULL_HANDLE;
    }
    img.width = 0;
    img.height = 0;
    img.format = VK_FORMAT_UNDEFINED;
}

bool gsIsSrgbFormat(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

static VkFormat gsUnormSiblingOf(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return format;
    }
}

bool gsEnsureSwapchainScratch(VkDevice device,
                              VkPhysicalDevice physDevice,
                              GsImage& scratch,
                              uint32_t width,
                              uint32_t height,
                              VkFormat swapchainFormat)
{
    if (!gsIsSrgbFormat(swapchainFormat)) {
        return true;
    }
    const VkFormat want = gsUnormSiblingOf(swapchainFormat);
    if (scratch.image != VK_NULL_HANDLE && scratch.format == want &&
        scratch.width == width && scratch.height == height) {
        return true;
    }
    gsDestroyImage(device, scratch);
    // SAMPLED only so gsCreateImage2D's view is valid; the scratch is
    // transfer-only in practice.
    scratch = gsCreateImage2D(device, physDevice, width, height, want,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT);
    return scratch.image != VK_NULL_HANDLE;
}

void gsCmdBlitToSwapchain(VkCommandBuffer cmd,
                          VkImage src,
                          uint32_t srcW,
                          uint32_t srcH,
                          VkImage dst,
                          VkFormat dstFormat,
                          int32_t dstX,
                          int32_t dstY,
                          uint32_t dstW,
                          uint32_t dstH,
                          VkFilter filter,
                          const GsImage& scratch)
{
    const bool viaScratch = gsIsSrgbFormat(dstFormat) && scratch.image != VK_NULL_HANDLE &&
                            dstW <= scratch.width && dstH <= scratch.height;

    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {(int32_t)srcW, (int32_t)srcH, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

    if (!viaScratch) {
        if (gsIsSrgbFormat(dstFormat)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                fprintf(stderr, "gs_vulkan_utils: no UNORM scratch for the sRGB swapchain — "
                                "blitting directly, colours will be encoded twice (washed out)\n");
            }
        }
        blit.dstOffsets[0] = {dstX, dstY, 0};
        blit.dstOffsets[1] = {dstX + (int32_t)dstW, dstY + (int32_t)dstH, 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        return;
    }

    // scratch -> TRANSFER_DST (its last use, if any, was the copy read below).
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = scratch.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    // UNORM -> UNORM: scale + channel swizzle, no colour conversion.
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {(int32_t)dstW, (int32_t)dstH, 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   scratch.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    // Size-compatible formats (UNORM sibling -> *_SRGB): a raw byte copy.
    VkImageCopy c = {};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.srcOffset = {0, 0, 0};
    c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstOffset = {dstX, dstY, 0};
    c.extent = {dstW, dstH, 1};
    vkCmdCopyImage(cmd, scratch.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

bool gsUploadBuffer(VkDevice device,
                    VkPhysicalDevice physDevice,
                    VkQueue queue,
                    VkCommandPool cmdPool,
                    GsBuffer& dst,
                    const void* data,
                    VkDeviceSize size)
{
    // Create staging buffer
    GsBuffer staging = gsCreateBuffer(device, physDevice, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging.buffer == VK_NULL_HANDLE) return false;

    // Map and copy
    void* mapped = nullptr;
    vkMapMemory(device, staging.memory, 0, size, 0, &mapped);
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(device, staging.memory);

    // Record copy command
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &ai, &cmd);

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkBufferCopy region = {};
    region.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    gsDestroyBuffer(device, staging);
    return true;
}

// ── Radix-sort subgroup-size precondition ──────────────────────────────────
// Contract and rationale: gs_vulkan_utils.h.

//! WORKGROUP_SIZE / RADIX_SORT_BINS in shaders/sort.comp. Both are 256 there
//! and the arithmetic below assumes it, so keep them equal to the shader.
static const uint32_t kGsSortWorkgroupSize = 256;

bool gsSortSubgroupSizeSupported(uint32_t subgroupSize, const char** whyNot) {
    const char* why = nullptr;
    bool ok = true;
    if (subgroupSize == 0 || (subgroupSize & (subgroupSize - 1)) != 0) {
        why = "not a power of two"; ok = false;
    } else if (subgroupSize > kGsSortWorkgroupSize) {
        why = "larger than the 256-thread workgroup (sums[] would be zero-length)"; ok = false;
    } else if (subgroupSize < 16) {
        // 256 / s subgroups must fit in one subgroup for the cross-subgroup
        // scan's subgroupBroadcast(.., gl_SubgroupID) to be in range.
        why = "below 16, so the 256/s subgroup sums cannot be scanned inside one subgroup";
        ok = false;
    }
    if (whyNot != nullptr) *whyNot = why;
    return ok;
}

uint32_t gsPickSortSubgroupSize(uint32_t minSubgroupSize, uint32_t maxSubgroupSize) {
    if (minSubgroupSize == 0 || maxSubgroupSize < minSubgroupSize) return 0;
    // Prefer the LARGEST legal size in range: fewer subgroups means a shorter
    // cross-subgroup scan, and it is what every device that reports a sane
    // default already uses.
    for (uint32_t s = kGsSortWorkgroupSize; s >= 1; s >>= 1) {
        if (s < minSubgroupSize || s > maxSubgroupSize) continue;
        if (gsSortSubgroupSizeSupported(s, nullptr)) return s;
    }
    return 0;
}

bool gsSpirvAllowsVaryingSubgroupSize(const void* spirv, size_t byteSize) {
    // SPIR-V header: word 0 magic (0x07230203), word 1 version as
    // 0x00MMmm00. Anything malformed is reported as "varying allowed" — the
    // conservative answer, because it makes the caller complain rather than
    // assume a guarantee it cannot verify.
    if (spirv == nullptr || byteSize < 8) return true;
    const uint32_t* w = static_cast<const uint32_t*>(spirv);
    if (w[0] != 0x07230203u) return true;
    const uint32_t major = (w[1] >> 16) & 0xffu;
    const uint32_t minor = (w[1] >> 8) & 0xffu;
    return (major > 1u) || (major == 1u && minor >= 6u);
}

// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Vulkan buffer/image creation helpers for 3DGS compute pipeline
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>

// Find a memory type index matching the required properties.
uint32_t gsFindMemoryType(VkPhysicalDevice physDevice,
                          uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);

struct GsBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

// Create a GPU buffer with the specified usage and memory properties.
GsBuffer gsCreateBuffer(VkDevice device,
                        VkPhysicalDevice physDevice,
                        VkDeviceSize size,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags memProps);

// Destroy a buffer and free its memory.
void gsDestroyBuffer(VkDevice device, GsBuffer& buf);

struct GsImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Create a 2D image with a view.
GsImage gsCreateImage2D(VkDevice device,
                        VkPhysicalDevice physDevice,
                        uint32_t width,
                        uint32_t height,
                        VkFormat format,
                        VkImageUsageFlags usage);

// Destroy an image, its view, and free memory.
void gsDestroyImage(VkDevice device, GsImage& img);

// Upload CPU data to a device-local buffer via a staging buffer.
bool gsUploadBuffer(VkDevice device,
                    VkPhysicalDevice physDevice,
                    VkQueue queue,
                    VkCommandPool cmdPool,
                    GsBuffer& dst,
                    const void* data,
                    VkDeviceSize size);

// ── Radix-sort subgroup-size precondition (shaders/sort.comp) ───────────────
//
// sort.comp's cross-workgroup scan is written against a specialization
// constant SUBGROUP_SIZE that the host feeds from
// VkPhysicalDeviceSubgroupProperties::subgroupSize. Two preconditions come out
// of the shader source and NEITHER was checked anywhere:
//
//   1. `shared uint[RADIX_SORT_BINS / SUBGROUP_SIZE] sums;` is indexed by
//      gl_SubgroupID, so the workgroup's subgroup count must equal
//      256 / SUBGROUP_SIZE exactly — i.e. SUBGROUP_SIZE must DIVIDE 256, and
//      must equal the real subgroup size at dispatch.
//
//   2. `subgroupBroadcast(subgroupExclusiveAdd(sums[lsID]), sID)` reduces the
//      per-subgroup sums inside ONE subgroup and broadcasts lane sID, so the
//      number of subgroups must not exceed the subgroup size:
//      256 / SUBGROUP_SIZE <= SUBGROUP_SIZE, i.e. SUBGROUP_SIZE >= 16.
//      Below 16 the broadcast lane index runs past gl_SubgroupSize (undefined
//      behaviour) and only the first SUBGROUP_SIZE of the 256/SUBGROUP_SIZE
//      bin sums are ever accumulated.
//
// So the legal set is {16, 32, 64, 128, 256}. Precondition 2 is not
// theoretical: llvmpipe — Mesa's software Vulkan device, which is what a
// headless, VM or no-GPU box falls back to — reports subgroupSize 8 with
// minSubgroupSize == maxSubgroupSize == 8, so it cannot satisfy it at all.
// The failure mode is a silently mis-sorted splat list: wrong back-to-front
// order, garbled alpha compositing, and not one validation-layer message,
// because every individual Vulkan call is legal.
//
// Returns true when `subgroupSize` is usable. On false, *whyNot (optional) is
// set to a short static string naming which precondition failed.
bool gsSortSubgroupSizeSupported(uint32_t subgroupSize, const char** whyNot);

//! True when this SPIR-V module is version 1.6 or newer.
//!
//! This is precondition 1's real trigger, and it is not where anyone looks.
//! The usual belief is that a pipeline stage created WITHOUT
//! VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT and without
//! VkPipelineShaderStageRequiredSubgroupSizeCreateInfo is guaranteed a subgroup
//! size equal to VkPhysicalDeviceSubgroupProperties::subgroupSize. That is only
//! two thirds of the rule. The Vulkan spec (Shaders, "varying subgroup size")
//! lists THREE ways to opt in, and the first one needs no API call at all:
//!
//!     Varying subgroups are allowed if ANY of:
//!       * the shader was created from SPIR-V version 1.6 or higher
//!       * VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT
//!       * VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT
//!
//! glslangValidator emits SPIR-V 1.6 by default for --target-env vulkan1.3, so
//! bumping that one word in 3dgs_common/CMakeLists.txt silently forfeits the
//! guarantee, and the shared-memory array sized 256/SUBGROUP_SIZE starts being
//! indexed past its end. Measured on this project's target hardware (Intel
//! PTL, Mesa 26.0.8): at SPIR-V 1.5 the driver honours 32; at SPIR-V 1.6 with
//! identical pipeline state it reports 32 and dispatches 16.
//!
//! So the SPIR-V version is a correctness input to this shader, and the
//! renderers check it rather than trusting the build to stay pinned.
bool gsSpirvAllowsVaryingSubgroupSize(const void* spirv, size_t byteSize);

//! Longest legal subgroup size in [minSubgroupSize, maxSubgroupSize] that
//! satisfies gsSortSubgroupSizeSupported, or 0 when the device's whole range
//! is unusable. Only meaningful with VK_EXT_subgroup_size_control /
//! Vulkan 1.3; without it the size is not the app's to choose.
uint32_t gsPickSortSubgroupSize(uint32_t minSubgroupSize, uint32_t maxSubgroupSize);

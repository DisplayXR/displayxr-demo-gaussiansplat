// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Splat-renderer selection for the shared demo harness.
 *
 * Two renderers ship in 3dgs_common with the same public API surface:
 *   - GsRenderer (gs_renderer.h)        — compute compositor: expand to per-tile
 *     fragments, global radix sort, per-pixel composite into a storage image.
 *     Built for an IMMEDIATE-MODE desktop GPU.
 *   - GsAdrenoRenderer (gs_adreno_renderer.h) — graphics pipeline: instanced
 *     alpha-blended quads, ROP composite in on-chip tile memory. The win on a
 *     TILE-BASED DEFERRED (TBDR) GPU (Adreno; Apple Silicon; Snapdragon-X /
 *     Windows-on-ARM), where the compute path's global sort + storage-image
 *     scatter are exactly wrong.
 *
 * Pick the graphics renderer where the GPU is TBDR, the compute one otherwise.
 *
 * Three ways to get there, in order of precedence:
 *   - GS_RENDERER_GRAPHICS / GS_RENDERER_COMPUTE predefined (CMake
 *     -DGS_RENDERER=GRAPHICS|COMPUTE): a hard compile-time pin.
 *   - GS_RENDERER_RUNTIME (the Windows default): GsAutoRenderer chooses at
 *     startup from the VkPhysicalDevice's vendor, with DXR_GS_RENDERER=
 *     compute|graphics as the override (gs_auto_renderer.h). The CPU
 *     architecture is the wrong key on Windows: the shipped exe is x64 and runs
 *     emulated on Windows-on-ARM, on an Adreno.
 *   - otherwise a compile-time default from the platform (macOS: graphics;
 *     Linux: compute, though linux/CMakeLists.txt pins GRAPHICS).
 *
 * NOTE: the Android leg includes gs_adreno_renderer.h directly (its NativeAct
 * harness predates this header); this selector drives the desktop legs.
 */
#pragma once

#if !defined(GS_RENDERER_GRAPHICS) && !defined(GS_RENDERER_COMPUTE) && !defined(GS_RENDERER_RUNTIME)
#  if defined(_WIN32)
//   x64 and ARM64 alike: the GPU decides, not the instruction set.
#    define GS_RENDERER_RUNTIME 1
#  elif defined(__ANDROID__) || defined(__APPLE__)
//   Apple Silicon is TBDR like Adreno, so the graphics path is the macOS
//   default (over MoltenVK). x86_64 macs are EOL for this product; if one ever
//   matters, predefine GS_RENDERER_COMPUTE for it.
#    define GS_RENDERER_GRAPHICS 1
#  else
#    define GS_RENDERER_COMPUTE 1
#  endif
#endif

#if defined(GS_RENDERER_GRAPHICS)
#  include "gs_adreno_renderer.h"
using GsActiveRenderer = GsAdrenoRenderer;
#elif defined(GS_RENDERER_COMPUTE)
#  include "gs_renderer.h"
using GsActiveRenderer = GsRenderer;
#else
#  include "gs_auto_renderer.h"
using GsActiveRenderer = GsAutoRenderer;
#endif

// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compile-time splat-renderer selection for the shared demo harness.
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
 * Override by predefining GS_RENDERER_GRAPHICS or GS_RENDERER_COMPUTE before
 * including this header (e.g. to benchmark the off-default path).
 *
 * NOTE: the Android leg includes gs_adreno_renderer.h directly (its NativeAct
 * harness predates this header); this selector drives the desktop legs.
 */
#pragma once

#if !defined(GS_RENDERER_GRAPHICS) && !defined(GS_RENDERER_COMPUTE)
#  if defined(__ANDROID__) || (defined(_WIN32) && defined(_M_ARM64))
#    define GS_RENDERER_GRAPHICS 1
#  else
#    define GS_RENDERER_COMPUTE 1
#  endif
#endif

#if defined(GS_RENDERER_GRAPHICS)
#  include "gs_adreno_renderer.h"
using GsActiveRenderer = GsAdrenoRenderer;
#else
#  include "gs_renderer.h"
using GsActiveRenderer = GsRenderer;
#endif

// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SOG scene loading utilities for 3DGS (PlayCanvas "Spatially Ordered
 *         Gaussians" — a PKZip of meta.json + WebP planes)
 *
 * SOG is what the DisplayXR Gallery ships: a `.sog` bundle is a ZIP whose
 * `meta.json` describes a handful of WebP textures, one texel per gaussian, in
 * raster order. Everything is quantised:
 *
 *   means_l.webp / means_u.webp   16-bit split (low byte / high byte) of a
 *                                 lerp between meta.means.mins/maxs, in
 *                                 LOG-TRANSFORMED space (see InvLogTransform)
 *   quats.webp                    smallest-three packed quaternion; RGB are the
 *                                 three stored components, A is 252+maxComp
 *   scales.webp                   RGB index the shared 256-entry
 *                                 meta.scales.codebook (log scales)
 *   sh0.webp                      RGB index meta.sh0.codebook (SH DC), A is the
 *                                 already-sigmoided opacity
 *   shN (optional)                a palette: a labels texture (16-bit index in
 *                                 R|G<<8) into a centroids texture whose bytes
 *                                 index meta.shN.codebook
 *
 * Spec: https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/
 * Reference decoder: @playcanvas/splat-transform `src/lib/readers/read-sog.ts`
 * (`readSogSourceV2`). This file is a straight port of that decode.
 *
 * ── Coordinate system ───────────────────────────────────────────────────────
 *
 * Like SPZ v4, SOG carries NO coordinate-system field, and the one writer that
 * matters bakes the cloud into PLY space before encoding
 * (splat-transform sets `transform: Transform.PLY`). So the on-disk bytes are
 * RDF (+x right, +y down, +z forward) and the loader converts them to the
 * app's canonical RUB (+x right, +y up, +z back) — the same half-turn about x
 * the SPZ loader applies to a v4 container, performed by the SAME code
 * (`spz::GaussianCloud::convertCoordinates`), so the same scene delivered as
 * `.spz` and as `.sog` renders to the same picture. `DXR_SOG_COORD_SYSTEM=rub`
 * overrides the assumption for a file that breaks the rule.
 *
 * ── The `camera` block ──────────────────────────────────────────────────────
 *
 * A photo-lifted scene may carry an OPTIONAL top-level `camera` object — a
 * sibling of `count`, with `version` still 2 — naming the camera the gaussians
 * were predicted through:
 *
 *   "camera": {
 *     "convention": "opencv",
 *     "rest": { "position": [0,0,0], "rotation": [0,0,0,1] },
 *     "intrinsics": { "fx": 1194.666, "fy": 1194.666, "cx": 1024.0,
 *                     "cy": 576.0, "width": 2048, "height": 1152 },
 *     "stereo": { "baseline_m": 0.063 }
 *   }
 *
 * `rotation` is xyzw, camera->world. Intrinsics are in pixels of ONE EYE's
 * image (`width` x `height`); `cx` may be off-centre (the horizontal shift a
 * stereo lift bakes in when it deconverges the pair). `stereo` is optional —
 * the second capture camera sits at +x * baseline_m.
 *
 * Every reader ignores the block when it is absent, which is the entire
 * installed base: nothing written before this proposal carries it. Its
 * PRESENCE is what selects the viewer's camera rig; see GsSceneCamera in
 * gs_scene_loader.h.
 */

#pragma once

#include <string>
#include <vector>

#include "gs_scene_loader.h"  // GsVertex, GsSceneCamera

//! What the loader learned about the file, whether or not it loaded.
//!
//! Mirrors SpzFileInfo's contract: a rejected file can still be NAMED in the
//! log. `version` is meta.json's own `version` field (2 for every SOG in the
//! wild); 0 means the bundle was not readable as a SOG at all.
struct SogFileInfo {
    int         version   = 0;  //!< meta.json "version", 0 = not a readable SOG
    int         numPoints = 0;  //!< meta.json "count" (0 when unknown)
    int         shBands   = 0;  //!< meta.shN.bands, 0 when the bundle is DC-only
    std::string generator;      //!< meta.asset.generator, empty when absent
    std::string error;          //!< one-line failure reason, empty on success
    //! Coordinate system the loader ASSUMED before righting to RUB. The
    //! container records none — see the file comment.
    std::string sourceSystem;
};

//! Parse a PlayCanvas SOG bundle and return GPU-ready vertices.
//!
//! Converts the cloud into the app's canonical RUB, applies exp(scale) /
//! sigmoid(opacity) / quaternion normalisation exactly as ParseSpzFile does,
//! and — when `camera` is non-null — fills it from the optional `camera` block
//! (left untouched, i.e. `present == false`, when the bundle carries none).
//!
//! Returns true on success. On failure `vertices` is empty and, when `info` is
//! non-null, `info->error` carries a one-line reason. NEVER throws: a corrupt
//! or hostile bundle is a return value, not a crash.
bool ParseSogFile(const std::string& path,
                  std::vector<GsVertex>& vertices,
                  SogFileInfo* info = nullptr,
                  GsSceneCamera* camera = nullptr);

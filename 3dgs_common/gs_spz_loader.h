// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SPZ scene loading utilities for 3DGS (Niantic compressed format)
 */

#pragma once

#include <string>
#include <vector>
#include "gs_scene_loader.h"  // for GsVertex

//! What the loader learned about the file, whether or not it loaded.
//!
//! `version` is read straight out of the SPZ container header (both the legacy
//! gzip-wrapped 16-byte header and the v4 NGSP one put it at byte offset 4), so
//! a rejected file can still be NAMED in the log — "unsupported SPZ v9" beats
//! "load failed". 0 means the file is not an SPZ container at all (wrong magic,
//! truncated below a header, or not decompressible).
struct SpzFileInfo {
    int         version   = 0;  //!< SPZ header version, 0 = unknown / not an SPZ container
    int         numPoints = 0;  //!< gaussians the header claims (0 when unknown)
    int         shDegree  = 0;  //!< spherical-harmonics degree the header claims
    std::string error;          //!< one-line failure reason, empty on success
};

// Parse a Niantic SPZ file and return GPU-ready vertices.
// Converts from SPZ's RUB coordinate system to PLY's RDF convention.
// Applies: sigmoid(opacity), exp(scale), normalize(rotation), SH mapping.
// Returns true on success. On failure, vertices is empty and, when `info` is
// non-null, info->error carries a one-line reason naming the SPZ version.
// NEVER throws and never divides by a header-supplied count: a corrupt or
// unsupported file is a return value, not a crash.
bool ParseSpzFile(const std::string& path,
                  std::vector<GsVertex>& vertices,
                  SpzFileInfo* info = nullptr);

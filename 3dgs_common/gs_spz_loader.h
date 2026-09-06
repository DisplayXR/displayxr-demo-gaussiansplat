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
    //! Coordinate system the loader ASSUMED the on-disk bytes were in before
    //! converting them to the app's canonical RUB. The SPZ container records no
    //! such field, so this is the loader's policy choice, not a header read —
    //! see the coordinate-systems section of gs_spz_loader.cpp's file comment.
    std::string sourceSystem;
};

// Parse a Niantic SPZ file and return GPU-ready vertices.
// Converts the cloud into the app's canonical RUB (+X right, +Y up, +Z back).
// The container carries no coordinate-system field, so the source system is
// inferred from the container generation — legacy v1-v3 are RUB (Niantic's
// writer), NGSP v4+ are RDF (@playcanvas/splat-transform bakes PLY space) —
// and `DXR_SPZ_COORD_SYSTEM=rub|rdf` overrides that. See gs_spz_loader.cpp.
// Applies: sigmoid(opacity), exp(scale), normalize(rotation), SH mapping.
// Returns true on success. On failure, vertices is empty and, when `info` is
// non-null, info->error carries a one-line reason naming the SPZ version.
// NEVER throws and never divides by a header-supplied count: a corrupt or
// unsupported file is a return value, not a crash.
bool ParseSpzFile(const std::string& path,
                  std::vector<GsVertex>& vertices,
                  SpzFileInfo* info = nullptr);

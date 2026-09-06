// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SPZ scene loading — Niantic compressed format → GsVertex conversion
 *
 * Uses the Niantic spz library to decompress .spz files into GaussianCloud,
 * then converts to our GPU vertex format (GsVertex, 240 bytes per splat).
 *
 * Container shapes this has to cope with (all four are ".spz"):
 *
 *   1. gzip( legacy 16-byte header + one raw stream )   — SPZ v1…v3, e.g. the
 *      bundled butterfly.spz (v2). This is what `spz::loadSpzPacked` calls the
 *      "legacy single-stream GZip format" and handles directly.
 *   2. raw NGSP 32-byte header + N ZSTD streams          — SPZ v4 as upstream
 *      writes it. `spz::loadSpzPacked` dispatches on the leading NGSP magic.
 *   3. gzip( NGSP 32-byte header + N ZSTD streams )      — SPZ v4 as
 *      @playcanvas/splat-transform 3.3.3 writes it (the storefront's
 *      undocked gen/*.spz). Upstream does NOT re-dispatch after gunzip: it
 *      sees the gzip magic, inflates, then parses the result with the LEGACY
 *      16-byte header and fails on the size check. So we peel the gzip
 *      ourselves and hand the inner NGSP buffer to spz.
 *   4. anything else                                     — rejected, loudly.
 *
 * Note the asymmetry in 1 vs 3: for a legacy file we must pass the ORIGINAL
 * gzip bytes (the inner legacy header also starts with NGSP magic, so handing
 * spz the inflated buffer would send it down the NGSP path and it would reject
 * the version as < 4). Hence the "gunzip, look, then choose which buffer to
 * pass" shape below.
 *
 * ── Coordinate systems ──────────────────────────────────────────────────────
 *
 * THE SPZ CONTAINER DOES NOT RECORD ITS COORDINATE SYSTEM. Neither header
 * carries the field: in the 32-byte `NgspFileHeader` the byte at offset 15 —
 * the one a legacy 16-byte read shows as "reserved" — is `numStreams` (5 for a
 * degree-0 file: positions, alphas, colors, scales, rotations). Upstream's only
 * coordinate metadata is an optional *extension* block gated behind
 * `SPZ_BUILD_EXTENSIONS`, which is compiled out here (and whose header isn't
 * even in the tree at our pinned rev); the storefront's files carry
 * `flags = 0`, i.e. no extensions. So the source system is a POLICY CHOICE the
 * loader has to make, not something it can read.
 *
 * Upstream picks one unconditionally — `unpackGaussians()` ends in
 * `convertCoordinates(CoordinateSystem::RUB, o.to)`, i.e. "the bytes are RUB".
 * That is right for files the Niantic writer produced (v1-v3 in practice) and
 * WRONG for every v4 file in the wild, because the only v4 writer anyone uses,
 * `@playcanvas/splat-transform`, bakes the cloud into PLY space
 * (`Transform.PLY`) and then calls `saveSpz` with `from = UNSPECIFIED` — an
 * identity converter — so the on-disk bytes are RDF, not RUB. Its own source
 * says so: "splat-transform stores SPZ with no coordinate conversion: data is
 * baked to PLY/RDF space before saving … the format carries no coordinate
 * metadata". Asking spz for `to = RUB` then applies RUB->RUB (nothing) and the
 * scene renders upside down (RDF vs RUB differ by 180 deg about X).
 *
 * Hence `SpzSourceSystem()` below: legacy containers (v1-v3) are RUB, NGSP
 * containers (v4+) are RDF. That is the generation split between the two
 * writers, and it reproduces what the web viewers show — the storefront's
 * Spark page loads the SAME cloud as `.sog` (splat-transform bakes `.sog` to
 * `Transform.PLY` too) and rights it with `mesh.quaternion.set(1,0,0,0)`, a
 * 180 deg rotation about X, which is exactly RDF -> RUB.
 *
 * `DXR_SPZ_COORD_SYSTEM=rub|rdf` overrides the choice for a file that breaks
 * the rule (a raw-NGSP v4 straight out of Niantic's own writer would be RUB).
 */

#include "gs_spz_loader.h"

#include <load-spz.h>
#include <splat-types.h>

#include <zlib.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <exception>
#include <vector>

namespace {

//! 'NGSP' — leads BOTH the legacy 16-byte header and the v4 NGSP one.
constexpr uint32_t kNgspMagic = 0x5053474eu;
//! Smallest container that can carry a readable version field.
constexpr size_t kMinHeaderBytes = 16;
//! Refuse absurd inputs before allocating; 2 GB is far past any real scene.
//! uint64_t, not size_t: a 32-bit ABI (armeabi-v7a) would wrap the constant.
constexpr uint64_t kMaxFileBytes = 2ull * 1024 * 1024 * 1024;
//! Same ceiling for the inflated payload — a zip bomb must not OOM the viewer.
constexpr uint64_t kMaxInflatedBytes = 2ull * 1024 * 1024 * 1024;

float spz_sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

//! SH floats per point beyond the DC term (which lives in cloud.colors).
//! Mirrors the table in spz's GaussianCloud docs; degree 4 (72) exists upstream
//! but our GsVertex only has room for degrees 1–3, so the copy below clamps.
int ShFloatsPerPoint(int shDegree) {
    switch (shDegree) {
        case 1:  return 9;   //  3 coeffs * 3 channels
        case 2:  return 24;  //  8 coeffs * 3 channels
        case 3:  return 45;  // 15 coeffs * 3 channels
        case 4:  return 72;  // 24 coeffs * 3 channels
        default: return 0;
    }
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    const long len = ftell(f);
    if (len < 0 || (uint64_t)len > kMaxFileBytes) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    out.resize((size_t)len);
    const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (got != out.size()) { out.clear(); return false; }
    return true;
}

//! Inflate a gzip member. windowBits 15|16 = "gzip wrapper only" (the same
//! setting spz uses internally). Returns false on any inflate error, including
//! a truncated stream — a partial payload is never handed on as if it were
//! whole.
bool GunzipBuffer(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (size == 0) return false;

    z_stream zs = {};
    zs.next_in = const_cast<Bytef*>(data);
    zs.avail_in = (uInt)size;
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;

    std::vector<uint8_t> chunk(64 * 1024);
    bool ok = false;
    for (;;) {
        zs.next_out = chunk.data();
        zs.avail_out = (uInt)chunk.size();
        const int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        const size_t produced = chunk.size() - zs.avail_out;
        if ((uint64_t)out.size() + produced > kMaxInflatedBytes) break;
        out.insert(out.end(), chunk.data(), chunk.data() + produced);
        if (rc == Z_STREAM_END) { ok = true; break; }
        if (produced == 0 && zs.avail_in == 0) break;  // truncated input
    }
    inflateEnd(&zs);
    if (!ok) out.clear();
    return ok;
}

//! Read magic + version + numPoints + shDegree out of an SPZ header. Works for
//! both header shapes: magic@0, version@4, numPoints@8, shDegree@12.
bool PeekSpzHeader(const uint8_t* data, size_t size,
                   uint32_t* version, uint32_t* numPoints, uint32_t* shDegree) {
    if (size < kMinHeaderBytes) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != kNgspMagic) return false;
    std::memcpy(version, data + 4, sizeof(uint32_t));
    std::memcpy(numPoints, data + 8, sizeof(uint32_t));
    *shDegree = data[12];
    return true;
}

//! Printable name for the systems this loader can pick between.
const char* CoordSystemName(spz::CoordinateSystem cs) {
    switch (cs) {
        case spz::CoordinateSystem::RUB: return "RUB (+X right, +Y up, +Z back)";
        case spz::CoordinateSystem::RDF: return "RDF (+X right, +Y down, +Z front)";
        default:                         return "UNSPECIFIED";
    }
}

//! Which coordinate system the on-disk bytes are in. See the coordinate-systems
//! section of the file header comment — the container never says, so this is the
//! loader's policy: legacy containers come from Niantic's writer (RUB), NGSP
//! containers come from @playcanvas/splat-transform (RDF/PLY space).
//!
//! `DXR_SPZ_COORD_SYSTEM=rub|rdf` overrides it for a file that breaks the rule.
spz::CoordinateSystem SpzSourceSystem(uint32_t version) {
    if (const char* env = std::getenv("DXR_SPZ_COORD_SYSTEM")) {
        if (env[0] == 'r' || env[0] == 'R') {
            if (env[1] == 'u' || env[1] == 'U') return spz::CoordinateSystem::RUB;
            if (env[1] == 'd' || env[1] == 'D') return spz::CoordinateSystem::RDF;
        }
        fprintf(stderr, "ParseSpzFile: ignoring DXR_SPZ_COORD_SYSTEM='%s' (expected rub or rdf)\n",
                env);
    }
    return version >= 4 ? spz::CoordinateSystem::RDF : spz::CoordinateSystem::RUB;
}

}  // namespace

bool ParseSpzFile(const std::string& path, std::vector<GsVertex>& vertices, SpzFileInfo* info)
{
    vertices.clear();
    SpzFileInfo local;
    SpzFileInfo& fi = info ? *info : local;
    fi = SpzFileInfo{};

    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, file) || file.size() < kMinHeaderBytes) {
        fi.error = "cannot read file (missing, empty or truncated)";
        fprintf(stderr, "ParseSpzFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    // Pick the buffer to hand to spz (see the file header comment for the four
    // container shapes). Default: the file exactly as it sits on disk.
    const uint8_t* payload = file.data();
    size_t payloadSize = file.size();
    std::vector<uint8_t> inflated;

    uint32_t version = 0, headerPoints = 0, headerShDegree = 0;
    const bool isGzip = file[0] == 0x1f && file[1] == 0x8b;
    if (isGzip) {
        if (!GunzipBuffer(file.data(), file.size(), inflated)) {
            fi.error = "gzip container is corrupt or truncated";
            fprintf(stderr, "ParseSpzFile: %s — %s\n", path.c_str(), fi.error.c_str());
            return false;
        }
        if (PeekSpzHeader(inflated.data(), inflated.size(), &version, &headerPoints,
                          &headerShDegree) &&
            version >= 4) {
            // Shape 3: gzip-wrapped NGSP. Feed spz the INNER buffer so its
            // magic dispatch takes the NGSP/ZSTD path.
            payload = inflated.data();
            payloadSize = inflated.size();
        }
        // Shape 1 (legacy v1–v3): leave payload pointing at the gzip bytes.
    } else {
        PeekSpzHeader(file.data(), file.size(), &version, &headerPoints, &headerShDegree);
    }
    fi.version = (int)version;
    fi.numPoints = (int)headerPoints;
    fi.shDegree = (int)headerShDegree;

    // Ask spz for its own storage convention (RUB) and do NOT let it convert:
    // `unpackGaussians` would convert FROM an assumed RUB, and for a v4 file
    // that assumption is the bug. The real source system is decided below and
    // applied explicitly, so the conversion reads the way it means.
    spz::GaussianCloud cloud;
    try {
        spz::UnpackOptions options;
        options.to = spz::CoordinateSystem::RUB;  // RUB -> RUB, a no-op
        cloud = spz::loadSpz(payload, payloadSize, options);
    } catch (const std::exception& e) {
        fi.error = std::string("spz loader threw: ") + e.what();
        fprintf(stderr, "ParseSpzFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    } catch (...) {
        fi.error = "spz loader threw an unknown exception";
        fprintf(stderr, "ParseSpzFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    if (cloud.numPoints <= 0) {
        // Every rejection upstream can make — unsupported version, bad point
        // count, ZSTD/gzip failure, size mismatch — arrives here as an empty
        // cloud. Name the version so the log says WHICH file shape was refused.
        if (fi.version == 0) {
            fi.error = "not an SPZ container (bad magic or unreadable header)";
        } else if (fi.version > 4) {
            fi.error = "unsupported SPZ v" + std::to_string(fi.version) +
                       " (this build reads v1-v4)";
        } else {
            fi.error = "SPZ v" + std::to_string(fi.version) +
                       " file rejected by the loader (corrupt or truncated)";
        }
        fprintf(stderr, "ParseSpzFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    const uint32_t numPoints = (uint32_t)cloud.numPoints;
    fi.numPoints = (int)numPoints;
    fi.shDegree = cloud.shDegree;

    // The vertex loop below indexes six parallel arrays by numPoints. spz sizes
    // them itself and checkSizes() enforces it, but this is the arithmetic that
    // a malformed file used to reach — assert it here rather than trust it.
    const size_t n = (size_t)numPoints;
    if (cloud.positions.size() < n * 3 || cloud.scales.size() < n * 3 ||
        cloud.rotations.size() < n * 4 || cloud.alphas.size() < n ||
        cloud.colors.size() < n * 3) {
        fi.error = "SPZ v" + std::to_string(fi.version) +
                   " attribute arrays are short for " + std::to_string(numPoints) +
                   " gaussians";
        fprintf(stderr, "ParseSpzFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    // The container never declares its coordinate system (see the file header
    // comment), so name the one we assumed — a scene that comes out upside down
    // is then one grep away from its cause, and DXR_SPZ_COORD_SYSTEM is the fix.
    // Convert here, once per load, on the whole cloud: spz's own converter also
    // handles the rotations, scales and SH bands, which a hand-rolled position
    // flip in the vertex loop below would silently get wrong.
    const spz::CoordinateSystem sourceSystem = SpzSourceSystem(version);
    fi.sourceSystem = CoordSystemName(sourceSystem);
    if (sourceSystem != spz::CoordinateSystem::RUB) {
        cloud.convertCoordinates(sourceSystem, spz::CoordinateSystem::RUB);
    }

    printf("ParseSpzFile: loading %u gaussians from %s (spzVersion=%d, shDegree=%d, "
           "source coords=%s -> RUB)\n",
           numPoints, path.c_str(), fi.version, cloud.shDegree, fi.sourceSystem.c_str());
    // stdout is block-buffered when redirected, and a viewer is usually ended by
    // a kill, not a clean exit — an unflushed line is a line nobody ever sees.
    fflush(stdout);

    // SH floats per point (beyond DC, which is stored in cloud.colors). Degree 4
    // is accepted but only its first 45 floats (bands 1–3) fit GsVertex; the
    // layout is coefficient-outer/channel-inner in increasing band order, so the
    // prefix is exactly bands 1–3 and the truncation is well defined.
    int shPerPoint = ShFloatsPerPoint(cloud.shDegree);
    if (shPerPoint > 0 && cloud.sh.size() < n * (size_t)shPerPoint) {
        fprintf(stderr, "ParseSpzFile: %s — SH array short for degree %d, dropping SH\n",
                path.c_str(), cloud.shDegree);
        shPerPoint = 0;
    }

    vertices.resize(numPoints);

    for (uint32_t i = 0; i < numPoints; i++) {
        GsVertex& v = vertices[i];

        // Position (w=1)
        v.position[0] = cloud.positions[i * 3 + 0];
        v.position[1] = cloud.positions[i * 3 + 1];
        v.position[2] = cloud.positions[i * 3 + 2];
        v.position[3] = 1.0f;

        // Scale: exp(log_scale), Opacity: sigmoid(logit_alpha)
        v.scale_opacity[0] = std::exp(cloud.scales[i * 3 + 0]);
        v.scale_opacity[1] = std::exp(cloud.scales[i * 3 + 1]);
        v.scale_opacity[2] = std::exp(cloud.scales[i * 3 + 2]);
        v.scale_opacity[3] = spz_sigmoid(cloud.alphas[i]);

        // Quaternion: SPZ stores (x,y,z,w), we need (w,x,y,z)
        float qx = cloud.rotations[i * 4 + 0];
        float qy = cloud.rotations[i * 4 + 1];
        float qz = cloud.rotations[i * 4 + 2];
        float qw = cloud.rotations[i * 4 + 3];
        float qlen = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
        if (qlen > 1e-8f) {
            float inv = 1.0f / qlen;
            qw *= inv; qx *= inv; qy *= inv; qz *= inv;
        }
        v.rotation[0] = qw;
        v.rotation[1] = qx;
        v.rotation[2] = qy;
        v.rotation[3] = qz;

        // DC color (SH band 0) → sh[0..2]
        v.sh[0] = cloud.colors[i * 3 + 0];
        v.sh[1] = cloud.colors[i * 3 + 1];
        v.sh[2] = cloud.colors[i * 3 + 2];

        // Higher-order SH coefficients
        // SPZ uses color-inner layout: [coeff0_R, coeff0_G, coeff0_B, coeff1_R, ...]
        // Our GPU layout sh[3..47] is identical: [R_1, G_1, B_1, R_2, G_2, B_2, ...]
        if (shPerPoint > 0) {
            int floatsToCopy = std::min(shPerPoint, 45);
            std::memcpy(&v.sh[3], &cloud.sh[(size_t)i * (size_t)shPerPoint],
                        floatsToCopy * sizeof(float));
            // Zero-fill remaining coefficients if shDegree < 3
            for (int j = 3 + floatsToCopy; j < 48; j++) {
                v.sh[j] = 0.0f;
            }
        } else {
            // No SH beyond DC — zero-fill all higher coefficients
            std::memset(&v.sh[3], 0, 45 * sizeof(float));
        }
    }

    printf("ParseSpzFile: loaded %u gaussians (%.1f MB GPU data)\n",
           numPoints, (double)(numPoints * sizeof(GsVertex)) / (1024.0 * 1024.0));
    return true;
}

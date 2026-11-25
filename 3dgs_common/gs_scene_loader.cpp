// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  PLY scene loading utilities + binary PLY parser implementation
 */

#include "gs_scene_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

// On-disk PLY vertex: 62 floats = 248 bytes
struct PlyVertexStorage {
    float x, y, z;           // position (3)
    float nx, ny, nz;        // normals (3) — always 0, discarded
    float shs[48];           // SH coefficients in PLY order (48)
    float opacity;           // raw logit opacity (1)
    float sx, sy, sz;        // raw log scale (3)
    float rot_w, rot_x, rot_y, rot_z;  // quaternion (4)
};
static_assert(sizeof(PlyVertexStorage) == 248, "PlyVertexStorage must be 248 bytes");

static float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

namespace {

//! Refuse absurd inputs before allocating. The gaussian ceiling is the same
//! one the SOG loader applies (gs_sog_loader.cpp kMaxGaussians): a count past
//! it is a corrupt header, not a scene.
constexpr uint64_t kMaxPlyGaussians   = 100u * 1000 * 1000;
//! One header line. The longest legitimate line is a `comment`; 4 KB is far
//! past anything an exporter writes.
constexpr size_t   kMaxPlyHeaderLine  = 4096;
//! The whole header. The 62-property layout is ~1.5 KB; comments add a little.
constexpr size_t   kMaxPlyHeaderBytes = 64u * 1024;

//! The standard 62-float 3DGS export, in exactly this order (the
//! PlyVertexStorage struct above). A file whose vertex element is exactly this
//! list takes the bulk-read fast path; every other layout is decoded by name.
std::vector<std::string> ExpectedPlyVertexProperties()
{
    std::vector<std::string> names = {"x", "y", "z", "nx", "ny", "nz",
                                      "f_dc_0", "f_dc_1", "f_dc_2"};
    for (int i = 0; i < 45; i++) names.push_back("f_rest_" + std::to_string(i));
    names.push_back("opacity");
    for (int i = 0; i < 3; i++) names.push_back("scale_" + std::to_string(i));
    for (int i = 0; i < 4; i++) names.push_back("rot_" + std::to_string(i));
    return names;
}

//! Read one header line (up to '\n', '\r' stripped) without letting a file
//! with no newline pull an unbounded string into memory. False on EOF, on a
//! line longer than kMaxPlyHeaderLine, or once the header passes
//! kMaxPlyHeaderBytes in total; `error` says which.
bool ReadPlyHeaderLine(std::istream& in, std::string& line, size_t& headerBytes,
                       std::string& error)
{
    line.clear();
    for (;;) {
        const int c = in.get();
        if (c == std::char_traits<char>::eof()) {
            error = "header ended before end_header";
            return false;
        }
        if (++headerBytes > kMaxPlyHeaderBytes) {
            error = "header is longer than " + std::to_string(kMaxPlyHeaderBytes) +
                    " bytes without an end_header";
            return false;
        }
        if (c == '\n') break;
        if (line.size() >= kMaxPlyHeaderLine) {
            error = "header line longer than " + std::to_string(kMaxPlyHeaderLine) + " bytes";
            return false;
        }
        line.push_back((char)c);
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

//! Parse a decimal element count. Rejects signs, junk and anything that does
//! not fit in 64 bits.
bool ParsePlyCount(const std::string& token, uint64_t& out)
{
    if (token.empty() || token.size() > 20) return false;
    for (char ch : token) {
        if (ch < '0' || ch > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(token.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') return false;
    out = (uint64_t)v;
    return true;
}

//! Byte size of a PLY scalar type (both the classic and the sized names), or
//! 0 for a type this loader does not know.
size_t PlyScalarSize(const std::string& type)
{
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2;
    if (type == "int" || type == "uint" || type == "int32" || type == "uint32") return 4;
    if (type == "float" || type == "float32") return 4;
    if (type == "double" || type == "float64") return 8;
    return 0;
}

bool IsPlyFloat32(const std::string& type)
{
    return type == "float" || type == "float32";
}

//! Where each property the renderer needs sits inside one vertex record, as
//! byte offsets, plus the record stride. Built from the header BY NAME, so the
//! property order does not matter, normals are optional, unknown scalar
//! properties are stepped over, and the SH rest block may be absent (degree 0)
//! or hold degree 1/2/3.
struct PlyLayout {
    size_t stride = 0;
    size_t pos[3] = {};
    size_t dc[3] = {};
    size_t opacity = 0;
    size_t scale[3] = {};
    size_t rot[4] = {};
    //! f_rest_0..N-1 in file order. N is 0, 9, 24 or 45 (degree 0..3): PLY
    //! stores the rest coefficients channel-major, N/3 per channel.
    std::vector<size_t> rest;
    //! True when the vertex element is exactly ExpectedPlyVertexProperties(),
    //! all float32: the record is then a PlyVertexStorage and is bulk-read.
    bool standard62 = false;
};

bool BuildPlyLayout(const std::vector<std::pair<std::string, std::string>>& props,
                    PlyLayout& layout, std::string& error)
{
    constexpr size_t kMissing = (size_t)-1;
    const char* const required[] = {"x",       "y",       "z",       "f_dc_0", "f_dc_1",
                                    "f_dc_2",  "opacity", "scale_0", "scale_1", "scale_2",
                                    "rot_0",   "rot_1",   "rot_2",   "rot_3"};
    constexpr size_t kRequired = sizeof(required) / sizeof(required[0]);
    size_t reqOffset[kRequired];
    for (size_t& o : reqOffset) o = kMissing;

    std::vector<size_t> restOffset(45, kMissing);
    size_t restCount = 0;
    std::vector<std::string> seen;
    seen.reserve(props.size());

    size_t offset = 0;
    for (const auto& prop : props) {
        const std::string& type = prop.first;
        const std::string& name = prop.second;
        const size_t size = PlyScalarSize(type);
        if (size == 0) {
            error = "vertex property '" + name + "' has unsupported type '" + type + "'";
            return false;
        }
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
            error = "vertex property '" + name + "' is declared more than once";
            return false;
        }
        seen.push_back(name);

        bool isRequired = false;
        for (size_t r = 0; r < kRequired; r++) {
            if (name == required[r]) {
                if (!IsPlyFloat32(type)) {
                    error = "vertex property '" + name + "' must be float, found '" + type + "'";
                    return false;
                }
                reqOffset[r] = offset;
                isRequired = true;
                break;
            }
        }
        if (!isRequired && name.rfind("f_rest_", 0) == 0) {
            uint64_t idx = 0;
            if (!ParsePlyCount(name.substr(7), idx) || idx >= 45) {
                error = "vertex property '" + name +
                        "' is not an SH rest coefficient this loader understands "
                        "(f_rest_0..f_rest_44)";
                return false;
            }
            if (!IsPlyFloat32(type)) {
                error = "vertex property '" + name + "' must be float, found '" + type + "'";
                return false;
            }
            restOffset[(size_t)idx] = offset;
            restCount++;
        }
        // nx/ny/nz and any other property: present in the record, not read.
        offset += size;
    }
    layout.stride = offset;

    for (size_t r = 0; r < kRequired; r++) {
        if (reqOffset[r] == kMissing) {
            error = std::string("missing required vertex property '") + required[r] +
                    "' (a 3DGS PLY needs x y z f_dc_0..2 opacity scale_0..2 rot_0..3)";
            return false;
        }
    }

    if (restCount != 0 && restCount != 9 && restCount != 24 && restCount != 45) {
        error = "vertex element has " + std::to_string(restCount) +
                " f_rest properties; expected 0, 9, 24 or 45 (SH degree 0..3)";
        return false;
    }
    layout.rest.clear();
    for (size_t i = 0; i < restCount; i++) {
        if (restOffset[i] == kMissing) {
            error = "vertex element has " + std::to_string(restCount) +
                    " f_rest properties but no 'f_rest_" + std::to_string(i) + "'";
            return false;
        }
        layout.rest.push_back(restOffset[i]);
    }

    for (int i = 0; i < 3; i++) layout.pos[i] = reqOffset[i];
    for (int i = 0; i < 3; i++) layout.dc[i] = reqOffset[3 + i];
    layout.opacity = reqOffset[6];
    for (int i = 0; i < 3; i++) layout.scale[i] = reqOffset[7 + i];
    for (int i = 0; i < 4; i++) layout.rot[i] = reqOffset[10 + i];

    const std::vector<std::string> expected = ExpectedPlyVertexProperties();
    layout.standard62 = props.size() == expected.size();
    for (size_t i = 0; layout.standard62 && i < props.size(); i++) {
        layout.standard62 = IsPlyFloat32(props[i].first) && props[i].second == expected[i];
    }
    return true;
}

//! Unpack one vertex record of any layout into the standard 62-float storage
//! (normals zero, absent SH rest coefficients zero), so a single conversion
//! path serves every layout.
void DecodePlyRecord(const unsigned char* rec, const PlyLayout& L, PlyVertexStorage& s)
{
    auto rd = [rec](size_t off) {
        float f;
        std::memcpy(&f, rec + off, sizeof(f));
        return f;
    };
    std::memset(&s, 0, sizeof(s));
    s.x = rd(L.pos[0]);
    s.y = rd(L.pos[1]);
    s.z = rd(L.pos[2]);
    s.shs[0] = rd(L.dc[0]);
    s.shs[1] = rd(L.dc[1]);
    s.shs[2] = rd(L.dc[2]);
    // PLY rest block is channel-major with k = N/3 coefficients per channel;
    // the storage holds 15 per channel, so degree < 3 leaves the tail zero.
    const size_t k = L.rest.size() / 3;
    for (size_t c = 0; c < 3; c++) {
        for (size_t j = 0; j < k; j++) {
            s.shs[3 + c * 15 + j] = rd(L.rest[c * k + j]);
        }
    }
    s.opacity = rd(L.opacity);
    s.sx = rd(L.scale[0]);
    s.sy = rd(L.scale[1]);
    s.sz = rd(L.scale[2]);
    s.rot_w = rd(L.rot[0]);
    s.rot_x = rd(L.rot[1]);
    s.rot_y = rd(L.rot[2]);
    s.rot_z = rd(L.rot[3]);
}

bool ParsePlyFileImpl(const std::string& path, std::vector<GsVertex>& vertices,
                      std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "cannot open file";
        return false;
    }

    // ── Header ──────────────────────────────────────────────────────────
    // Every line is read, not just the vertex count: the format, the element
    // order and the vertex property list all decide how a vertex record is
    // laid out (see BuildPlyLayout).
    std::string line;
    size_t headerBytes = 0;
    if (!ReadPlyHeaderLine(file, line, headerBytes, error)) return false;
    if (line != "ply") {
        error = "not a PLY file (first line is not 'ply')";
        return false;
    }

    bool haveFormat = false;
    bool haveVertex = false;
    bool inVertex = false;
    bool foundEnd = false;
    uint64_t numVertices = 0;
    // "<type> <name>" per vertex property, in file order.
    std::vector<std::pair<std::string, std::string>> vertexProps;

    while (ReadPlyHeaderLine(file, line, headerBytes, error)) {
        std::istringstream ls(line);
        std::string keyword;
        ls >> keyword;

        if (keyword.empty() || keyword == "comment" || keyword == "obj_info") {
            continue;
        }
        if (keyword == "end_header") {
            foundEnd = true;
            break;
        }
        if (keyword == "format") {
            std::string fmt, ver, extra;
            ls >> fmt >> ver >> extra;
            if (haveFormat) {
                error = "header has more than one format line";
                return false;
            }
            if (fmt != "binary_little_endian" || ver != "1.0" || !extra.empty()) {
                error = "unsupported PLY format '" + line +
                        "' (only 'format binary_little_endian 1.0' is supported)";
                return false;
            }
            haveFormat = true;
            continue;
        }
        if (keyword == "element") {
            std::string name, countTok, extra;
            ls >> name >> countTok >> extra;
            uint64_t count = 0;
            if (name.empty() || !extra.empty() || !ParsePlyCount(countTok, count)) {
                error = "malformed element line '" + line + "'";
                return false;
            }
            if (name == "vertex") {
                if (haveVertex) {
                    error = "header declares more than one vertex element";
                    return false;
                }
                haveVertex = true;
                inVertex = true;
                numVertices = count;
            } else {
                // Elements AFTER the vertex block sit past the bytes read
                // here and are ignored. One BEFORE it would put its data
                // where the vertices are expected.
                if (!haveVertex) {
                    error = "element '" + name +
                            "' precedes the vertex element (vertex must be first)";
                    return false;
                }
                inVertex = false;
            }
            continue;
        }
        if (keyword == "property") {
            if (!haveVertex) {
                error = "property line before any element: '" + line + "'";
                return false;
            }
            if (!inVertex) continue;  // a later element's property; not read
            std::string type, name, extra;
            ls >> type >> name >> extra;
            if (type == "list") {
                error = "vertex element has a list property ('" + line +
                        "'); only scalar float properties are supported";
                return false;
            }
            if (type.empty() || name.empty() || !extra.empty()) {
                error = "malformed property line '" + line + "'";
                return false;
            }
            vertexProps.emplace_back(type, name);
            continue;
        }
        error = "unrecognised header line '" + line + "'";
        return false;
    }
    if (!foundEnd) return false;  // `error` set by ReadPlyHeaderLine

    if (!haveFormat) {
        error = "header has no format line";
        return false;
    }
    if (!haveVertex) {
        error = "header has no vertex element";
        return false;
    }

    // ── Vertex layout, by property name ────────────────────────────────
    PlyLayout layout;
    if (!BuildPlyLayout(vertexProps, layout, error)) return false;
    static_assert(sizeof(PlyVertexStorage) == 62 * sizeof(float),
                  "PlyVertexStorage must be the 62-float layout");

    // ── Count vs ceiling vs the bytes actually in the file ─────────────
    if (numVertices == 0) {
        error = "vertex element declares 0 gaussians";
        return false;
    }
    if (numVertices > kMaxPlyGaussians) {
        error = "vertex count " + std::to_string(numVertices) + " exceeds the " +
                std::to_string(kMaxPlyGaussians) + " gaussian ceiling";
        return false;
    }

    const std::streamoff dataStart = (std::streamoff)file.tellg();
    file.seekg(0, std::ios::end);
    const std::streamoff fileEnd = (std::streamoff)file.tellg();
    if (!file || dataStart < 0 || fileEnd < dataStart) {
        error = "cannot determine file size";
        return false;
    }
    file.seekg(dataStart, std::ios::beg);
    const uint64_t remaining = (uint64_t)(fileEnd - dataStart);
    const uint64_t needed = numVertices * (uint64_t)layout.stride;
    if (needed > remaining) {
        error = "header declares " + std::to_string(numVertices) + " gaussians (" +
                std::to_string(needed) + " bytes) but only " + std::to_string(remaining) +
                " bytes follow the header";
        return false;
    }
    // A 32-bit ABI cannot address the block even when the file holds it.
    if (needed > (uint64_t)(std::numeric_limits<size_t>::max)() ||
        needed > (uint64_t)(std::numeric_limits<std::streamsize>::max)()) {
        error = "vertex data (" + std::to_string(needed) +
                " bytes) is too large for this build";
        return false;
    }

    const size_t count = (size_t)numVertices;
    printf("ParsePlyFile: loading %zu gaussians from %s (%zu-byte records, SH degree %d%s)\n",
           count, path.c_str(), layout.stride,
           layout.rest.empty() ? 0 : layout.rest.size() == 9 ? 1 : layout.rest.size() == 24 ? 2 : 3,
           layout.standard62 ? ", standard layout" : "");

    vertices.resize(count);

    // Fast path: the standard 62-float layout is read straight into
    // PlyVertexStorage in one go. Any other layout is read in chunks of raw
    // records and unpacked by name into the same storage.
    std::vector<PlyVertexStorage> storage;
    std::vector<unsigned char> raw;
    size_t chunk = count;
    if (layout.standard62) {
        storage.resize(count);
        file.read(reinterpret_cast<char*>(storage.data()), (std::streamsize)needed);
        if (!file) {
            error = "failed to read vertex data (expected " + std::to_string(needed) + " bytes)";
            return false;
        }
    } else {
        chunk = (std::min)(count, (size_t)65536);
        raw.resize(chunk * layout.stride);
        storage.resize(1);
    }

    for (size_t base = 0; base < count; base += chunk) {
        const size_t n = (std::min)(chunk, count - base);
        if (!layout.standard62) {
            file.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)(n * layout.stride));
            if (!file) {
                error = "failed to read vertex data (expected " + std::to_string(needed) +
                        " bytes)";
                return false;
            }
        }
        for (size_t ci = 0; ci < n; ci++) {
            const size_t i = base + ci;
            if (!layout.standard62) {
                DecodePlyRecord(raw.data() + ci * layout.stride, layout, storage[0]);
            }
            const PlyVertexStorage& s = layout.standard62 ? storage[i] : storage[0];
            GsVertex& v = vertices[i];

            // gsplat-trainer PLY: RDF (Y-down, Z-forward) plus an X-mirror vs
            // SuperSplat reference. Convert to canonical RUB (+X right, +Y up,
            // +Z back) at load so the renderer / app code can stay in one
            // consistent +Y-up convention end-to-end. Without this, runtime
            // view-stage flips would have to compensate (and that pattern caused
            // the eye-Y / click-NDC / teleport-target Y inconsistencies).
            v.position[0] = -s.x;   // undo X-mirror
            v.position[1] = -s.y;   // RDF Y-down → canonical Y-up
            v.position[2] = -s.z;   // RDF Z-forward → canonical Z-back
            v.position[3] = 1.0f;

            // Scale: exp(raw), Opacity: sigmoid(raw)
            v.scale_opacity[0] = std::exp(s.sx);
            v.scale_opacity[1] = std::exp(s.sy);
            v.scale_opacity[2] = std::exp(s.sz);
            v.scale_opacity[3] = sigmoid(s.opacity);

            // Rotation: normalize quaternion
            float qw = s.rot_w, qx = s.rot_x, qy = s.rot_y, qz = s.rot_z;
            float qlen = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
            if (qlen > 1e-8f) {
                float inv = 1.0f / qlen;
                qw *= inv; qx *= inv; qy *= inv; qz *= inv;
            }
            // Quaternion under the (-X, -Y, -Z) coord-flip composition:
            // for q = (w, x, y, z), the rotation conjugated by the -I sign flip
            // on the imaginary axes leaves the imaginary part unchanged in a
            // pure 180-about-anything sense; but here we're flipping all three
            // axes, which is equivalent to negating w (or equivalently the
            // double-cover identity gives same rotation). Empirically this
            // pattern matches the position transform for the X+Y+Z mirror:
            v.rotation[0] = qw;  // w first (matches shader convention: q.x = w)
            v.rotation[1] = -qx;
            v.rotation[2] =  qy;
            v.rotation[3] = -qz;

            // SH coefficients: de-interleave from PLY layout to GPU layout.
            // PLY:  [R_dc, G_dc, B_dc, R_1..R_15, G_1..G_15, B_1..B_15]
            // GPU:  [R_dc, G_dc, B_dc, R_1, G_1, B_1, R_2, G_2, B_2, ...]
            v.sh[0] = s.shs[0];  // R DC
            v.sh[1] = s.shs[1];  // G DC
            v.sh[2] = s.shs[2];  // B DC

            for (int j = 1; j < 16; j++) {
                v.sh[j * 3 + 0] = s.shs[(j - 1) + 3];       // R channel
                v.sh[j * 3 + 1] = s.shs[(j - 1) + 18];      // G channel (3 + 15)
                v.sh[j * 3 + 2] = s.shs[(j - 1) + 33];      // B channel (3 + 30)
            }
        }
    }

    printf("ParsePlyFile: loaded %zu gaussians (%.1f MB GPU data)\n",
           count, (double)count * (double)sizeof(GsVertex) / (1024.0 * 1024.0));
    return true;
}

} // namespace

bool ParsePlyFile(const std::string& path, std::vector<GsVertex>& vertices,
                  std::string* error)
{
    vertices.clear();
    std::string err;
    bool ok = false;
    // A parse failure is a RETURN, never an exception: the header is checked
    // against the file size before anything is allocated, but an allocation
    // can still fail on a large valid scene.
    try {
        ok = ParsePlyFileImpl(path, vertices, err);
    } catch (const std::bad_alloc&) {
        err = "out of memory while loading the scene";
        ok = false;
    } catch (const std::length_error&) {
        err = "scene is too large to hold in memory";
        ok = false;
    }
    if (!ok) {
        vertices.clear();
        vertices.shrink_to_fit();
        if (err.empty()) err = "unknown error";
        fprintf(stderr, "ParsePlyFile: %s - %s\n", path.c_str(), err.c_str());
        if (error) *error = err;
    }
    return ok;
}

bool ValidatePlyFile(const std::string& path)
{
    if (path.empty()) return false;

    // Check extension
    std::filesystem::path p(path);
    auto ext = p.extension().string();
    if (ext != ".ply" && ext != ".PLY") return false;

    // Check file exists
    return std::filesystem::exists(p);
}

bool ValidateSceneFile(const std::string& path)
{
    if (path.empty()) return false;

    std::filesystem::path p(path);
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".ply" && ext != ".spz" && ext != ".sog") return false;

    return std::filesystem::exists(p);
}

std::string GetPlyFilename(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string GetPlyFileSize(const std::string& path)
{
    try {
        auto size = std::filesystem::file_size(path);
        if (size >= 1024 * 1024 * 1024) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024 * 1024) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f MB", (double)size / (1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f KB", (double)size / 1024.0);
            return buf;
        } else {
            return std::to_string(size) + " B";
        }
    } catch (...) {
        return "unknown";
    }
}

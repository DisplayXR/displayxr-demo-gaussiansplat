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
#include <bit>
#include <cstdint>
#include <memory>
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

//! PLY scalar types (classic and sized names map to the same entry).
enum class PlyType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64 };

bool ParsePlyType(const std::string& s, PlyType& t)
{
    if (s == "char" || s == "int8") t = PlyType::Int8;
    else if (s == "uchar" || s == "uint8") t = PlyType::UInt8;
    else if (s == "short" || s == "int16") t = PlyType::Int16;
    else if (s == "ushort" || s == "uint16") t = PlyType::UInt16;
    else if (s == "int" || s == "int32") t = PlyType::Int32;
    else if (s == "uint" || s == "uint32") t = PlyType::UInt32;
    else if (s == "float" || s == "float32") t = PlyType::Float32;
    else if (s == "double" || s == "float64") t = PlyType::Float64;
    else return false;
    return true;
}

size_t PlyTypeSize(PlyType t)
{
    switch (t) {
    case PlyType::Int8:
    case PlyType::UInt8: return 1;
    case PlyType::Int16:
    case PlyType::UInt16: return 2;
    case PlyType::Int32:
    case PlyType::UInt32:
    case PlyType::Float32: return 4;
    case PlyType::Float64: return 8;
    }
    return 0;
}

bool PlyTypeIsInteger(PlyType t)
{
    return t != PlyType::Float32 && t != PlyType::Float64;
}

enum class PlyFormat { Ascii, BinaryLittleEndian, BinaryBigEndian };

//! One property of an element. `typeName` is kept for messages; `typeOk` is
//! false for a type this loader does not know, which only matters if the
//! element has to be read or stepped over (elements after the vertex block
//! are never touched).
struct PlyProperty {
    std::string name;
    std::string typeName;
    PlyType type = PlyType::Float32;
    bool typeOk = false;
    bool isList = false;
    PlyType countType = PlyType::UInt8;
};

struct PlyElement {
    std::string name;
    uint64_t count = 0;
    std::vector<PlyProperty> props;
};

//! Decode one binary scalar of any PLY type, in either byte order, to double.
double ReadPlyScalar(const unsigned char* p, PlyType t, bool swap)
{
    unsigned char b[8];
    const size_t n = PlyTypeSize(t);
    if (swap) {
        for (size_t i = 0; i < n; i++) b[i] = p[n - 1 - i];
    } else {
        std::memcpy(b, p, n);
    }
    switch (t) {
    case PlyType::Int8:    { int8_t v;   std::memcpy(&v, b, 1); return v; }
    case PlyType::UInt8:   { uint8_t v;  std::memcpy(&v, b, 1); return v; }
    case PlyType::Int16:   { int16_t v;  std::memcpy(&v, b, 2); return v; }
    case PlyType::UInt16:  { uint16_t v; std::memcpy(&v, b, 2); return v; }
    case PlyType::Int32:   { int32_t v;  std::memcpy(&v, b, 4); return v; }
    case PlyType::UInt32:  { uint32_t v; std::memcpy(&v, b, 4); return v; }
    case PlyType::Float32: { float v;    std::memcpy(&v, b, 4); return v; }
    case PlyType::Float64: { double v;   std::memcpy(&v, b, 8); return v; }
    }
    return 0.0;
}

//! Whitespace-separated tokens from the ASCII body, through a fixed buffer
//! (no unbounded line reads). A token longer than kMaxToken is an error: no
//! number needs it.
class PlyAsciiReader {
public:
    explicit PlyAsciiReader(std::istream& in) : in_(in) {}

    //! False at end of data or on an over-long token (`tooLong` tells which).
    bool Next(std::string& tok, bool& tooLong)
    {
        tok.clear();
        tooLong = false;
        int c;
        do {
            c = Get();
            if (c < 0) return false;
        } while (IsSpace(c));
        while (c >= 0 && !IsSpace(c)) {
            if (tok.size() >= kMaxToken) {
                tooLong = true;
                return false;
            }
            tok.push_back((char)c);
            c = Get();
        }
        return true;
    }

private:
    static constexpr size_t kMaxToken = 64;
    static bool IsSpace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
    int Get()
    {
        if (pos_ == len_) {
            in_.read(buf_, sizeof(buf_));
            len_ = (size_t)in_.gcount();
            pos_ = 0;
            if (len_ == 0) return -1;
        }
        return (unsigned char)buf_[pos_++];
    }
    std::istream& in_;
    char buf_[64 * 1024];
    size_t pos_ = 0;
    size_t len_ = 0;
};

//! Parse an ASCII number (integer or floating) in full; junk is an error.
bool ParsePlyAsciiNumber(const std::string& tok, double& out)
{
    if (tok.empty()) return false;
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end != nullptr && *end == '\0';
}

//! Where each property the renderer needs sits inside one vertex record: its
//! byte offset (binary), its index in the property list (ASCII) and its type.
struct PlyField {
    size_t offset = 0;
    size_t index = 0;
    PlyType type = PlyType::Float32;
};

//! The vertex record layout, built from the header BY NAME: the property
//! order does not matter, normals are optional, unknown scalar properties are
//! stepped over, any scalar type is converted, and the SH rest block may be
//! absent (degree 0) or hold degree 1/2/3.
struct PlyLayout {
    size_t stride = 0;
    PlyField pos[3];
    PlyField dc[3];
    PlyField opacity;
    PlyField scale[3];
    PlyField rot[4];
    //! f_rest_0..N-1 in file order. N is 0, 9, 24 or 45 (degree 0..3): PLY
    //! stores the rest coefficients channel-major, N/3 per channel.
    std::vector<PlyField> rest;
    //! True when the vertex element is exactly ExpectedPlyVertexProperties(),
    //! all float32: in a little-endian file the record is then a
    //! PlyVertexStorage and is bulk-read.
    bool standard62 = false;
};

bool BuildPlyLayout(const std::vector<PlyProperty>& props, PlyLayout& layout, std::string& error)
{
    const char* const required[] = {"x",       "y",       "z",       "f_dc_0", "f_dc_1",
                                    "f_dc_2",  "opacity", "scale_0", "scale_1", "scale_2",
                                    "rot_0",   "rot_1",   "rot_2",   "rot_3"};
    constexpr size_t kRequired = sizeof(required) / sizeof(required[0]);
    PlyField reqField[kRequired];
    bool reqSeen[kRequired] = {};

    std::vector<PlyField> restField(45);
    std::vector<bool> restSeen(45, false);
    size_t restCount = 0;
    std::vector<std::string> seen;
    seen.reserve(props.size());

    size_t offset = 0;
    for (size_t pi = 0; pi < props.size(); pi++) {
        const PlyProperty& prop = props[pi];
        const std::string& name = prop.name;
        if (prop.isList) {
            error = "vertex element has a list property '" + name +
                    "'; only scalar vertex properties are supported";
            return false;
        }
        if (!prop.typeOk) {
            error = "vertex property '" + name + "' has unsupported type '" + prop.typeName + "'";
            return false;
        }
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
            error = "vertex property '" + name + "' is declared more than once";
            return false;
        }
        seen.push_back(name);

        const PlyField field{offset, pi, prop.type};
        bool isRequired = false;
        for (size_t r = 0; r < kRequired; r++) {
            if (name == required[r]) {
                reqField[r] = field;
                reqSeen[r] = true;
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
            restField[(size_t)idx] = field;
            restSeen[(size_t)idx] = true;
            restCount++;
        }
        // nx/ny/nz and any other property: present in the record, not read.
        offset += PlyTypeSize(prop.type);
    }
    layout.stride = offset;

    for (size_t r = 0; r < kRequired; r++) {
        if (!reqSeen[r]) {
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
        if (!restSeen[i]) {
            error = "vertex element has " + std::to_string(restCount) +
                    " f_rest properties but no 'f_rest_" + std::to_string(i) + "'";
            return false;
        }
        layout.rest.push_back(restField[i]);
    }

    for (int i = 0; i < 3; i++) layout.pos[i] = reqField[i];
    for (int i = 0; i < 3; i++) layout.dc[i] = reqField[3 + i];
    layout.opacity = reqField[6];
    for (int i = 0; i < 3; i++) layout.scale[i] = reqField[7 + i];
    for (int i = 0; i < 4; i++) layout.rot[i] = reqField[10 + i];

    const std::vector<std::string> expected = ExpectedPlyVertexProperties();
    layout.standard62 = props.size() == expected.size();
    for (size_t i = 0; layout.standard62 && i < props.size(); i++) {
        layout.standard62 = props[i].type == PlyType::Float32 && props[i].name == expected[i];
    }
    return true;
}

//! Unpack one vertex record of any layout into the standard 62-float storage
//! (normals zero, absent SH rest coefficients zero), so a single conversion
//! path serves every layout and format. `get` returns a field's value.
template <typename Get>
void DecodePlyRecord(const PlyLayout& L, const Get& get, PlyVertexStorage& s)
{
    std::memset(&s, 0, sizeof(s));
    s.x = get(L.pos[0]);
    s.y = get(L.pos[1]);
    s.z = get(L.pos[2]);
    s.shs[0] = get(L.dc[0]);
    s.shs[1] = get(L.dc[1]);
    s.shs[2] = get(L.dc[2]);
    // PLY rest block is channel-major with k = N/3 coefficients per channel;
    // the storage holds 15 per channel, so degree < 3 leaves the tail zero.
    const size_t k = L.rest.size() / 3;
    for (size_t c = 0; c < 3; c++) {
        for (size_t j = 0; j < k; j++) {
            s.shs[3 + c * 15 + j] = get(L.rest[c * k + j]);
        }
    }
    s.opacity = get(L.opacity);
    s.sx = get(L.scale[0]);
    s.sy = get(L.scale[1]);
    s.sz = get(L.scale[2]);
    s.rot_w = get(L.rot[0]);
    s.rot_x = get(L.rot[1]);
    s.rot_y = get(L.rot[2]);
    s.rot_z = get(L.rot[3]);
}

//! PlyVertexStorage (raw 3DGS values, PLY coordinates) -> GsVertex.
void ConvertPlyVertex(const PlyVertexStorage& s, GsVertex& v)
{
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

//! Step over every record of a binary element that precedes the vertex
//! block. All-scalar elements are one seek (checked against the file size
//! first); elements with list properties are walked record by record.
bool SkipBinaryPlyElement(std::istream& in, const PlyElement& el, bool swap,
                          std::streamoff fileEnd, std::string& error)
{
    bool hasList = false;
    uint64_t stride = 0;
    for (const PlyProperty& p : el.props) {
        if (!p.typeOk) {
            error = "element '" + el.name + "' property '" + p.name + "' has unsupported type '" +
                    p.typeName + "'";
            return false;
        }
        if (p.isList) {
            if (!PlyTypeIsInteger(p.countType)) {
                error = "element '" + el.name + "' list '" + p.name +
                        "' has a non-integer count type";
                return false;
            }
            hasList = true;
        } else {
            stride += PlyTypeSize(p.type);
        }
    }
    const std::string ends = "element '" + el.name + "' (before the vertex element) ends early";

    if (!hasList) {
        if (stride == 0 || el.count == 0) return true;
        const std::streamoff here = (std::streamoff)in.tellg();
        if (here < 0 || here > fileEnd) {
            error = "cannot determine file position";
            return false;
        }
        const uint64_t remaining = (uint64_t)(fileEnd - here);
        if (el.count > remaining / stride) {
            error = ends;
            return false;
        }
        in.seekg((std::streamoff)(el.count * stride), std::ios::cur);
        if (!in) {
            error = ends;
            return false;
        }
        return true;
    }

    // ignore() at end of file sets only eofbit, so the byte count is checked.
    auto skip = [&in](uint64_t bytes) {
        in.ignore((std::streamsize)bytes);
        return (uint64_t)in.gcount() == bytes;
    };
    unsigned char buf[8];
    for (uint64_t r = 0; r < el.count; r++) {
        for (const PlyProperty& p : el.props) {
            if (!p.isList) {
                if (!skip(PlyTypeSize(p.type))) {
                    error = ends;
                    return false;
                }
                continue;
            }
            in.read(reinterpret_cast<char*>(buf), (std::streamsize)PlyTypeSize(p.countType));
            if (!in) {
                error = ends;
                return false;
            }
            const double n = ReadPlyScalar(buf, p.countType, swap);
            if (n < 0) {
                error = "element '" + el.name + "' list '" + p.name + "' has a negative count";
                return false;
            }
            const uint64_t bytes = (uint64_t)n * PlyTypeSize(p.type);
            if (bytes > (uint64_t)fileEnd || !skip(bytes)) {
                error = ends;
                return false;
            }
        }
    }
    return true;
}

//! Same, ASCII: consume the element's tokens (list counts decide how many).
bool SkipAsciiPlyElement(PlyAsciiReader& rd, const PlyElement& el, std::string& error)
{
    const std::string ends = "element '" + el.name + "' (before the vertex element) ends early";
    std::string tok;
    bool tooLong = false;
    for (uint64_t r = 0; r < el.count; r++) {
        for (const PlyProperty& p : el.props) {
            uint64_t n = 1;
            if (p.isList) {
                if (!rd.Next(tok, tooLong)) {
                    error = tooLong ? "over-long token in element '" + el.name + "'" : ends;
                    return false;
                }
                if (!ParsePlyCount(tok, n)) {
                    error = "element '" + el.name + "' list '" + p.name + "' has a bad count '" +
                            tok + "'";
                    return false;
                }
            }
            for (uint64_t i = 0; i < n; i++) {
                if (!rd.Next(tok, tooLong)) {
                    error = tooLong ? "over-long token in element '" + el.name + "'" : ends;
                    return false;
                }
            }
        }
    }
    return true;
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
    // Every line is read: the format, every element and every property
    // decide where the vertex records are and how one is laid out.
    std::string line;
    size_t headerBytes = 0;
    if (!ReadPlyHeaderLine(file, line, headerBytes, error)) return false;
    if (line != "ply") {
        error = "not a PLY file (first line is not 'ply')";
        return false;
    }

    bool haveFormat = false;
    PlyFormat format = PlyFormat::BinaryLittleEndian;
    bool foundEnd = false;
    std::vector<PlyElement> elements;
    size_t vertexIndex = (size_t)-1;

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
            if (ver != "1.0" || !extra.empty()) {
                error = "unsupported PLY format '" + line + "' (only version 1.0 is supported)";
                return false;
            }
            if (fmt == "binary_little_endian") format = PlyFormat::BinaryLittleEndian;
            else if (fmt == "binary_big_endian") format = PlyFormat::BinaryBigEndian;
            else if (fmt == "ascii") format = PlyFormat::Ascii;
            else {
                error = "unsupported PLY format '" + line +
                        "' (expected ascii, binary_little_endian or binary_big_endian)";
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
                if (vertexIndex != (size_t)-1) {
                    error = "header declares more than one vertex element";
                    return false;
                }
                vertexIndex = elements.size();
            }
            elements.push_back(PlyElement{name, count, {}});
            continue;
        }
        if (keyword == "property") {
            if (elements.empty()) {
                error = "property line before any element: '" + line + "'";
                return false;
            }
            PlyProperty prop;
            std::string first, extra;
            ls >> first;
            if (first == "list") {
                std::string countType;
                ls >> countType >> prop.typeName >> prop.name >> extra;
                prop.isList = true;
                if (countType.empty() || !ParsePlyType(countType, prop.countType)) {
                    // Unknown count type: unreadable, but only an error if
                    // this element has to be read or stepped over.
                    prop.typeName = countType;
                    prop.typeOk = false;
                } else {
                    prop.typeOk = ParsePlyType(prop.typeName, prop.type);
                }
            } else {
                prop.typeName = first;
                ls >> prop.name >> extra;
                prop.typeOk = ParsePlyType(prop.typeName, prop.type);
            }
            if (prop.typeName.empty() || prop.name.empty() || !extra.empty()) {
                error = "malformed property line '" + line + "'";
                return false;
            }
            elements.back().props.push_back(prop);
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
    if (vertexIndex == (size_t)-1) {
        error = "header has no vertex element";
        return false;
    }
    const PlyElement& vertexEl = elements[vertexIndex];
    const uint64_t numVertices = vertexEl.count;

    // ── Vertex layout, by property name ────────────────────────────────
    PlyLayout layout;
    if (!BuildPlyLayout(vertexEl.props, layout, error)) return false;
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
    const bool ascii = format == PlyFormat::Ascii;
    const bool hostLittle = std::endian::native == std::endian::little;
    const bool swap = !ascii && ((format == PlyFormat::BinaryLittleEndian) != hostLittle);

    // Smallest vertex block the header allows: the record stride in binary;
    // in ASCII one character plus one separator per value (the last value of
    // the file may lack its separator). Checked from the end of the header,
    // i.e. before any preceding element is stepped over, so it is a lower
    // bound; binary is rechecked exactly at the vertex block below.
    {
        const uint64_t remaining = (uint64_t)(fileEnd - dataStart);
        const uint64_t needed = ascii
            ? numVertices * (uint64_t)vertexEl.props.size() * 2 - 1
            : numVertices * (uint64_t)layout.stride;
        if (needed > remaining) {
            error = "header declares " + std::to_string(numVertices) + " gaussians (at least " +
                    std::to_string(needed) + " bytes) but only " + std::to_string(remaining) +
                    " bytes follow the header";
            return false;
        }
    }

    // ── Elements declared before the vertex element: step over them ────
    std::unique_ptr<PlyAsciiReader> asciiReader;
    if (ascii) asciiReader = std::make_unique<PlyAsciiReader>(file);
    for (size_t e = 0; e < vertexIndex; e++) {
        const bool ok = ascii ? SkipAsciiPlyElement(*asciiReader, elements[e], error)
                              : SkipBinaryPlyElement(file, elements[e], swap, fileEnd, error);
        if (!ok) return false;
    }
    // Elements after the vertex block sit past the bytes read here and are
    // never read.

    uint64_t needed = numVertices * (uint64_t)layout.stride;
    if (!ascii) {
        const std::streamoff here = (std::streamoff)file.tellg();
        if (here < 0 || here > fileEnd) {
            error = "cannot determine file position";
            return false;
        }
        const uint64_t remaining = (uint64_t)(fileEnd - here);
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
    }

    const size_t count = (size_t)numVertices;
    const bool fast = layout.standard62 && format == PlyFormat::BinaryLittleEndian && hostLittle;
    printf("ParsePlyFile: loading %zu gaussians from %s (%s, %zu properties, SH degree %d%s)\n",
           count, path.c_str(),
           ascii ? "ascii" : format == PlyFormat::BinaryBigEndian ? "big-endian" : "little-endian",
           vertexEl.props.size(),
           layout.rest.empty() ? 0 : layout.rest.size() == 9 ? 1 : layout.rest.size() == 24 ? 2 : 3,
           fast ? ", standard layout" : "");

    vertices.resize(count);

    if (fast) {
        // The standard 62-float little-endian layout is read straight into
        // PlyVertexStorage in one go.
        std::vector<PlyVertexStorage> storage(count);
        file.read(reinterpret_cast<char*>(storage.data()), (std::streamsize)needed);
        if (!file) {
            error = "failed to read vertex data (expected " + std::to_string(needed) + " bytes)";
            return false;
        }
        for (size_t i = 0; i < count; i++) ConvertPlyVertex(storage[i], vertices[i]);
    } else if (!ascii) {
        // Any other binary layout: raw records in chunks, unpacked by name.
        const size_t chunk = (std::min)(count, (size_t)65536);
        std::vector<unsigned char> raw(chunk * layout.stride);
        PlyVertexStorage s;
        for (size_t base = 0; base < count; base += chunk) {
            const size_t n = (std::min)(chunk, count - base);
            file.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)(n * layout.stride));
            if (!file) {
                error = "failed to read vertex data (expected " + std::to_string(needed) +
                        " bytes)";
                return false;
            }
            for (size_t ci = 0; ci < n; ci++) {
                const unsigned char* rec = raw.data() + ci * layout.stride;
                DecodePlyRecord(
                    layout,
                    [rec, swap](const PlyField& f) {
                        return (float)ReadPlyScalar(rec + f.offset, f.type, swap);
                    },
                    s);
                ConvertPlyVertex(s, vertices[base + ci]);
            }
        }
    } else {
        // ASCII: one value per token, every value parsed (so junk anywhere
        // in the vertex block is an error, not a silent zero).
        std::vector<float> vals(vertexEl.props.size());
        std::string tok;
        bool tooLong = false;
        PlyVertexStorage s;
        for (size_t i = 0; i < count; i++) {
            for (size_t p = 0; p < vals.size(); p++) {
                double d = 0.0;
                if (!asciiReader->Next(tok, tooLong)) {
                    error = tooLong ? "over-long token in vertex " + std::to_string(i)
                                    : "vertex data ends at vertex " + std::to_string(i) + " of " +
                                          std::to_string(count);
                    return false;
                }
                if (!ParsePlyAsciiNumber(tok, d)) {
                    error = "vertex " + std::to_string(i) + " property '" +
                            vertexEl.props[p].name + "' is not a number: '" + tok + "'";
                    return false;
                }
                vals[p] = (float)d;
            }
            DecodePlyRecord(layout, [&vals](const PlyField& f) { return vals[f.index]; }, s);
            ConvertPlyVertex(s, vertices[i]);
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

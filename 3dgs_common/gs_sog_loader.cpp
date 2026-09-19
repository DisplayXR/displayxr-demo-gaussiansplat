// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  SOG scene loading — PlayCanvas SOG bundle -> GsVertex conversion
 *
 * Format, sources and the coordinate-system policy: see gs_sog_loader.h.
 *
 * Three self-contained pieces live in the anonymous namespace below, in
 * dependency order, so nothing outside this file grows a new include:
 *
 *   1. a minimal PKZip walker over zlib (stored + deflate; ZIP64 refused
 *      loudly). The repo already links zlib for the SPZ loader's gzip peel,
 *      so this costs no new dependency.
 *   2. a small recursive-descent JSON reader. meta.json nests objects and
 *      carries 256-entry float codebooks, which the two `JsonFindValue`
 *      key-scanners already in the platform mains cannot express; this one is
 *      ~200 lines and refuses depth/size abuse rather than recursing forever.
 *   3. the texel decode itself, a straight port of splat-transform's
 *      `readSogSourceV2`.
 *
 * The decoded attributes are staged in a `spz::GaussianCloud` and righted with
 * `convertCoordinates(RDF, RUB)` — the exact call the SPZ loader makes for a
 * v4 container. That is deliberate: it is the one place that knows how to turn
 * the quaternions, scales AND the SH bands through a coordinate change, and
 * sharing it is what makes "the same cloud as .spz and as .sog draws the same
 * picture" a property of the code rather than a coincidence.
 */

#include "gs_sog_loader.h"

#include <splat-types.h>

#include <webp/decode.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

//! Refuse absurd inputs before allocating. Mirrors the SPZ loader's ceilings.
//! uint64_t, not size_t: a 32-bit ABI (armeabi-v7a) would wrap the constant.
constexpr uint64_t kMaxFileBytes      = 2ull * 1024 * 1024 * 1024;
constexpr uint64_t kMaxInflatedBytes  = 2ull * 1024 * 1024 * 1024;
//! meta.json is ~14 KB for a million gaussians (the codebooks dominate).
constexpr size_t   kMaxMetaJsonBytes  = 16u * 1024 * 1024;
//! A gaussian count past this is a corrupt header, not a scene.
constexpr uint32_t kMaxGaussians      = 100u * 1000 * 1000;

float SogSigmoidInv(float y) {
    const float e = std::min(1.0f - 1.0e-6f, std::max(1.0e-6f, y));
    return std::log(e / (1.0f - e));
}

float SogSigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

//! Inverse of splat-transform's logTransform(x) = sign(x) * ln(|x| + 1).
float InvLogTransform(float v) {
    const float e = std::exp(std::fabs(v)) - 1.0f;
    return v < 0.0f ? -e : e;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. PKZip walker
// ─────────────────────────────────────────────────────────────────────────────

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

struct ZipEntry {
    uint16_t method       = 0;  //!< 0 = stored, 8 = deflate; nothing else is accepted
    uint32_t compressed   = 0;
    uint32_t uncompressed = 0;
    uint32_t localOffset  = 0;
};

//! A whole `.sog` held in memory, indexed by member name.
//!
//! SOG bundles are tens of megabytes and every member is read exactly once, so
//! slurping the archive and indexing it beats re-seeking a FILE* per member.
class ZipArchive {
  public:
    bool Open(const std::string& path, std::string& error);
    //! Extract `name` into `out`. Returns false (with `error` set) for a
    //! missing member, an unsupported method, or a size/CRC-shaped mismatch.
    bool Extract(const std::string& name, std::vector<uint8_t>& out,
                 std::string& error) const;
    bool Has(const std::string& name) const { return entries_.count(name) != 0; }

  private:
    std::vector<uint8_t>          bytes_;
    std::map<std::string, ZipEntry> entries_;
};

bool ZipArchive::Open(const std::string& path, std::string& error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open file"; return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); error = "seek failed"; return false; }
    const long len = ftell(f);
    if (len < 22 || (uint64_t)len > kMaxFileBytes) {
        fclose(f);
        error = "file is not a readable archive (bad size)";
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); error = "seek failed"; return false; }
    bytes_.resize((size_t)len);
    const size_t got = fread(bytes_.data(), 1, bytes_.size(), f);
    fclose(f);
    if (got != bytes_.size()) { error = "short read"; return false; }

    // End-of-central-directory: scan backwards over the 64 KB comment window.
    const size_t n = bytes_.size();
    const size_t scanFrom = (n > 22 + 65535) ? (n - 22 - 65535) : 0;
    size_t eocd = SIZE_MAX;
    for (size_t i = n - 22 + 1; i-- > scanFrom;) {
        if (Rd32(&bytes_[i]) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) { error = "not a ZIP container (no end-of-central-directory)"; return false; }

    const uint16_t entryCount = Rd16(&bytes_[eocd + 10]);
    const uint32_t cdSize     = Rd32(&bytes_[eocd + 12]);
    const uint32_t cdOffset   = Rd32(&bytes_[eocd + 16]);
    if (cdOffset == 0xFFFFFFFFu || cdSize == 0xFFFFFFFFu || entryCount == 0xFFFFu) {
        error = "ZIP64 archives are not supported";
        return false;
    }
    if ((uint64_t)cdOffset + cdSize > n) { error = "central directory out of range"; return false; }

    size_t p = cdOffset;
    for (uint16_t i = 0; i < entryCount; i++) {
        if (p + 46 > n || Rd32(&bytes_[p]) != 0x02014b50u) {
            error = "corrupt central-directory entry";
            return false;
        }
        ZipEntry e;
        e.method        = Rd16(&bytes_[p + 10]);
        e.compressed    = Rd32(&bytes_[p + 20]);
        e.uncompressed  = Rd32(&bytes_[p + 24]);
        const uint16_t nameLen    = Rd16(&bytes_[p + 28]);
        const uint16_t extraLen   = Rd16(&bytes_[p + 30]);
        const uint16_t commentLen = Rd16(&bytes_[p + 32]);
        e.localOffset   = Rd32(&bytes_[p + 42]);
        if (p + 46 + nameLen > n) { error = "corrupt central-directory entry"; return false; }
        if (e.compressed == 0xFFFFFFFFu || e.uncompressed == 0xFFFFFFFFu ||
            e.localOffset == 0xFFFFFFFFu) {
            error = "ZIP64 archives are not supported";
            return false;
        }
        std::string name((const char*)&bytes_[p + 46], nameLen);
        // A SOG bundle is flat; a member naming a parent directory is either
        // corrupt or hostile, and we never write extracted members to disk, but
        // refuse it anyway so the failure names itself.
        if (name.find("..") == std::string::npos && !name.empty() &&
            name.back() != '/') {
            entries_.emplace(std::move(name), e);
        }
        p += 46 + nameLen + extraLen + commentLen;
    }
    if (entries_.empty()) { error = "archive holds no members"; return false; }
    return true;
}

bool ZipArchive::Extract(const std::string& name, std::vector<uint8_t>& out,
                         std::string& error) const {
    out.clear();
    auto it = entries_.find(name);
    if (it == entries_.end()) { error = "missing member '" + name + "'"; return false; }
    const ZipEntry& e = it->second;

    const size_t n = bytes_.size();
    if ((uint64_t)e.localOffset + 30 > n || Rd32(&bytes_[e.localOffset]) != 0x04034b50u) {
        error = "corrupt local header for '" + name + "'";
        return false;
    }
    // The local header's name/extra lengths may differ from the central
    // directory's — the data offset MUST come from the local one.
    const uint16_t nameLen  = Rd16(&bytes_[e.localOffset + 26]);
    const uint16_t extraLen = Rd16(&bytes_[e.localOffset + 28]);
    const uint64_t dataOff  = (uint64_t)e.localOffset + 30 + nameLen + extraLen;
    if (dataOff + e.compressed > n) { error = "member '" + name + "' runs past end of file"; return false; }
    if ((uint64_t)e.uncompressed > kMaxInflatedBytes) {
        error = "member '" + name + "' inflates past the 2 GB ceiling";
        return false;
    }

    if (e.method == 0) {
        if (e.compressed != e.uncompressed) {
            error = "stored member '" + name + "' has mismatched sizes";
            return false;
        }
        out.assign(bytes_.begin() + (ptrdiff_t)dataOff,
                   bytes_.begin() + (ptrdiff_t)(dataOff + e.compressed));
        return true;
    }
    if (e.method != 8) {
        error = "member '" + name + "' uses unsupported compression method " +
                std::to_string(e.method);
        return false;
    }

    out.resize(e.uncompressed);
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // Negative windowBits = raw deflate, no zlib/gzip wrapper — what a ZIP
    // member is.
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { error = "inflateInit2 failed"; return false; }
    zs.next_in   = const_cast<Bytef*>(bytes_.data() + dataOff);
    zs.avail_in  = (uInt)e.compressed;
    zs.next_out  = out.empty() ? nullptr : out.data();
    zs.avail_out = (uInt)out.size();
    const int rc = inflate(&zs, Z_FINISH);
    const uLong produced = zs.total_out;
    inflateEnd(&zs);
    if ((rc != Z_STREAM_END && rc != Z_OK) || produced != e.uncompressed) {
        out.clear();
        error = "member '" + name + "' failed to inflate";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Minimal JSON reader
// ─────────────────────────────────────────────────────────────────────────────

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    JsonObject  obj;

    const JsonValue* Find(const char* key) const {
        if (kind != Kind::Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool IsNum() const { return kind == Kind::Number; }
    double Num(double fallback) const { return IsNum() ? num : fallback; }
};

class JsonReader {
  public:
    JsonReader(const char* p, const char* end) : p_(p), end_(end) {}
    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return true;  // trailing bytes are tolerated; meta.json is machine-written
    }
    const std::string& Error() const { return error_; }

  private:
    static constexpr int kMaxDepth = 32;

    void SkipWs() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
    }
    bool Fail(const char* why) { if (error_.empty()) error_ = why; return false; }

    bool ParseValue(JsonValue& v, int depth) {
        if (depth > kMaxDepth) return Fail("JSON nested too deep");
        if (p_ >= end_) return Fail("unexpected end of JSON");
        switch (*p_) {
            case '{': return ParseObject(v, depth);
            case '[': return ParseArray(v, depth);
            case '"': v.kind = JsonValue::Kind::String; return ParseString(v.str);
            case 't':
                if (end_ - p_ < 4 || std::memcmp(p_, "true", 4) != 0) return Fail("bad literal");
                p_ += 4; v.kind = JsonValue::Kind::Bool; v.b = true; return true;
            case 'f':
                if (end_ - p_ < 5 || std::memcmp(p_, "false", 5) != 0) return Fail("bad literal");
                p_ += 5; v.kind = JsonValue::Kind::Bool; v.b = false; return true;
            case 'n':
                if (end_ - p_ < 4 || std::memcmp(p_, "null", 4) != 0) return Fail("bad literal");
                p_ += 4; v.kind = JsonValue::Kind::Null; return true;
            default:  return ParseNumber(v);
        }
    }

    bool ParseNumber(JsonValue& v) {
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) p_++;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' ||
                             *p_ == 'E' || *p_ == '-' || *p_ == '+')) p_++;
        if (p_ == start) return Fail("expected a value");
        // strtod needs NUL-termination; the token is short, so copy it.
        const std::string token(start, (size_t)(p_ - start));
        char* endp = nullptr;
        v.num = strtod(token.c_str(), &endp);
        if (endp == token.c_str()) return Fail("malformed number");
        v.kind = JsonValue::Kind::Number;
        return true;
    }

    bool ParseString(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return Fail("expected a string");
        p_++;
        out.clear();
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= end_) break;
            const char esc = *p_++;
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    // Member names and the strings we read (generator,
                    // convention, file names) are ASCII in practice; decode the
                    // BMP codepoint to UTF-8 and leave surrogate pairs as the
                    // replacement character rather than guessing.
                    if (end_ - p_ < 4) return Fail("truncated \\u escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        const char h = *p_++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return Fail("malformed \\u escape");
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    if (cp < 0x80) {
                        out.push_back((char)cp);
                    } else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseArray(JsonValue& v, int depth) {
        v.kind = JsonValue::Kind::Array;
        p_++;  // '['
        SkipWs();
        if (p_ < end_ && *p_ == ']') { p_++; return true; }
        for (;;) {
            SkipWs();
            v.arr.emplace_back();
            if (!ParseValue(v.arr.back(), depth + 1)) return false;
            SkipWs();
            if (p_ < end_ && *p_ == ',') { p_++; continue; }
            if (p_ < end_ && *p_ == ']') { p_++; return true; }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseObject(JsonValue& v, int depth) {
        v.kind = JsonValue::Kind::Object;
        p_++;  // '{'
        SkipWs();
        if (p_ < end_ && *p_ == '}') { p_++; return true; }
        for (;;) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (p_ >= end_ || *p_ != ':') return Fail("expected ':'");
            p_++;
            SkipWs();
            v.obj.emplace_back(std::move(key), JsonValue{});
            if (!ParseValue(v.obj.back().second, depth + 1)) return false;
            SkipWs();
            if (p_ < end_ && *p_ == ',') { p_++; continue; }
            if (p_ < end_ && *p_ == '}') { p_++; return true; }
            return Fail("expected ',' or '}'");
        }
    }

    const char* p_;
    const char* end_;
    std::string error_;
};

//! Read a float array of exactly `want` entries; false when absent or short.
bool JsonFloatArray(const JsonValue* v, size_t want, std::vector<float>& out) {
    if (!v || v->kind != JsonValue::Kind::Array || v->arr.size() < want) return false;
    out.resize(want);
    for (size_t i = 0; i < want; i++) out[i] = (float)v->arr[i].Num(0.0);
    return true;
}

//! First filename of a `files: [...]` array, or "" when absent.
std::string JsonFirstFile(const JsonValue* plane, size_t index = 0) {
    if (!plane) return std::string();
    const JsonValue* files = plane->Find("files");
    if (!files || files->kind != JsonValue::Kind::Array || files->arr.size() <= index)
        return std::string();
    const JsonValue& f = files->arr[index];
    return f.kind == JsonValue::Kind::String ? f.str : std::string();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. WebP plane
// ─────────────────────────────────────────────────────────────────────────────

//! One decoded RGBA texture plus its dimensions. `rgba` is texel-major in
//! raster order; gaussian `g` lives at `rgba[g * 4 + channel]`.
struct WebpPlane {
    std::vector<uint8_t> rgba;
    int width  = 0;
    int height = 0;
    size_t texels() const { return (size_t)width * (size_t)height; }
};

bool DecodeWebp(const std::vector<uint8_t>& bytes, WebpPlane& out, std::string& error) {
    int w = 0, h = 0;
    if (!WebPGetInfo(bytes.data(), bytes.size(), &w, &h) || w <= 0 || h <= 0) {
        error = "not a decodable WebP image";
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.resize((size_t)w * (size_t)h * 4);
    if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), out.rgba.data(),
                            out.rgba.size(), w * 4)) {
        out.rgba.clear();
        error = "WebP decode failed";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Quaternion unpack (smallest-three), per splat-transform's unpackQuat
// ─────────────────────────────────────────────────────────────────────────────

//! Which output slot each of the three stored components fills, per maxComp.
//! Output order is [w, x, y, z] (index 0 = w), matching the reference decoder.
constexpr int kQuatIdx[12] = {1, 2, 3,  0, 2, 3,  0, 1, 3,  0, 1, 2};

//! Decode one packed quaternion into wxyz[4] = {w, x, y, z}.
void UnpackQuat(uint8_t px, uint8_t py, uint8_t pz, uint8_t tag, float wxyz[4]) {
    const int maxComp = (int)tag - 252;
    const float inv = 1.0f / 1.41421356237309505f;  // 1/sqrt(2)
    const float a = ((float)px / 255.0f * 2.0f - 1.0f) * inv;
    const float b = ((float)py / 255.0f * 2.0f - 1.0f) * inv;
    const float c = ((float)pz / 255.0f * 2.0f - 1.0f) * inv;
    wxyz[0] = wxyz[1] = wxyz[2] = wxyz[3] = 0.0f;
    const int base = maxComp * 3;
    wxyz[kQuatIdx[base + 0]] = a;
    wxyz[kQuatIdx[base + 1]] = b;
    wxyz[kQuatIdx[base + 2]] = c;
    const float t = 1.0f - (a * a + b * b + c * c);
    wxyz[maxComp] = std::sqrt(t > 0.0f ? t : 0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. The optional `camera` block
// ─────────────────────────────────────────────────────────────────────────────

//! Fill `out` from meta.json's optional top-level `camera` object.
//!
//! Absent -> leaves `out.present == false` and returns true: a bundle without
//! the block is the overwhelming majority and is NOT an error. Present but
//! malformed -> also leaves it false, with a WARN, because a half-read camera
//! would frame the scene through a camera that never existed; falling back to
//! the display rig is the safe failure.
bool ReadCameraBlock(const JsonValue& meta, GsSceneCamera& out) {
    const JsonValue* cam = meta.Find("camera");
    if (!cam) return true;
    if (cam->kind != JsonValue::Kind::Object) {
        fprintf(stderr, "ParseSogFile: 'camera' is not an object — ignoring it\n");
        return true;
    }

    const JsonValue* conv = cam->Find("convention");
    const std::string convention =
        (conv && conv->kind == JsonValue::Kind::String) ? conv->str : std::string();
    if (convention != "opencv") {
        fprintf(stderr,
                "ParseSogFile: camera.convention '%s' is not understood "
                "(only 'opencv') — ignoring the camera block\n",
                convention.empty() ? "(missing)" : convention.c_str());
        return true;
    }

    const JsonValue* intr = cam->Find("intrinsics");
    if (!intr || intr->kind != JsonValue::Kind::Object) {
        fprintf(stderr, "ParseSogFile: camera block has no 'intrinsics' — ignoring it\n");
        return true;
    }
    const JsonValue* fx = intr->Find("fx");
    const JsonValue* fy = intr->Find("fy");
    const JsonValue* cx = intr->Find("cx");
    const JsonValue* cy = intr->Find("cy");
    const JsonValue* iw = intr->Find("width");
    const JsonValue* ih = intr->Find("height");
    if (!fx || !fy || !iw || !ih || !fx->IsNum() || !fy->IsNum() || !iw->IsNum() ||
        !ih->IsNum()) {
        fprintf(stderr,
                "ParseSogFile: camera.intrinsics is missing fx/fy/width/height "
                "— ignoring the camera block\n");
        return true;
    }

    GsSceneCamera c;
    c.convention = convention;
    c.fx = (float)fx->num;
    c.fy = (float)fy->num;
    c.width  = (int)iw->num;
    c.height = (int)ih->num;
    // A centred principal point is the norm; an absent cx/cy means exactly that.
    c.cx = (cx && cx->IsNum()) ? (float)cx->num : (float)c.width * 0.5f;
    c.cy = (cy && cy->IsNum()) ? (float)cy->num : (float)c.height * 0.5f;
    if (!(c.fx > 0.0f) || !(c.fy > 0.0f) || c.width <= 0 || c.height <= 0) {
        fprintf(stderr,
                "ParseSogFile: camera.intrinsics are degenerate "
                "(fx=%.3f fy=%.3f %dx%d) — ignoring the camera block\n",
                (double)c.fx, (double)c.fy, c.width, c.height);
        return true;
    }

    if (const JsonValue* rest = cam->Find("rest")) {
        std::vector<float> v;
        if (JsonFloatArray(rest->Find("position"), 3, v))
            for (int i = 0; i < 3; i++) c.restPosition[i] = v[i];
        if (JsonFloatArray(rest->Find("rotation"), 4, v)) {
            // xyzw, camera->world.
            const float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
            if (len > 1.0e-8f)
                for (int i = 0; i < 4; i++) c.restRotation[i] = v[i] / len;
        }
    }

    if (const JsonValue* stereo = cam->Find("stereo")) {
        const JsonValue* b = stereo->Find("baseline_m");
        if (b && b->IsNum() && b->num > 0.0) {
            c.hasStereo = true;
            c.baselineM = (float)b->num;
        }
    }

    // ── v2 fields. Every one is optional and independently ignorable, so a
    //    v1 block (or a v2 block from a producer that fills only some of it)
    //    reaches exactly the same state it did before. ────────────────────
    if (const JsonValue* r = cam->Find("rig")) {
        if (r->kind == JsonValue::Kind::String) {
            if (r->str == "camera") { c.hasRigHint = true; c.rigHint = GsRigKind::Camera; }
            else if (r->str == "display") { c.hasRigHint = true; c.rigHint = GsRigKind::Display; }
            else fprintf(stderr, "ParseSogFile: camera.rig '%s' is not 'camera' or "
                                 "'display' — ignoring the hint\n", r->str.c_str());
        }
    }

    if (const JsonValue* f = cam->Find("focus")) {
        std::vector<float> v;
        if (JsonFloatArray(f->Find("point"), 3, v)) {
            // A focus behind the camera, or on it, cannot be a convergence
            // plane; refuse it rather than divide by it downstream.
            if (v[2] > 1.0e-3f && std::isfinite(v[0]) && std::isfinite(v[1])) {
                c.hasFocus = true;
                for (int i = 0; i < 3; i++) c.focusPoint[i] = v[i];
            } else {
                fprintf(stderr, "ParseSogFile: camera.focus.point is not in front of "
                                "the camera (z=%.4f) — ignoring it\n", (double)v[2]);
            }
        }
        if (const JsonValue* s = f->Find("source"))
            if (s->kind == JsonValue::Kind::String) c.focusSourceLabel = s->str;
        const JsonValue* sm = f->Find("subject_m");
        const JsonValue* nm = f->Find("near_m");
        const JsonValue* fm = f->Find("far_m");
        if (sm && sm->IsNum() && sm->num > 0.0) { c.hasSubjectM = true; c.subjectM = (float)sm->num; }
        if (nm && nm->IsNum() && nm->num > 0.0) { c.hasNearM = true;    c.nearM    = (float)nm->num; }
        if (fm && fm->IsNum() && fm->num > 0.0) { c.hasFarM = true;     c.farM     = (float)fm->num; }
    }

    if (const JsonValue* d = cam->Find("dxr")) {
        const JsonValue* ip = d->Find("ipd_factor");
        const JsonValue* pf = d->Find("parallax_factor");
        // Absolute scalars, so the only bound is sanity: a negative or absurd
        // one would invert or explode the stereo rather than tune it.
        if (ip && ip->IsNum() && ip->num >= 0.0 && ip->num <= 10.0) {
            c.hasDxrIpd = true; c.dxrIpdFactor = (float)ip->num;
        }
        if (pf && pf->IsNum() && pf->num >= 0.0 && pf->num <= 10.0) {
            c.hasDxrParallax = true; c.dxrParallaxFactor = (float)pf->num;
        }
    }

    c.present = true;
    out = c;
    printf("ParseSogFile: camera block: convention=%s fx=%.3f fy=%.3f cx=%.1f cy=%.1f "
           "%dx%d baseline=%s\n",
           c.convention.c_str(), (double)c.fx, (double)c.fy, (double)c.cx, (double)c.cy,
           c.width, c.height,
           c.hasStereo ? (std::to_string(c.baselineM) + " m").c_str() : "(none)");
    if (c.hasRigHint || c.hasFocus || c.hasDxrIpd || c.hasDxrParallax) {
        printf("ParseSogFile: camera v2: rig=%s focus=%s%s dxr.ipd=%s dxr.parallax=%s\n",
               c.hasRigHint ? (c.rigHint == GsRigKind::Camera ? "camera" : "display") : "(unset)",
               c.hasFocus ? "" : "(unset)",
               c.hasFocus ? (std::string("(") + std::to_string(c.focusPoint[0]) + ", " +
                             std::to_string(c.focusPoint[1]) + ", " +
                             std::to_string(c.focusPoint[2]) + ")" +
                             (c.focusSourceLabel.empty() ? "" : " src=" + c.focusSourceLabel)).c_str()
                          : "",
               c.hasDxrIpd ? std::to_string(c.dxrIpdFactor).c_str() : "(unset)",
               c.hasDxrParallax ? std::to_string(c.dxrParallaxFactor).c_str() : "(unset)");
    }
    fflush(stdout);
    return true;
}

//! SH coefficients per channel, indexed by band count. Mirrors the reference
//! decoder's SH_COEFFS table.
constexpr int kShCoeffs[4] = {0, 3, 8, 15};

//! Which coordinate system the on-disk bytes are assumed to be in. See the
//! header comment; DXR_SOG_COORD_SYSTEM=rub|rdf overrides.
spz::CoordinateSystem SogSourceSystem() {
    if (const char* env = std::getenv("DXR_SOG_COORD_SYSTEM")) {
        if (std::strcmp(env, "rub") == 0 || std::strcmp(env, "RUB") == 0)
            return spz::CoordinateSystem::RUB;
        if (std::strcmp(env, "rdf") == 0 || std::strcmp(env, "RDF") == 0)
            return spz::CoordinateSystem::RDF;
        fprintf(stderr, "ParseSogFile: ignoring unrecognised DXR_SOG_COORD_SYSTEM='%s'\n", env);
    }
    return spz::CoordinateSystem::RDF;
}

}  // namespace

bool ParseSogFile(const std::string& path,
                  std::vector<GsVertex>& vertices,
                  SogFileInfo* info,
                  GsSceneCamera* camera) {
    vertices.clear();
    SogFileInfo local;
    SogFileInfo& fi = info ? *info : local;
    fi = SogFileInfo();

    ZipArchive zip;
    if (!zip.Open(path, fi.error)) {
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    std::vector<uint8_t> metaBytes;
    if (!zip.Extract("meta.json", metaBytes, fi.error)) {
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }
    if (metaBytes.size() > kMaxMetaJsonBytes) {
        fi.error = "meta.json is implausibly large";
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    JsonValue meta;
    {
        JsonReader reader((const char*)metaBytes.data(),
                          (const char*)metaBytes.data() + metaBytes.size());
        if (!reader.Parse(meta) || meta.kind != JsonValue::Kind::Object) {
            fi.error = "meta.json did not parse (" +
                       (reader.Error().empty() ? std::string("not an object") : reader.Error()) + ")";
            fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
            return false;
        }
    }

    if (const JsonValue* v = meta.Find("version")) fi.version = (int)v->Num(0.0);
    if (const JsonValue* a = meta.Find("asset")) {
        if (const JsonValue* g = a->Find("generator"))
            if (g->kind == JsonValue::Kind::String) fi.generator = g->str;
    }
    if (fi.version != 2) {
        // V1 SOG (per-plane mins/maxs, no codebooks) is a different decode and
        // is not in the wild for DisplayXR content — name it rather than
        // half-reading it.
        fi.error = "unsupported SOG version " + std::to_string(fi.version) +
                   " (this build reads version 2)";
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    const JsonValue* countV = meta.Find("count");
    const double countD = countV ? countV->Num(-1.0) : -1.0;
    if (!(countD >= 1.0) || countD > (double)kMaxGaussians) {
        fi.error = "meta.json 'count' is missing or out of range";
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }
    const uint32_t count = (uint32_t)countD;
    fi.numPoints = (int)count;

    // The camera block is read before the (expensive) texel decode so a
    // malformed one is reported next to the header, not after a 10 s load.
    if (camera) {
        GsSceneCamera parsed;
        ReadCameraBlock(meta, parsed);
        *camera = parsed;
    }

    // ── Plane descriptors ────────────────────────────────────────────────────
    const JsonValue* meansV  = meta.Find("means");
    const JsonValue* scalesV = meta.Find("scales");
    const JsonValue* quatsV  = meta.Find("quats");
    const JsonValue* sh0V    = meta.Find("sh0");
    if (!meansV || !scalesV || !quatsV || !sh0V) {
        fi.error = "meta.json is missing one of means/scales/quats/sh0";
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    std::vector<float> meansMins, meansMaxs;
    if (!JsonFloatArray(meansV->Find("mins"), 3, meansMins) ||
        !JsonFloatArray(meansV->Find("maxs"), 3, meansMaxs)) {
        fi.error = "meta.means is missing mins/maxs";
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    std::vector<float> scaleCodebook, sh0Codebook;
    if (!JsonFloatArray(scalesV->Find("codebook"), 256, scaleCodebook) ||
        !JsonFloatArray(sh0V->Find("codebook"), 256, sh0Codebook)) {
        fi.error = "meta.scales / meta.sh0 is missing its 256-entry codebook";
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    // ── Decode the planes ────────────────────────────────────────────────────
    auto loadPlane = [&](const std::string& name, WebpPlane& plane) -> bool {
        if (name.empty()) { fi.error = "meta.json names no file for a required plane"; return false; }
        std::vector<uint8_t> bytes;
        if (!zip.Extract(name, bytes, fi.error)) return false;
        std::string werr;
        if (!DecodeWebp(bytes, plane, werr)) {
            fi.error = "'" + name + "': " + werr;
            return false;
        }
        if (plane.texels() < (size_t)count) {
            fi.error = "'" + name + "' holds " + std::to_string(plane.texels()) +
                       " texels, short of the declared " + std::to_string(count);
            return false;
        }
        return true;
    };

    WebpPlane meansLo, meansHi, quats, scales, sh0;
    if (!loadPlane(JsonFirstFile(meansV, 0), meansLo) ||
        !loadPlane(JsonFirstFile(meansV, 1), meansHi) ||
        !loadPlane(JsonFirstFile(quatsV), quats) ||
        !loadPlane(JsonFirstFile(scalesV), scales) ||
        !loadPlane(JsonFirstFile(sh0V), sh0)) {
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    // ── Optional shN palette ─────────────────────────────────────────────────
    int shBands = 0, shCoeffs = 0, paletteCount = 0;
    std::vector<float> shCodebook;
    WebpPlane shCentroids, shLabels;
    if (const JsonValue* shNV = meta.Find("shN")) {
        const JsonValue* bandsV = shNV->Find("bands");
        const int bands = bandsV ? (int)bandsV->Num(0.0) : 0;
        if (bands >= 1 && bands <= 3) {
            shBands  = bands;
            shCoeffs = kShCoeffs[bands];
            const JsonValue* pc = shNV->Find("count");
            paletteCount = pc ? (int)pc->Num(0.0) : 0;
            const std::string cenName = JsonFirstFile(shNV, 0);
            const std::string labName = JsonFirstFile(shNV, 1);
            std::vector<uint8_t> bytes;
            std::string werr;
            const bool ok =
                JsonFloatArray(shNV->Find("codebook"), 256, shCodebook) &&
                zip.Extract(cenName, bytes, werr) && DecodeWebp(bytes, shCentroids, werr) &&
                zip.Extract(labName, bytes, werr) && DecodeWebp(bytes, shLabels, werr) &&
                shLabels.texels() >= (size_t)count &&
                shCentroids.width == 64 * shCoeffs;
            if (!ok) {
                // Dropping the higher bands costs view-dependent shading, not the
                // scene — far better than refusing the file. Say so loudly.
                fprintf(stderr,
                        "ParseSogFile: %s — shN palette unreadable (%s); "
                        "loading DC-only\n",
                        path.c_str(), werr.empty() ? "layout mismatch" : werr.c_str());
                shBands = 0;
                shCoeffs = 0;
                paletteCount = 0;
            }
        } else if (bands != 0) {
            fprintf(stderr,
                    "ParseSogFile: %s — shN declares %d bands (1-3 supported); "
                    "loading DC-only\n", path.c_str(), bands);
        }
    }
    fi.shBands = shBands;

    // ── Stage into a spz::GaussianCloud so the coordinate change is shared ───
    spz::GaussianCloud cloud;
    const size_t n = (size_t)count;
    try {
        cloud.numPoints = (int32_t)count;
        cloud.shDegree  = shBands;
        cloud.positions.resize(n * 3);
        cloud.scales.resize(n * 3);
        cloud.rotations.resize(n * 4);
        cloud.alphas.resize(n);
        cloud.colors.resize(n * 3);
        if (shBands > 0) cloud.sh.resize(n * (size_t)shCoeffs * 3);
    } catch (const std::exception& e) {
        fi.error = std::string("out of memory staging ") + std::to_string(count) +
                   " gaussians: " + e.what();
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    const float xMin = meansMins[0], xScale = (meansMaxs[0] - meansMins[0]) != 0.0f
                                                  ? (meansMaxs[0] - meansMins[0]) : 1.0f;
    const float yMin = meansMins[1], yScale = (meansMaxs[1] - meansMins[1]) != 0.0f
                                                  ? (meansMaxs[1] - meansMins[1]) : 1.0f;
    const float zMin = meansMins[2], zScale = (meansMaxs[2] - meansMins[2]) != 0.0f
                                                  ? (meansMaxs[2] - meansMins[2]) : 1.0f;

    const uint8_t* lo = meansLo.rgba.data();
    const uint8_t* hi = meansHi.rgba.data();
    const uint8_t* qr = quats.rgba.data();
    const uint8_t* sl = scales.rgba.data();
    const uint8_t* c0 = sh0.rgba.data();
    const uint8_t* lab = shBands > 0 ? shLabels.rgba.data() : nullptr;
    const uint8_t* cen = shBands > 0 ? shCentroids.rgba.data() : nullptr;
    const int cW = shCentroids.width, cH = shCentroids.height;

    for (uint32_t g = 0; g < count; g++) {
        const size_t o4 = (size_t)g * 4;

        const uint32_t xv = (uint32_t)lo[o4 + 0] | ((uint32_t)hi[o4 + 0] << 8);
        const uint32_t yv = (uint32_t)lo[o4 + 1] | ((uint32_t)hi[o4 + 1] << 8);
        const uint32_t zv = (uint32_t)lo[o4 + 2] | ((uint32_t)hi[o4 + 2] << 8);
        cloud.positions[(size_t)g * 3 + 0] = InvLogTransform(xMin + xScale * ((float)xv / 65535.0f));
        cloud.positions[(size_t)g * 3 + 1] = InvLogTransform(yMin + yScale * ((float)yv / 65535.0f));
        cloud.positions[(size_t)g * 3 + 2] = InvLogTransform(zMin + zScale * ((float)zv / 65535.0f));

        // Quaternion: SOG packs smallest-three and tags the dropped component
        // in alpha. An out-of-range tag is an unset texel (the encoder pads the
        // texture past `count`), so fall back to identity rather than indexing
        // the table out of bounds.
        const uint8_t tag = qr[o4 + 3];
        float wxyz[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        if (tag >= 252 && tag <= 255) UnpackQuat(qr[o4 + 0], qr[o4 + 1], qr[o4 + 2], tag, wxyz);
        // spz stores xyzw.
        cloud.rotations[(size_t)g * 4 + 0] = wxyz[1];
        cloud.rotations[(size_t)g * 4 + 1] = wxyz[2];
        cloud.rotations[(size_t)g * 4 + 2] = wxyz[3];
        cloud.rotations[(size_t)g * 4 + 3] = wxyz[0];

        cloud.scales[(size_t)g * 3 + 0] = scaleCodebook[sl[o4 + 0]];
        cloud.scales[(size_t)g * 3 + 1] = scaleCodebook[sl[o4 + 1]];
        cloud.scales[(size_t)g * 3 + 2] = scaleCodebook[sl[o4 + 2]];

        // SOG stores the ALREADY-sigmoided opacity; spz wants the logit, and
        // the vertex loop below re-applies sigmoid. The round trip is clamped
        // at 1e-6 and is what the reference decoder does on its way to PLY.
        cloud.alphas[g] = SogSigmoidInv((float)c0[o4 + 3] / 255.0f);

        cloud.colors[(size_t)g * 3 + 0] = sh0Codebook[c0[o4 + 0]];
        cloud.colors[(size_t)g * 3 + 1] = sh0Codebook[c0[o4 + 1]];
        cloud.colors[(size_t)g * 3 + 2] = sh0Codebook[c0[o4 + 2]];

        if (shBands > 0) {
            const size_t shBase = (size_t)g * (size_t)shCoeffs * 3;
            const int label = (int)lab[o4 + 0] | ((int)lab[o4 + 1] << 8);
            if (label >= 0 && label < paletteCount) {
                const int cy = label / 64;
                const int cxBase = (label % 64) * shCoeffs;
                for (int j = 0; j < shCoeffs; j++) {
                    const int cx = cxBase + j;
                    const bool ok = cy < cH && cx < cW;
                    const size_t idx = ok ? ((size_t)cy * (size_t)cW + (size_t)cx) * 4 : 0;
                    const uint8_t lr = ok ? cen[idx + 0] : 0;
                    const uint8_t lg = ok ? cen[idx + 1] : 0;
                    const uint8_t lb = ok ? cen[idx + 2] : 0;
                    // spz's layout is coefficient-outer / channel-inner; the
                    // SOG palette is channel-major per coefficient.
                    cloud.sh[shBase + (size_t)j * 3 + 0] = shCodebook[lr];
                    cloud.sh[shBase + (size_t)j * 3 + 1] = shCodebook[lg];
                    cloud.sh[shBase + (size_t)j * 3 + 2] = shCodebook[lb];
                }
            } else {
                std::memset(&cloud.sh[shBase], 0, (size_t)shCoeffs * 3 * sizeof(float));
            }
        }
    }

    // Right the cloud. spz's converter is the one implementation that turns
    // positions, quaternions AND the SH bands through a coordinate change
    // together — see the header comment for why this must not be hand-rolled.
    const spz::CoordinateSystem sourceSystem = SogSourceSystem();
    fi.sourceSystem = (sourceSystem == spz::CoordinateSystem::RDF) ? "RDF" : "RUB";
    if (sourceSystem != spz::CoordinateSystem::RUB) {
        try {
            cloud.convertCoordinates(sourceSystem, spz::CoordinateSystem::RUB);
        } catch (const std::exception& e) {
            fi.error = std::string("coordinate conversion threw: ") + e.what();
            fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
            return false;
        }
    }

    printf("ParseSogFile: loading %u gaussians from %s (sogVersion=%d, generator=%s, "
           "shBands=%d, source coords=%s -> RUB)\n",
           count, path.c_str(), fi.version,
           fi.generator.empty() ? "(none)" : fi.generator.c_str(), shBands,
           fi.sourceSystem.c_str());
    fflush(stdout);

    // ── Emit GPU vertices. Identical to ParseSpzFile's loop by construction. ─
    try {
        vertices.resize(count);
    } catch (const std::exception& e) {
        fi.error = std::string("out of memory for ") + std::to_string(count) +
                   " vertices: " + e.what();
        fprintf(stderr, "ParseSogFile: %s — %s\n", path.c_str(), fi.error.c_str());
        return false;
    }

    const int shFloatsPerPoint = shBands > 0 ? shCoeffs * 3 : 0;
    for (uint32_t i = 0; i < count; i++) {
        GsVertex& v = vertices[i];

        v.position[0] = cloud.positions[(size_t)i * 3 + 0];
        v.position[1] = cloud.positions[(size_t)i * 3 + 1];
        v.position[2] = cloud.positions[(size_t)i * 3 + 2];
        v.position[3] = 1.0f;

        v.scale_opacity[0] = std::exp(cloud.scales[(size_t)i * 3 + 0]);
        v.scale_opacity[1] = std::exp(cloud.scales[(size_t)i * 3 + 1]);
        v.scale_opacity[2] = std::exp(cloud.scales[(size_t)i * 3 + 2]);
        v.scale_opacity[3] = SogSigmoid(cloud.alphas[i]);

        float qx = cloud.rotations[(size_t)i * 4 + 0];
        float qy = cloud.rotations[(size_t)i * 4 + 1];
        float qz = cloud.rotations[(size_t)i * 4 + 2];
        float qw = cloud.rotations[(size_t)i * 4 + 3];
        const float qlen = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
        if (qlen > 1.0e-8f) {
            const float inv = 1.0f / qlen;
            qw *= inv; qx *= inv; qy *= inv; qz *= inv;
        } else {
            qw = 1.0f; qx = qy = qz = 0.0f;
        }
        v.rotation[0] = qw;
        v.rotation[1] = qx;
        v.rotation[2] = qy;
        v.rotation[3] = qz;

        v.sh[0] = cloud.colors[(size_t)i * 3 + 0];
        v.sh[1] = cloud.colors[(size_t)i * 3 + 1];
        v.sh[2] = cloud.colors[(size_t)i * 3 + 2];

        if (shFloatsPerPoint > 0) {
            const int floatsToCopy = std::min(shFloatsPerPoint, 45);
            std::memcpy(&v.sh[3], &cloud.sh[(size_t)i * (size_t)shFloatsPerPoint],
                        (size_t)floatsToCopy * sizeof(float));
            for (int j = 3 + floatsToCopy; j < 48; j++) v.sh[j] = 0.0f;
        } else {
            std::memset(&v.sh[3], 0, 45 * sizeof(float));
        }
    }

    printf("ParseSogFile: loaded %u gaussians (%.1f MB GPU data)\n",
           count, (double)((size_t)count * sizeof(GsVertex)) / (1024.0 * 1024.0));
    fflush(stdout);
    return true;
}

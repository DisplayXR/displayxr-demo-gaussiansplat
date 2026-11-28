// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the PLY loader (ParsePlyFile): synthetic tiny PLYs in
 *         the layouts the loader must accept or refuse, written to a temp dir
 *         at run time. No GPU, no Vulkan calls. Exit code 0 = all passed.
 */

#include "gs_scene_loader.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

bool Near(float a, float b)
{
    const float scale = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-5f * scale;
}

//! One property in the synthetic header: its PLY type and name. Values are
//! generated from (vertex index, property name) so the test can predict them.
struct Prop {
    std::string type;
    std::string name;
};

//! Small per-vertex offset that keeps exp()/sigmoid() finite for any count.
float VDelta(int v) { return 0.01f * (float)(v % 97); }

//! Deterministic value for (vertex, property). Quaternion and scale values are
//! chosen to keep exp/normalize well-defined; f_rest values encode their index.
float ValueFor(int v, const std::string& name)
{
    static const std::map<std::string, float> base = {
        {"x", 1.0f},       {"y", 2.0f},       {"z", 3.0f},       {"f_dc_0", 0.1f},
        {"f_dc_1", 0.2f},  {"f_dc_2", 0.3f},  {"opacity", 0.0f}, {"scale_0", -1.0f},
        {"scale_1", -2.0f}, {"scale_2", -3.0f}, {"rot_0", 1.0f},  {"rot_1", 0.0f},
        {"rot_2", 0.0f},   {"rot_3", 0.0f},   {"nx", 9.0f},      {"ny", 9.0f},
        {"nz", 9.0f}};
    auto it = base.find(name);
    if (it != base.end()) return it->second + (name.rfind("rot_", 0) == 0 ? 0.0f : VDelta(v));
    if (name.rfind("f_rest_", 0) == 0) return 100.0f + std::stof(name.substr(7)) + VDelta(v);
    return 777.0f;  // unknown property: must never be read
}

std::vector<Prop> FloatProps(const std::vector<std::string>& names)
{
    std::vector<Prop> out;
    for (const auto& n : names) out.push_back({"float", n});
    return out;
}

std::vector<std::string> Degree0Names()
{
    return {"x",       "y",       "z",       "f_dc_0",  "f_dc_1", "f_dc_2",  "opacity",
            "scale_0", "scale_1", "scale_2", "rot_0",   "rot_1",  "rot_2",   "rot_3"};
}

std::vector<std::string> StandardNames(int restCount, bool normals)
{
    std::vector<std::string> n = {"x", "y", "z"};
    if (normals) {
        n.push_back("nx");
        n.push_back("ny");
        n.push_back("nz");
    }
    n.push_back("f_dc_0");
    n.push_back("f_dc_1");
    n.push_back("f_dc_2");
    for (int i = 0; i < restCount; i++) n.push_back("f_rest_" + std::to_string(i));
    for (const char* s : {"opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2",
                          "rot_3"})
        n.push_back(s);
    return n;
}

//! Output encoding of a synthetic file.
enum class Fmt { LE, BE, Ascii };

const char* FmtName(Fmt fmt)
{
    return fmt == Fmt::LE ? "binary_little_endian" : fmt == Fmt::BE ? "binary_big_endian" : "ascii";
}

template <typename T>
void PutRaw(std::ofstream& f, Fmt fmt, T v)
{
    unsigned char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    if (fmt == Fmt::BE) std::reverse(b, b + sizeof(T));
    f.write(reinterpret_cast<const char*>(b), sizeof(T));
}

//! Integer types store the value truncated, or 7 when it does not fit
//! (unknown properties are 777 and are never read).
template <typename T>
T ToInt(float value)
{
    const double t = std::trunc((double)value);
    if (t < (double)(std::numeric_limits<T>::min)() || t > (double)(std::numeric_limits<T>::max)())
        return (T)7;
    return (T)t;
}

void Put(std::ofstream& f, Fmt fmt, const std::string& type, float value)
{
    if (fmt == Fmt::Ascii) {
        char buf[64];
        if (type == "float" || type == "double")
            std::snprintf(buf, sizeof(buf), "%.9g ", (double)value);
        else if (type == "uchar")
            std::snprintf(buf, sizeof(buf), "%d ", (int)ToInt<uint8_t>(value));
        else if (type == "short")
            std::snprintf(buf, sizeof(buf), "%d ", (int)ToInt<int16_t>(value));
        else if (type == "int")
            std::snprintf(buf, sizeof(buf), "%d ", (int)ToInt<int32_t>(value));
        else {
            std::fprintf(stderr, "test bug: type %s\n", type.c_str());
            std::abort();
        }
        f << buf;
        return;
    }
    if (type == "float") PutRaw(f, fmt, value);
    else if (type == "double") PutRaw(f, fmt, (double)value);
    else if (type == "uchar") PutRaw(f, fmt, ToInt<uint8_t>(value));
    else if (type == "short") PutRaw(f, fmt, ToInt<int16_t>(value));
    else if (type == "int") PutRaw(f, fmt, ToInt<int32_t>(value));
    else {
        std::fprintf(stderr, "test bug: type %s\n", type.c_str());
        std::abort();
    }
}

struct WriteOpts {
    Fmt fmt = Fmt::LE;
    //! Two elements BEFORE the vertex element (one all-scalar, one with a
    //! list property), and a face element after it.
    bool otherElements = false;
};

std::string WritePly(const std::filesystem::path& dir, const std::string& name,
                     const std::vector<Prop>& props, int count, WriteOpts o = {})
{
    const std::filesystem::path p = dir / (name + ".ply");
    std::ofstream f(p, std::ios::binary);
    f << "ply\nformat " << FmtName(o.fmt) << " 1.0\ncomment synthetic test\n";
    if (o.otherElements) {
        f << "element camera 2\nproperty float fx\nproperty double fy\nproperty uchar id\n";
        f << "element tag 3\nproperty list uchar int ids\nproperty short weight\n";
    }
    f << "element vertex " << count << "\n";
    for (const auto& pr : props) f << "property " << pr.type << " " << pr.name << "\n";
    if (o.otherElements) f << "element face 1\nproperty list uchar int vertex_indices\n";
    f << "end_header\n";
    auto nl = [&] {
        if (o.fmt == Fmt::Ascii) f << "\n";
    };
    if (o.otherElements) {
        for (int c = 0; c < 2; c++) {
            Put(f, o.fmt, "float", 500.0f);
            Put(f, o.fmt, "double", 600.0f);
            Put(f, o.fmt, "uchar", 9.0f);
            nl();
        }
        for (int t = 0; t < 3; t++) {
            // A list of t+1 ints, then a short.
            if (o.fmt == Fmt::Ascii) f << (t + 1) << " ";
            else PutRaw(f, o.fmt, (uint8_t)(t + 1));
            for (int i = 0; i <= t; i++) Put(f, o.fmt, "int", 40.0f + (float)i);
            Put(f, o.fmt, "short", -5.0f);
            nl();
        }
    }
    for (int v = 0; v < count; v++) {
        for (const auto& pr : props) Put(f, o.fmt, pr.type, ValueFor(v, pr.name));
        nl();
    }
    if (o.otherElements) {
        if (o.fmt == Fmt::Ascii) f << "3 ";
        else PutRaw(f, o.fmt, (uint8_t)3);
        for (int i = 0; i < 3; i++) Put(f, o.fmt, "int", (float)i);
        nl();
    }
    return p.string();
}

//! Every property typed `type`.
std::vector<Prop> TypedProps(const std::vector<std::string>& names, const std::string& type)
{
    std::vector<Prop> out;
    for (const auto& n : names) out.push_back({type, n});
    return out;
}

//! Checks the fields every accepted layout must decode identically, and the
//! SH rest block for `restCount` (0 = all zero).
void CheckVertices(const std::vector<GsVertex>& verts, int count, int restCount,
                   bool intPos = false)
{
    CHECK((int)verts.size() == count);
    if ((int)verts.size() != count) return;
    const int k = restCount / 3;  // coefficients per channel in the file
    for (int v = 0; v < count; v++) {
        const GsVertex& g = verts[v];
        // RDF -> RUB: all three axes negated.
        // Integer-typed positions hold the truncated value (1, 2, 3).
        const float d = intPos ? 0.0f : VDelta(v);
        CHECK(Near(g.position[0], -(1.0f + d)));
        CHECK(Near(g.position[1], -(2.0f + d)));
        CHECK(Near(g.position[2], -(3.0f + d)));
        CHECK(Near(g.position[3], 1.0f));
        CHECK(Near(g.scale_opacity[0], std::exp(-1.0f + VDelta(v))));
        CHECK(Near(g.scale_opacity[2], std::exp(-3.0f + VDelta(v))));
        CHECK(Near(g.scale_opacity[3], 1.0f / (1.0f + std::exp(-(VDelta(v))))));
        CHECK(Near(g.rotation[0], 1.0f));
        CHECK(Near(g.sh[0], 0.1f + VDelta(v)));
        CHECK(Near(g.sh[1], 0.2f + VDelta(v)));
        CHECK(Near(g.sh[2], 0.3f + VDelta(v)));
        for (int j = 1; j < 16; j++) {
            for (int c = 0; c < 3; c++) {
                const float got = g.sh[j * 3 + c];
                float want = 0.0f;
                if (j - 1 < k) {
                    // File index of channel c, coefficient j: channel-major.
                    const int fileIdx = c * k + (j - 1);
                    want = 100.0f + fileIdx + VDelta(v);
                }
                if (!Near(got, want)) {
                    std::fprintf(stderr, "  sh mismatch v=%d j=%d c=%d got=%f want=%f\n", v, j, c,
                                 got, want);
                    g_failures++;
                }
            }
        }
    }
}

void ExpectOk(const char* label, const std::string& path, int count, int restCount,
              bool intPos = false)
{
    std::printf("[case] %s\n", label);
    std::vector<GsVertex> verts;
    std::string err;
    const bool ok = ParsePlyFile(path, verts, &err);
    if (!ok) std::fprintf(stderr, "  unexpected error: %s\n", err.c_str());
    CHECK(ok);
    if (ok) CheckVertices(verts, count, restCount, intPos);
}

void ExpectError(const char* label, const std::string& path, const std::string& needle)
{
    std::printf("[case] %s\n", label);
    std::vector<GsVertex> verts;
    std::string err;
    const bool ok = ParsePlyFile(path, verts, &err);
    CHECK(!ok);
    CHECK(verts.empty());
    if (err.find(needle) == std::string::npos) {
        std::fprintf(stderr, "  error '%s' does not mention '%s'\n", err.c_str(), needle.c_str());
        g_failures++;
    }
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "gs_ply_loader_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const int n = 3;

    // 14 floats, no normals, no f_rest: the conversion provider's layout.
    ExpectOk("degree-0 14-float (conversion provider)",
             WritePly(dir, "deg0", FloatProps(Degree0Names()), n), n, 0);

    // The standard 62-float export (bulk fast path).
    ExpectOk("standard 62-float", WritePly(dir, "std62", FloatProps(StandardNames(45, true)), n), n,
             45);

    // Degree 1: 3 rest coefficients per channel = 9 f_rest.
    ExpectOk("degree-1 (9 f_rest, normals)",
             WritePly(dir, "deg1", FloatProps(StandardNames(9, true)), n), n, 9);

    // Degree 2: 8 per channel = 24 f_rest, no normals.
    ExpectOk("degree-2 (24 f_rest, no normals)",
             WritePly(dir, "deg2", FloatProps(StandardNames(24, false)), n), n, 24);

    // Degree 3 without normals (61 floats, not the fast path).
    ExpectOk("degree-3 without normals",
             WritePly(dir, "deg3nn", FloatProps(StandardNames(45, false)), n), n, 45);

    // Unknown extra properties of several types, interleaved: must be skipped
    // with the correct stride.
    {
        std::vector<Prop> props = FloatProps(Degree0Names());
        props.insert(props.begin() + 3, {"float", "confidence"});
        props.insert(props.begin() + 8, {"uchar", "red"});
        props.insert(props.begin() + 9, {"double", "timestamp"});
        props.push_back({"int", "segment_id"});
        ExpectOk("degree-0 + unknown extra properties", WritePly(dir, "extra", props, n), n, 0);
    }

    // Property order shuffled (rot before position, etc.).
    {
        std::vector<std::string> names = Degree0Names();
        std::vector<std::string> shuffled(names.rbegin(), names.rend());
        ExpectOk("degree-0 reversed property order",
                 WritePly(dir, "reversed", FloatProps(shuffled), n), n, 0);
    }

    // Large enough to cross the 65536-record read chunk.
    ExpectOk("degree-0 70000 records (chunked read)",
             WritePly(dir, "deg0big", FloatProps(Degree0Names()), 70000), 70000, 0);

    // ── Scalar types, byte order, ASCII, other elements ──
    ExpectOk("degree-3, every property double",
             WritePly(dir, "deg3dbl", TypedProps(StandardNames(45, true), "double"), n), n, 45);
    {
        std::vector<Prop> props = FloatProps(Degree0Names());
        props[0].type = "int";
        props[1].type = "short";
        props[2].type = "uchar";
        ExpectOk("degree-0, integer-typed x/y/z", WritePly(dir, "intpos", props, n), n, 0, true);
    }
    {
        // Degree 3, every property reversed (f_rest_44 first, x last).
        std::vector<std::string> names = StandardNames(45, false);
        std::vector<std::string> shuffled(names.rbegin(), names.rend());
        ExpectOk("degree-3 reversed property order",
                 WritePly(dir, "deg3rev", FloatProps(shuffled), n), n, 45);
    }
    {
        WriteOpts o;
        o.otherElements = true;
        ExpectOk("degree-0, elements before and after vertex",
                 WritePly(dir, "lead0", FloatProps(Degree0Names()), n, o), n, 0);
        ExpectOk("standard 62-float, elements before and after vertex (fast path)",
                 WritePly(dir, "lead62", FloatProps(StandardNames(45, true)), n, o), n, 45);
    }
    {
        WriteOpts o;
        o.fmt = Fmt::BE;
        ExpectOk("big-endian standard 62-float",
                 WritePly(dir, "be62", FloatProps(StandardNames(45, true)), n, o), n, 45);
        std::vector<Prop> props = FloatProps(Degree0Names());
        props.insert(props.begin() + 3, {"double", "timestamp"});
        props.push_back({"short", "segment_id"});
        props[0].type = "double";
        o.otherElements = true;
        ExpectOk("big-endian degree-0, extras, double x, other elements",
                 WritePly(dir, "be0", props, n, o), n, 0);
    }
    {
        WriteOpts o;
        o.fmt = Fmt::Ascii;
        ExpectOk("ascii degree-0", WritePly(dir, "asc0", FloatProps(Degree0Names()), n, o), n, 0);
        ExpectOk("ascii standard 62-float",
                 WritePly(dir, "asc62", FloatProps(StandardNames(45, true)), n, o), n, 45);
        std::vector<std::string> names = StandardNames(24, false);
        std::vector<Prop> props = FloatProps(std::vector<std::string>(names.rbegin(), names.rend()));
        props.insert(props.begin() + 5, {"uchar", "red"});
        o.otherElements = true;
        ExpectOk("ascii degree-2 reversed, extras, other elements",
                 WritePly(dir, "asc2", props, n, o), n, 24);
    }

    // ── Refusals ──
    {
        std::vector<std::string> names = Degree0Names();
        names.pop_back();  // drop rot_3
        ExpectError("missing rot_3", WritePly(dir, "norot3", FloatProps(names), n), "'rot_3'");
    }
    {
        std::vector<std::string> names = Degree0Names();
        names.erase(names.begin() + 3);  // drop f_dc_0
        ExpectError("missing f_dc_0", WritePly(dir, "nodc0", FloatProps(names), n), "'f_dc_0'");
    }
    ExpectError("f_rest count not a whole SH degree",
                WritePly(dir, "rest5", FloatProps(StandardNames(5, false)), n), "f_rest");
    {
        const std::string p = (dir / "vlist.ply").string();
        {
            std::ofstream f(p, std::ios::binary);
            f << "ply\nformat binary_little_endian 1.0\nelement vertex 1\n";
            for (const auto& nm : Degree0Names()) f << "property float " << nm << "\n";
            f << "property list uchar int extra\nend_header\n";
        }
        ExpectError("list property in the vertex element", p, "list property 'extra'");
    }
    {
        const std::string p = (dir / "fmt.ply").string();
        {
            std::ofstream f(p, std::ios::binary);
            f << "ply\nformat binary_middle_endian 1.0\nelement vertex 1\nend_header\n";
        }
        ExpectError("unknown format", p, "unsupported PLY format");
    }
    {
        WriteOpts o;
        o.fmt = Fmt::Ascii;
        const std::string p = WritePly(dir, "asctrunc", FloatProps(Degree0Names()), 40, o);
        fs::resize_file(p, fs::file_size(p) - 300);
        ExpectError("ascii truncated", p, "vertex data ends at vertex");
    }
    {
        WriteOpts o;
        o.fmt = Fmt::Ascii;
        const std::string p = WritePly(dir, "ascjunk", FloatProps(Degree0Names()), n, o);
        std::string text;
        {
            std::ifstream in(p, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const size_t at = text.find("end_header\n") + 11;
        text.replace(at, 1, "q");  // first value of vertex 0 becomes junk
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out << text;
        }
        ExpectError("ascii non-number", p, "is not a number");
    }
    {
        // A leading element whose data is cut short.
        const std::string p = (dir / "leadtrunc.ply").string();
        {
            std::ofstream f(p, std::ios::binary);
            f << "ply\nformat binary_little_endian 1.0\nelement big 1000000\nproperty double v\n"
                 "element vertex 1\n";
            for (const auto& nm : Degree0Names()) f << "property float " << nm << "\n";
            f << "end_header\n";
            for (int i = 0; i < 100; i++) PutRaw(f, Fmt::LE, 1.0);
        }
        ExpectError("leading element truncated", p, "element 'big'");
    }
    {
        // Truncated data: header promises more records than the file holds.
        const std::string p = WritePly(dir, "trunc", FloatProps(Degree0Names()), n);
        fs::resize_file(p, fs::file_size(p) - 4);
        ExpectError("truncated data", p, "bytes follow the header");
    }

    fs::remove_all(dir, ec);
    if (g_failures) {
        std::printf("gs_ply_loader_test: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gs_ply_loader_test: all checks passed\n");
    return 0;
}

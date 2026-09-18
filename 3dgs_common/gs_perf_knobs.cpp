// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//! @file  Implementation of the `DXR_GS_*` knob lookups + the PNG dump writer.

#include "gs_perf_knobs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <zlib.h>

#if defined(__ANDROID__)
#  include <sys/system_properties.h>
#endif

#if defined(_WIN32)
// GetEnvironmentVariableA, not getenv: the CRT keeps its own snapshot of the
// environment taken at process start, and a `set VAR=1 && app.exe` parent does
// not necessarily land in it (the same reason windows/main.cpp reads
// DXR_LAUNCH_QUIET through the Win32 API). Declared locally so this TU does
// not have to pull in <windows.h>, which would leak min/max macros into every
// consumer of the renderer headers.
extern "C" __declspec(dllimport) unsigned long __stdcall
GetEnvironmentVariableA(const char *lpName, char *lpBuffer, unsigned long nSize);
#endif

namespace gsperf {

namespace {

void copyInto(char *out, size_t outSize, const char *v)
{
    if (outSize == 0) return;
    size_t i = 0;
    for (; v[i] != '\0' && i + 1 < outSize; i++) out[i] = v[i];
    out[i] = '\0';
}

bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
    }
    return *a == *b;
}

}  // namespace

bool lookup(const char *envName, const char *androidProp, char *out, size_t outSize)
{
    if (envName != nullptr && outSize > 0) {
#if defined(_WIN32)
        char buf[256] = {0};
        unsigned long n = GetEnvironmentVariableA(envName, buf, (unsigned long)sizeof(buf));
        if (n > 0 && n < sizeof(buf) && buf[0] != '\0') {
            copyInto(out, outSize, buf);
            return true;
        }
#else
        const char *v = getenv(envName);
        if (v != nullptr && v[0] != '\0') {
            copyInto(out, outSize, v);
            return true;
        }
#endif
    }
#if defined(__ANDROID__)
    if (androidProp != nullptr && outSize > 0) {
        char buf[PROP_VALUE_MAX] = {0};
        if (__system_property_get(androidProp, buf) > 0 && buf[0] != '\0') {
            copyInto(out, outSize, buf);
            return true;
        }
    }
#else
    (void)androidProp;
#endif
    return false;
}

float getFloat(const char *envName, const char *androidProp, float def)
{
    char buf[64];
    if (!lookup(envName, androidProp, buf, sizeof(buf))) return def;
    char *end = nullptr;
    const double d = strtod(buf, &end);
    if (end == buf) return def;  // unparseable → documented default, never 0
    return (float)d;
}

bool getBool(const char *envName, const char *androidProp, bool def)
{
    char buf[64];
    if (!lookup(envName, androidProp, buf, sizeof(buf))) return def;
    if (ieq(buf, "0") || ieq(buf, "off") || ieq(buf, "false") || ieq(buf, "no")) return false;
    return true;
}

bool equals(const char *envName, const char *androidProp, const char *match)
{
    char buf[64];
    if (!lookup(envName, androidProp, buf, sizeof(buf))) return false;
    return ieq(buf, match);
}

Knobs load(float defaultRenderScale, float defaultKeepFrac)
{
    Knobs k;
    k.renderScale = getFloat("DXR_GS_SCALE", "debug.dxr.gs.scale", defaultRenderScale);
    if (!(k.renderScale > 0.05f && k.renderScale <= 1.0f)) k.renderScale = defaultRenderScale;
    k.keepFrac = getFloat("DXR_GS_KEEP", "debug.dxr.gs.keep", defaultKeepFrac);
    if (!(k.keepFrac > 0.01f && k.keepFrac <= 1.0f)) k.keepFrac = defaultKeepFrac;
    k.cullAlpha = getFloat("DXR_GS_CULL_ALPHA", "debug.dxr.gs.cullalpha", 0.0f);
    if (!(k.cullAlpha >= 0.0f && k.cullAlpha < 1.0f)) k.cullAlpha = 0.0f;
    // Kill switch, not a selector: anything other than the literal "3sigma"
    // leaves the (bit-exact) opacity-aware extent on.
    k.opacityExtent = !equals("DXR_GS_EXTENT", "debug.dxr.gs.extent", "3sigma");
    k.invisibleCull = getBool("DXR_GS_INVISIBLE_CULL", "debug.dxr.gs.invcull", true);
    k.compactDraw = getBool("DXR_GS_COMPACT", "debug.dxr.gs.compact", true);
    k.maxRadiusFrac = getFloat("DXR_GS_MAX_RADIUS_FRAC", "debug.dxr.gs.maxradiusfrac", 0.0f);
    if (!(k.maxRadiusFrac > 0.0f && k.maxRadiusFrac <= 1.0f)) k.maxRadiusFrac = 0.0f;
    lookup("DXR_GS_DUMP", "debug.dxr.gs.dump", k.dumpPath, sizeof(k.dumpPath));
    const float df = getFloat("DXR_GS_DUMP_FRAME", "debug.dxr.gs.dumpframe", 240.0f);
    k.dumpFrame = (df >= 0.0f) ? (unsigned long long)df : 240ull;
    return k;
}

// ───────────────────────────── PNG writer ─────────────────────────────────
//
// Minimal but standards-correct: 8-bit RGBA, filter type 0 on every row, one
// zlib stream in a single IDAT. zlib is already linked into this target (the
// SPZ loader peels its own gzip wrapper), so this adds no dependency and no
// vendored stb blob. It exists purely so `DXR_GS_DUMP` can produce a file that
// can be diffed against another run — not as a capture feature.

namespace {

void be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t> &out, const char tag[4], const uint8_t *data, size_t n)
{
    be32(out, (uint32_t)n);
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (n > 0) out.insert(out.end(), data, data + n);
    const uLong c = crc32(crc32(0L, Z_NULL, 0), out.data() + start, (uInt)(out.size() - start));
    be32(out, (uint32_t)c);
}

}  // namespace

bool writePng(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height)
{
    if (path == nullptr || rgba == nullptr || width == 0 || height == 0) return false;

    // Raw scanlines, each prefixed by filter byte 0 (None).
    std::vector<uint8_t> raw;
    raw.reserve((size_t)height * ((size_t)width * 4 + 1));
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);
        const uint8_t *row = rgba + (size_t)y * (size_t)width * 4;
        raw.insert(raw.end(), row, row + (size_t)width * 4);
    }

    uLongf compLen = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    comp.resize(compLen);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(width >> 24);  ihdr[1] = (uint8_t)(width >> 16);
    ihdr[2] = (uint8_t)(width >> 8);   ihdr[3] = (uint8_t)width;
    ihdr[4] = (uint8_t)(height >> 24); ihdr[5] = (uint8_t)(height >> 16);
    ihdr[6] = (uint8_t)(height >> 8);  ihdr[7] = (uint8_t)height;
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // colour type RGBA
    ihdr[10] = 0;  // deflate
    ihdr[11] = 0;  // adaptive filtering
    ihdr[12] = 0;  // no interlace
    chunk(png, "IHDR", ihdr, sizeof(ihdr));
    chunk(png, "IDAT", comp.data(), comp.size());
    chunk(png, "IEND", nullptr, 0);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) return false;
    const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}

}  // namespace gsperf

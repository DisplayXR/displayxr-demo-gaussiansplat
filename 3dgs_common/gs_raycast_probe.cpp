// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  `gs_raycast_probe` — build + per-frame cost of the cursor ray cast
 *         (gs_raycast.h) on real assets, and its agreement with a brute-force
 *         composite, without a GPU, a window or a runtime.
 *
 * Usage:
 *     gs_raycast_probe <scene.ply|scene.spz|scene.sog> [more scenes...]
 *
 * Per asset: load, build the grid (timed), then
 *   - 600 "frames" of 20 rays each (the cursor's footprint: 10 points x two
 *     eyes 64 mm apart, scaled to the scene), the pointer wandering over the
 *     scene's fit box seen from +Z: mean / max wall time per frame, hit rate;
 *   - 200 rays checked against a brute-force composite over every kept splat
 *     (the same evaluation, no grid): the grid must return the same t.
 */

#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "gs_raycast.h"
#include "gs_scene_fit.h"
#include "gs_scene_loader.h"
#include "gs_sog_loader.h"
#include "gs_spz_loader.h"

namespace {

//! This thread's CPU time, us: the cost of the ray cast itself, immune to the
//! preemption a loaded build box adds to wall time. (Windows: wall time; the
//! MSVC CRT has no per-thread CPU clock.)
double ThreadCpuUs()
{
#if defined(_WIN32)
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
#endif
}

bool LoadScene(const std::string& path, std::vector<GsVertex>& verts, std::string& err)
{
    const size_t dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? std::string() : path.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".spz") {
        SpzFileInfo info;
        if (ParseSpzFile(path, verts, &info)) return true;
        err = info.error;
        return false;
    }
    if (ext == ".sog") {
        SogFileInfo info;
        GsSceneCamera cam;
        if (ParseSogFile(path, verts, &info, &cam)) return true;
        err = info.error;
        return false;
    }
    if (ParsePlyFile(path, verts)) return true;
    err = "not a readable PLY scene";
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: gs_raycast_probe <scene> [scene...]\n");
        return 2;
    }
    int rc = 0;
    for (int ai = 1; ai < argc; ai++) {
        const std::string path = argv[ai];
        std::vector<GsVertex> verts;
        std::string err;
        if (!LoadScene(path, verts, err) || verts.empty()) {
            std::printf("%s: load failed: %s\n", path.c_str(), err.c_str());
            rc = 1;
            continue;
        }
        const GsFitBounds fb = GsResolveFitBounds(verts.data(), verts.size(), 64u);
        if (!fb.valid) { std::printf("%s: no fit bounds\n", path.c_str()); continue; }

        GsSplatRayCaster rc0;
        const double bc0 = ThreadCpuUs();
        rc0.build(verts.data(), verts.size());
        const double buildCpuMs = (ThreadCpuUs() - bc0) / 1000.0;
        const GsSplatRayCaster::BuildStats s = rc0.buildStats();
        std::printf("%s\n  splats %zu, kept %zu (opacity >= 0.1, floaters trimmed)\n"
                    "  hierarchy %zu nodes / %u levels, %.1f MB, build %.1f ms wall / %.1f ms CPU\n",
                    path.c_str(), s.input, s.kept, s.nodes, s.levels, (double)s.bytes / 1048576.0, s.ms,
                    buildCpuMs);

        // Reference: the same evaluation over every kept splat, no hierarchy.
        GsSplatRayCaster bf;
        GsSplatRayCaster::Options bo;
        bo.bruteForce = true;
        bf.build(verts.data(), verts.size(), bo);

        const float ext = std::max({fb.extent[0], fb.extent[1], fb.extent[2]});
        const float eyeZ = fb.center[2] + 1.5f * ext;      // viewer on +Z
        const float ipd = 0.064f * ext / 0.3f;             // 64 mm on a ~30 cm-tall subject
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> U(0.0f, 1.0f);

        auto makeRay = [&](float u, float v, float eyeX, float o[3], float d[3]) {
            o[0] = fb.center[0] + eyeX; o[1] = fb.center[1]; o[2] = eyeZ;
            const float tx = fb.center[0] + (u - 0.5f) * fb.extent[0];
            const float ty = fb.center[1] + (0.5f - v) * fb.extent[1];
            const float tz = fb.center[2];
            d[0] = tx - o[0]; d[1] = ty - o[1]; d[2] = tz - o[2];
            const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            d[0] /= l; d[1] /= l; d[2] /= l;
        };

        // Per-frame cost.
        const int kFrames = 600;
        double sumUs = 0.0, maxUs = 0.0, sumCpu = 0.0, maxCpu = 0.0;
        uint64_t hits = 0, rays = 0, tests = 0, nodes = 0;
        for (int f = 0; f < kFrames; f++) {
            const float pu = U(rng), pv = U(rng);
            const double c0 = ThreadCpuUs();
            const auto t0 = std::chrono::steady_clock::now();
            for (int p = 0; p < 10; p++) {
                const float du = (p == 0) ? 0.0f : 0.02f * std::cos(0.785f * (float)p);
                const float dv = (p == 0) ? 0.0f : 0.02f * std::sin(0.785f * (float)p) + 0.015f;
                for (int e = 0; e < 2; e++) {
                    float o[3], d[3], t;
                    makeRay(pu + du, pv + dv, (e ? 0.5f : -0.5f) * ipd, o, d);
                    GsSplatRayCaster::RayStats st;
                    if (rc0.raycast(o, d, 0.0f, 1e30f, t, &st)) hits++;
                    rays++;
                    tests += st.tests;
                    nodes += st.nodes;
                }
            }
            const double us =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            const double cpu = ThreadCpuUs() - c0;
            sumCpu += cpu;
            if (cpu > maxCpu) maxCpu = cpu;
            sumUs += us;
            if (us > maxUs) maxUs = us;
        }
        std::printf("  cost: CPU %.1f us/frame mean, %.1f us max; wall %.1f mean, %.1f max "
                    "(20 rays/frame, %d frames), hit %.0f%% of rays, %.0f splat tests + %.0f box tests/ray\n",
                    sumCpu / kFrames, maxCpu, sumUs / kFrames, maxUs, kFrames, 100.0 * (double)hits / (double)rays,
                    (double)tests / (double)rays, (double)nodes / (double)rays);

        // Agreement with brute force.
        int agree = 0, both = 0, onlyGrid = 0, onlyBf = 0, n = 200;
        double maxRel = 0.0, bfUs = 0.0;
        for (int i = 0; i < n; i++) {
            float o[3], d[3], tg = 0, tb = 0;
            makeRay(U(rng), U(rng), 0.0f, o, d);
            const bool hg = rc0.raycast(o, d, 0.0f, 1e30f, tg);
            const auto t0 = std::chrono::steady_clock::now();
            const bool hb = bf.raycast(o, d, 0.0f, 1e30f, tb);
            bfUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            if (hg && hb) {
                both++;
                const double rel = std::fabs((double)tg - tb) / std::max(1e-6, (double)tb);
                if (rel > maxRel) maxRel = rel;
                if (rel >= 1e-3) std::printf("    disagree: hierarchy t=%.5f brute t=%.5f\n", tg, tb);
                if (rel < 1e-3) agree++;
            } else if (hg) {
                onlyGrid++;
                std::printf("    hierarchy-only hit: t=%.5f\n", tg);
            } else if (hb) {
                onlyBf++;
                std::printf("    brute-only hit: t=%.5f\n", tb);
            } else {
                agree++;
            }
        }
        std::printf("  vs brute force (%d rays, %.0f us/ray brute): agree %d, both-hit %d "
                    "(max rel dt %.2e), hierarchy-only %d, brute-only %d\n",
                    n, bfUs / n, agree, both, maxRel, onlyGrid, onlyBf);
        if (onlyGrid || onlyBf || maxRel > 1e-3) rc = 1;
    }
    return rc;
}

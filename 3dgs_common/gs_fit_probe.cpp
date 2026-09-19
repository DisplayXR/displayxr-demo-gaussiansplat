// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  `gs_fit_probe` — print what the scene fit does to an asset, without
 *         a GPU, a window or a runtime.
 *
 * The fit is a load-time measurement (gs_scene_fit.h), so it can be run from a
 * command line, and it should be: the only other way to see these numbers is
 * to launch the viewer on a 3D panel and read a log line, which is neither
 * scriptable nor available on the platform whose framing you are comparing
 * against.
 *
 * Prints, per asset and per viewport:
 *
 *   - OLD percentile   the [p5, p95] box, i.e. what the GRAPHICS leg
 *                      (`gs_adreno_renderer.cpp`) frames with today on Apple
 *                      Silicon / Android / Windows-on-ARM;
 *   - OLD flood-fill   the voxel flood-fill, i.e. what the COMPUTE leg
 *                      (`gs_renderer.cpp`) frames with today on desktop x86-64
 *                      — and, after this module lands, what BOTH legs use;
 *   - NEW vHeight      the same flood-fill bounds put through the pose-aware,
 *                      depth-budgeted @ref GsFitFrame.
 *
 * Usage:
 *     gs_fit_probe <scene.ply|scene.spz|scene.sog> [more scenes...]
 *
 * Options:
 *     --yaw=<deg> --pitch=<deg>   viewing pose to project into (default 0/0,
 *                                 which is the display rig's rest pose)
 *     --fill=<f>                  fill fraction (default 0.88, the value both
 *                                 main.mm and main.cpp pass)
 *     --viewport=<W>x<H>          replace the default landscape+portrait pair
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gs_scene_fit.h"
#include "gs_scene_loader.h"
#include "gs_sog_loader.h"
#include "gs_spz_loader.h"

namespace {

constexpr float kDeg2Rad = 0.01745329251994329577f;

//! The x/y-only rule, i.e. `dxr::AutoFitVHeight`. Duplicated here (three
//! lines) rather than pulled in from displayxr-common, which this directory
//! deliberately does not depend on — the probe must build wherever
//! 3dgs_common does.
float AutoFitVHeightXY(float extentW, float extentH,
                       float viewportW, float viewportH, float fill)
{
    if (!(extentH > 0.0f) || !(fill > 0.0f)) return 0.0f;
    float vh = extentH / fill;
    if (extentW > 0.0f && viewportW > 0.0f && viewportH > 0.0f) {
        const float aspect = viewportW / viewportH;
        const float vhForWidth = extentW / (fill * aspect);
        if (vhForWidth > vh) vh = vhForWidth;
    }
    return vh;
}

const char* SourceName(GsBoundsSource s)
{
    switch (s) {
    case GsBoundsSource::FloodFillObject: return "flood-fill/object";
    case GsBoundsSource::FloodFillScene:  return "flood-fill/scene";
    case GsBoundsSource::Percentile:      return "percentile p5-p95";
    default:                              return "none";
    }
}

bool LoadScene(const std::string& path, std::vector<GsVertex>& verts,
               std::string& err)
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
    err = "not a readable PLY scene (corrupt or unsupported)";
    return false;
}

bool ParseArgFloat(const char* arg, const char* key, float& out)
{
    const size_t klen = strlen(key);
    if (strncmp(arg, key, klen) != 0) return false;
    out = (float)atof(arg + klen);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    float yawDeg = 0.0f, pitchDeg = 0.0f, fill = 0.88f;
    std::vector<std::pair<float, float>> viewports;
    std::vector<std::string> scenes;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        float v = 0.0f;
        if (ParseArgFloat(a, "--yaw=", v))        { yawDeg = v; continue; }
        if (ParseArgFloat(a, "--pitch=", v))      { pitchDeg = v; continue; }
        if (ParseArgFloat(a, "--fill=", v))       { fill = v; continue; }
        if (strncmp(a, "--viewport=", 11) == 0) {
            int w = 0, h = 0;
            if (sscanf(a + 11, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                viewports.emplace_back((float)w, (float)h);
            } else {
                fprintf(stderr, "gs_fit_probe: bad --viewport= (want WxH): %s\n", a);
                return 2;
            }
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "gs_fit_probe: unknown option %s\n", a);
            return 2;
        }
        scenes.emplace_back(a);
    }

    if (scenes.empty()) {
        fprintf(stderr,
                "usage: gs_fit_probe [--yaw=deg] [--pitch=deg] [--fill=f] "
                "[--viewport=WxH] <scene>...\n");
        return 2;
    }
    if (viewports.empty()) {
        viewports.emplace_back(1280.0f, 720.0f);   // landscape
        viewports.emplace_back(720.0f, 1280.0f);   // portrait
    }

    const float yaw = yawDeg * kDeg2Rad;
    const float pitch = pitchDeg * kDeg2Rad;

    GsFitComfort comfort;
    float dFront = 0.0f, dBehind = 0.0f;
    GsFitDepthBudgetVH(comfort, dFront, dBehind);
    printf("comfort: viewer %.2f vH, eyes %.3f vH, max disparity %.3f vH "
           "-> half-depth budget front %.3f vH, behind %.3f vH "
           "(growth cap %.1fx)\n",
           comfort.viewerDistanceVH, comfort.eyeSeparationVH,
           comfort.maxDisparityVH, dFront, dBehind, kGsFitMaxDepthGrowth);
    printf("pose: yaw %.1f deg, pitch %.1f deg, fill %.2f\n\n", yawDeg, pitchDeg, fill);

    int failures = 0;
    for (const std::string& path : scenes) {
        std::vector<GsVertex> verts;
        std::string err;
        if (!LoadScene(path, verts, err) || verts.empty()) {
            fprintf(stderr, "FAIL %s: %s\n", path.c_str(),
                    err.empty() ? "no gaussians" : err.c_str());
            failures++;
            continue;
        }

        std::vector<GsFitPoint> pts;
        GsBuildFitPoints(verts.data(), verts.size(), pts);

        printf("=== %s\n", path.c_str());
        printf("    %zu gaussians, %zu after floater rejection (%.2f%% dropped)\n",
               verts.size(), pts.size(),
               verts.empty() ? 0.0
                             : 100.0 * (double)(verts.size() - pts.size()) /
                                   (double)verts.size());

        float pc[3] = {0, 0, 0}, pe[3] = {0, 0, 0};
        const bool havePct = GsPercentileBounds(verts.data(), verts.size(),
                                                0.05f, 0.95f, pc, pe);
        const GsFitBounds ff = GsMainObjectBoundsEx(verts.data(), verts.size(), 64u);

        if (havePct) {
            printf("    OLD percentile  center=(%8.3f,%8.3f,%8.3f) "
                   "extent=(%8.3f,%8.3f,%8.3f)\n",
                   pc[0], pc[1], pc[2], pe[0], pe[1], pe[2]);
        } else {
            printf("    OLD percentile  <unavailable>\n");
        }
        if (ff.valid) {
            printf("    NEW flood-fill  center=(%8.3f,%8.3f,%8.3f) "
                   "extent=(%8.3f,%8.3f,%8.3f)  [%s, %zu/%zu voxels = %.2f%%, "
                   "thresh %.2fx peak]\n",
                   ff.center[0], ff.center[1], ff.center[2],
                   ff.extent[0], ff.extent[1], ff.extent[2],
                   SourceName(ff.source), ff.filledVoxels, ff.totalVoxels,
                   100.0 * (double)ff.fillRatio, ff.threshold);
        } else {
            printf("    NEW flood-fill  <found nothing — falls back to percentile>\n");
        }

        const GsFitBounds resolved = GsResolveFitBounds(verts.data(), verts.size(), 64u);

        for (const auto& vp : viewports) {
            const float vpW = vp.first, vpH = vp.second;
            const char* shape = (vpW >= vpH) ? "landscape" : "portrait ";

            const float oldPctVh =
                havePct ? AutoFitVHeightXY(pe[0], pe[1], vpW, vpH, fill) : 0.0f;
            const float oldFfVh =
                ff.valid ? AutoFitVHeightXY(ff.extent[0], ff.extent[1], vpW, vpH, fill)
                         : 0.0f;

            const GsFitFrameResult fr =
                GsFitFrameEx(resolved, vpW, vpH, fill, yaw, pitch, comfort);

            printf("    %s %4.0fx%-4.0f  old vH: percentile %8.3f | flood-fill %8.3f"
                   "   NEW vH %8.3f (flat %8.3f, depth-asks %8.3f, bound=%s%s)"
                   "  pivot-shift %7.3f\n",
                   shape, vpW, vpH, oldPctVh, oldFfVh,
                   fr.vHeight, fr.vHeightFlat, fr.vHeightDepth, fr.boundBy,
                   fr.depthBudgetSatisfied ? "" : ", CLAMPED",
                   fr.pivotDepthShift);
            printf("        projected screen extents W=%.3f H=%.3f D=%.3f\n",
                   fr.screenW, fr.screenH, fr.screenD);
        }
        printf("\n");
    }

    return failures ? 1 : 0;
}

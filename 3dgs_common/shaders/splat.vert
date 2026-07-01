// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Adreno/TBDR-native Gaussian-splat rasterizer (graphics pipeline).
//
// Instead of the desktop compute-tile pipeline (expand to ~1.4M per-tile
// fragments → global radix sort → per-pixel composite into a storage image),
// this draws each gaussian as an INSTANCED screen-space quad with hardware
// alpha blending. The rasterizer + ROP composite in on-chip tile memory (GMEM)
// — exactly what a tile-based mobile GPU is built for — so there is no global
// fragment sort, no prefix-sum, no storage-image scatter/gather.
//
// One instance = one gaussian, drawn in back-to-front order (the payload buffer
// is the gaussians sorted far→near by view depth). The 4 vertices of the
// instance form the splat's 3-sigma bounding quad; the fragment shader does the
// conic falloff. All per-gaussian terms (screen center uv, conic, colour,
// opacity, pixel radius) are exactly what preprocess.comp already computes.

#version 450
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"

layout (std430, set = 0, binding = 0) readonly buffer Vertices {
    VertexAttribute attr[];
};

// Gaussian indices sorted back-to-front (far first) by the depth sort.
layout (std430, set = 0, binding = 1) readonly buffer SortedPayloads {
    uint sorted[];
};

layout (push_constant) uniform Constants {
    uint width;
    uint height;
};

// Triangle-strip corners of the unit quad: (-1,-1)(1,-1)(-1,1)(1,1).
const vec2 kCorners[4] = vec2[4](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

layout (location = 0) out vec3 v_conic;
layout (location = 1) out vec3 v_color;
layout (location = 2) out float v_opacity;
layout (location = 3) out vec2 v_offset;  // pixel offset from the gaussian center

void main() {
    uint gi = sorted[gl_InstanceIndex];
    VertexAttribute a = attr[gi];

    float radii = a.color_radii.w;
    // Culled gaussians (preprocess wrote radii 0 / no magic) collapse to a
    // degenerate point so they cover no fragments — cheaper than compacting.
    if (radii <= 0.0 || a.magic != MAGIC) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // off-clip, discarded
        v_conic = vec3(0.0); v_color = vec3(0.0); v_opacity = 0.0; v_offset = vec2(0.0);
        return;
    }

    vec2 corner = kCorners[gl_VertexIndex];
    vec2 offset = corner * radii;             // pixels
    vec2 px = a.uv + offset;                  // pixel-space quad corner

    // Pixel → Vulkan NDC. uv already carries the Y-down raster flip
    // (preprocess uses ndc2Pix(-ndc.y)), and pixel (0,0) = top-left = NDC(-1,-1),
    // so a plain scale/bias matches the framebuffer the same way the compute
    // path's imageStore(curr_uv) did.
    vec2 ndc = (px / vec2(float(width), float(height))) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    v_conic = a.conic_opacity.xyz;
    v_color = a.color_radii.xyz;
    v_opacity = a.conic_opacity.w;
    v_offset = offset;
}

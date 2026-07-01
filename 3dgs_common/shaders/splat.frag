// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
//
// Fragment stage of the graphics-pipeline splat rasterizer. Evaluates the 2D
// gaussian (conic quadratic form) at this pixel and emits PREMULTIPLIED colour.
// The pipeline is configured for back-to-front "over" blending:
//   srcFactor = ONE, dstFactor = ONE_MINUS_SRC_ALPHA
// so accumulating the depth-sorted instances reproduces the same result as the
// compute path's front-to-back T *= (1-a) composite. The per-fragment math is
// identical to render.comp (conic power, 0.99 opacity clamp, 1/255 floor).

#version 450

layout (location = 0) in vec3 v_conic;
layout (location = 1) in vec3 v_color;
layout (location = 2) in float v_opacity;
layout (location = 3) in vec2 v_offset;  // pixel offset from gaussian center

layout (location = 0) out vec4 o_color;

void main() {
    // power = -0.5 (cxx dx^2 + czz dy^2) - cxy dx dy   (conic = inverse cov2d)
    float dx = v_offset.x;
    float dy = v_offset.y;
    float power = -0.5 * (v_conic.x * dx * dx + v_conic.z * dy * dy)
                  - v_conic.y * dx * dy;
    if (power > 0.0) {
        discard;
    }

    float alpha = min(0.99, v_opacity * exp(power));
    if (alpha < 1.0 / 255.0) {
        discard;
    }

    // Premultiplied output for ONE / ONE_MINUS_SRC_ALPHA blending.
    o_color = vec4(v_color * alpha, alpha);
}

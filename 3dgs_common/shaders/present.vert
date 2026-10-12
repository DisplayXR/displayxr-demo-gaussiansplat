// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Present pass, vertex stage: one oversized triangle covering the whole
// framebuffer (vertices from gl_VertexIndex, no vertex buffer). The render
// pass's renderArea + scissor restrict it to the destination tile, and the
// fragment stage maps gl_FragCoord back into the source render target.
// See present.frag and GsAdrenoRenderer::renderEye for why this pass exists.

#version 450

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
//
// Present pass, fragment stage: move the splat render target's region
// (0,0)-(srcSize) onto the destination tile of the swapchain image.
//
// It replaces vkCmdBlitImage + an UNORM scratch + vkCmdCopyImage onto an
// *_SRGB swapchain (gsCmdBlitToSwapchain). That copy is byte-exact but on
// Adreno the driver allocates and zeroes GPU memory INSIDE vkCmdCopyImage at
// record time and frees it again at vkResetCommandBuffer — measured at 88 % of
// the render thread's CPU on an Adreno 740 (gauss #143), the same trap the
// runtime hit in its own compose path (displayxr-runtime#1935).
//
// The colour attachment is an UNORM view of the swapchain image (legal because
// the swapchain is created with XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT), so the
// display-referred bytes in the source land unchanged — no sRGB encode on
// write, exactly like the copy. At 1:1 (`nearest`) the texel is fetched, not
// filtered: an 8-bit UNORM value read as float and written back to an 8-bit
// UNORM attachment round-trips exactly, so the result is bit-identical to the
// blit+copy path. Scaled (render scale < 1) it is a bilinear upscale clamped
// to the source region, the same filter the blit applied.

#version 450

layout (set = 0, binding = 0) uniform sampler2D src_tex;

layout (push_constant) uniform PresentPush {
    vec2 dst_origin;     // top-left of the destination tile, framebuffer px
    vec2 dst_size;       // destination tile size, px
    vec2 src_size;       // used region of src_tex, texels
    vec2 src_inv_tex;    // 1 / full src_tex size
    uint nearest;        // 1 = 1:1 texel fetch, 0 = bilinear
} pc;

layout (location = 0) out vec4 o_color;

void main() {
    vec2 rel = (gl_FragCoord.xy - pc.dst_origin) / pc.dst_size;  // 0..1 in the tile
    if (pc.nearest != 0u) {
        ivec2 t = clamp(ivec2(floor(rel * pc.src_size)), ivec2(0), ivec2(pc.src_size) - 1);
        o_color = texelFetch(src_tex, t, 0);
    } else {
        vec2 p = clamp(rel * pc.src_size, vec2(0.5), pc.src_size - vec2(0.5));
        o_color = textureLod(src_tex, p * pc.src_inv_tex, 0.0);
    }
}

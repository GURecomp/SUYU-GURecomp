// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// NVN Vulkan renderer, [Debug] nvn_fsr_objvel: per-pixel object motion vector of a tracked draw (see the .vert).
//  - coverage / depth validation: the pixel keeps the vector only if this surface is what the final scene depth shows
//    (linear depth within max(2 units, 1%) of the scene depth); a later opaque draw in front, or the back faces of the
//    object itself, fail the test. Draws are rasterized without culling or a depth attachment.
//  - mv = previous position - current position, both with their frame's jitter removed (FSR convention: render px,
//    current + mv = previous).
// Output RGBA16F: rg = mv, b = 1 (valid), a = 1 in a low-rate pose update (jump) frame of the draw, else 0.

#version 450

layout(location = 0) in vec4 prev_clip;
layout(location = 1) in float bad;

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform sampler2D cam_mv;
// per tracked draw of this frame (draw_idx): bit 1 = visible (valid texel), bit 0 = its object vector differs from the
// camera vector somewhere (> 0.25 px); sampled on every 4th texel in x and y (static-draw exclusion, nvn_vk ObjvRun)
layout(set = 0, binding = 2, std430) buffer DrawFlags {
    uint draw_flags[];
};

layout(location = 0) out vec4 o_vel;

layout(push_constant) uniform PushConstants {
    vec4 vp;
    vec2 shift_cur;
    vec2 shift_prev;
    float p22;
    float p32;
    float max_px;
    uint flags;
    uint draw_idx;
} pc;

float Linear(float z) {
    return pc.p32 / (z + pc.p22);
}

void main() {
    if (bad > 0.0) {
        discard;
    }
    const ivec2 size = textureSize(scene_depth, 0);
    const ivec2 p = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
    const float zs = texelFetch(scene_depth, p, 0).r;
    const float zf = gl_FragCoord.z;
    if ((pc.flags & 1u) != 0u) {
        const float ls = Linear(zs), lf = Linear(zf);
        if (!(abs(lf - ls) <= max(2.0, 0.01 * max(lf, 0.0)))) {
            discard;
        }
    } else if (!(abs(zf - zs) <= max(2e-7, 2e-3 * (1.0 - zs)))) {
        discard;
    }
    if (!(prev_clip.w > 1e-6)) {
        discard;
    }
    const vec2 prev_px = pc.vp.xy + (prev_clip.xy / prev_clip.w * 0.5 + 0.5) * pc.vp.zw;
    const vec2 mv = (prev_px - pc.shift_prev) - (gl_FragCoord.xy - pc.shift_cur);
    if (any(isnan(mv)) || any(isinf(mv)) || !(length(mv) <= pc.max_px)) {
        discard;
    }
    if ((pc.flags & 4u) != 0u && (int(gl_FragCoord.x) & 3) == 0 && (int(gl_FragCoord.y) & 3) == 0) {
        const bool moving = length(mv - texelFetch(cam_mv, p, 0).rg) > 0.25;
        atomicOr(draw_flags[pc.draw_idx], moving ? 3u : 2u);
    }
    // a = 1: low-rate (cloth) pose update in this frame (flags bit 1): the prep pass raises the reactive mask there
    o_vel = vec4(mv, 1.0, (pc.flags & 2u) != 0u ? 1.0 : 0.0);
}

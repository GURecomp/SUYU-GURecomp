// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// NVN Vulkan renderer, [Debug] nvn_fsr_objvel: geometry object motion vectors.
// Input: two transform feedback captures of the same game draw, one per vertex of every assembled triangle:
//   location 0 = this frame's clip position (the game's vertex shader output, jittered with this frame's jitter),
//   location 1 = the previous frame's clip position of the same vertex (its own jitter).
// The triangle is rasterized at this frame's position, i.e. exactly where the game drew it (same floats -> same
// pixels and depth); the fragment shader turns the previous position into a motion vector. A vertex that moved more
// than max_px render pixels (or lies behind the previous camera) marks its triangles as untrusted.

#version 450

layout(location = 0) in vec4 cur_clip;
layout(location = 1) in vec4 prev_clip;

layout(location = 0) out vec4 o_prev_clip;
layout(location = 1) out float o_bad;

layout(push_constant) uniform PushConstants {
    vec4 vp;         // viewport x, y, w, h (h negative when the game's picture is flipped), framebuffer px
    vec2 shift_cur;  // picture shift (jitter) of this frame, framebuffer px, +y down
    vec2 shift_prev; // picture shift of the previous frame
    float p22;       // projection terms for linear depth (0 = unknown)
    float p32;
    float max_px;
    uint flags;
    uint draw_idx;
} pc;

vec2 ToPx(vec4 c) {
    const vec2 ndc = c.xy / c.w;
    return pc.vp.xy + (ndc * 0.5 + 0.5) * pc.vp.zw;
}

void main() {
    gl_Position = cur_clip;
    o_prev_clip = prev_clip;
    float bad = 0.0;
    if (!(prev_clip.w > 1e-6) || !(cur_clip.w > 1e-6) || any(isnan(prev_clip)) || any(isinf(prev_clip))) {
        bad = 1.0;
    } else {
        const vec2 mv = (ToPx(prev_clip) - pc.shift_prev) - (ToPx(cur_clip) - pc.shift_cur);
        if (!(length(mv) <= pc.max_px)) {
            bad = 1.0;
        }
    }
    o_bad = bad;
}

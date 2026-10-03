// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"

namespace Common::GpuProbe {

/// One draw as the GPU emulation executes it. The NVN shadow layer (core/arm/recomp) records
/// the same fields from the game's NVN calls and compares the two streams.
struct Draw {
    u32 topology;
    u32 indexed;
    u32 count;       // vertices or indices
    u32 first;       // first vertex (non-indexed draws)
    s32 base_vertex; // indexed draws
    u32 index_format;
    u32 instances;
    u64 index_addr; // GPU address of the first index
    u64 vs;         // GPU address of the vertex program
    u64 ps;         // GPU address of the pixel program
};

/// The rest of the state a draw uses, compared field by field once a draw is paired.
struct DrawState {
    static constexpr u32 kStages = 5;    // Maxwell: vertex, tess control, tess eval, geometry, pixel
    static constexpr u32 kSlots = 18;    // constant buffer slots per stage
    static constexpr u32 kStreams = 4;   // vertex streams compared
    std::array<u64, kStreams> vb{};
    std::array<std::array<u64, kSlots>, kStages> cb_addr{};
    std::array<std::array<u32, kSlots>, kStages> cb_size{};
    std::array<f32, 4> viewport{}; // x, y, width, height of viewport 0
    u32 rt_count{};
    u64 rt0{};
    u32 rt0_format{};
    u32 rt0_width{};
    u32 rt0_height{};
    u64 zeta{}; // 0 when depth is off
};

using DrawSink = void (*)(const Draw&, const DrawState&);

/// Set by the shadow layer when it is active; null otherwise (one relaxed load per draw).
inline std::atomic<DrawSink> draw_sink{nullptr};

} // namespace Common::GpuProbe

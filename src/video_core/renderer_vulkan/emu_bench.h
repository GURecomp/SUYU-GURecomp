// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Vulkan {

/// Emu Renderer bench ([Debug] nvn_bench = 1, NvnVulkanMode() == 0): call once per RendererVulkan::Composite.
/// Writes user\emu_perf.txt. A no-op when the bench is off or the replacement renderer is active.
void EmuBenchFrame();

} // namespace Vulkan

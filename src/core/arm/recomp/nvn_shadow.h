// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}

namespace Core::NvnShadow {

/**
 * NVN shadow layer (G1). Native observers for the game's NVN calls keep a model of what the
 * game asked for (programs, bound program per command buffer, draws), while the guest NVN
 * driver still renders through the GPU emulation. Every draw the emulated Maxwell3D executes
 * is recorded too, and the two streams are compared: agreement on each field confirms how
 * the NVN arguments decode (clean room: from observation, not documentation), and is the
 * check the replacement layer (G3) is held to.
 *
 * Output: <user dir>/nvn_shadow.txt, rewritten with the NVN census.
 */

/// Observer for an NVN entry point: r0-r3 and sp as the call starts.
using Observer = void (*)(const u32* regs, Memory::Memory& mem);

/// The observer for a named entry point, or null if the shadow layer ignores it.
Observer Find(const std::string& name);

/// Starts receiving the GPU emulation's draws.
void Start();

void Flush(const std::filesystem::path& file);

} // namespace Core::NvnShadow

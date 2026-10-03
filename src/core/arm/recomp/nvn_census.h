// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}

namespace Core::NvnCensus {

/**
 * Method census (G3-0): which GPU methods the guest NVN driver writes for each command buffer
 * call. A sample of calls is bracketed: the command buffer object is read as the call starts
 * and again when it returns. The word that advanced is the driver's write pointer (found by
 * voting over many calls), and the words between the two pointer values are what the call
 * recorded, decoded with the GPU's pushbuffer format (suyu's dma_pusher.h). Clean room: this
 * only looks at what the driver produces, never at its code.
 *
 * Output: <user dir>/nvn_methods.txt.
 */

/// Whether calls to this entry point are sampled.
bool Wants(const std::string& name);

/// A watched call starts (the caller holds the trace lock). `call` counts this entry point's
/// calls. Returns true when this call is sampled; End must then see its return.
bool Begin(const std::string& name, u64 call, const u32* regs, Memory::Memory& mem);

/// Some block is about to run: if it is the return of a sampled call, finishes that sample.
/// Returns true if it was.
bool End(u32 pc, const u32* regs, Memory::Memory& mem);

/// Sampled calls that haven't returned yet.
int Pending();

void Flush(const std::filesystem::path& file);

} // namespace Core::NvnCensus

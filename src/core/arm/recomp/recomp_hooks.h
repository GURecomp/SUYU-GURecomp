// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

/**
 * Guest addresses the dispatcher must see on entry (G3 replacements, the NVN and timing watches,
 * game-settings hooks). Recompiled blocks call static branch targets directly (block chaining);
 * the generated code checks this bitmap and returns to the dispatcher for a marked target, so
 * every hook keeps firing. One bit per word over the loaded modules.
 */
namespace Core::RecompHooks {

/// Sets the covered range (once; later calls are ignored).
void Init(u32 lo, u32 span);

/// Marks a hooked guest address (outside the range: ignored, such code isn't chained to).
void Mark(u32 pc);

const volatile u8* Bits();
u32 Lo();
u32 Span();
} // namespace Core::RecompHooks

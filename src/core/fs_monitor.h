// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>

#include "common/common_types.h"

/**
 * File load monitor: records the game's storage reads (RomFS through IStorage) and presented
 * frames, and writes user/fs_monitor.txt every 10 s and at exit. Reads closer than 50 ms
 * together form a burst (time, wall duration, bytes, time spent reading, and the contiguous
 * offset ranges read); a frame that took over 50 ms is a hitch, listed with the burst that
 * overlapped it, if any. Offsets are RomFS offsets: tools/a32recomp/fs_monitor_map.py names
 * the files with the exported romfs.bin.
 */
namespace Core::FsMonitor {

/// Turns the monitor on (recompiled titles) and starts the 10 s report rewrite.
void Enable();

/// One storage read: RomFS offset and length, and the host time the backend read took.
void OnRead(u64 offset, u64 length, double seconds);

/// One presented frame.
void OnFrame();

/// Writes the report now (exit).
void Flush();

/// What the guest did between two instants (per-core busy share, hottest blocks), one line;
/// the recompiler's profiler provides it. Called for every hitch with the slow frame's span.
using HitchProbe = std::string (*)(std::chrono::steady_clock::time_point from,
                                   std::chrono::steady_clock::time_point to);
void SetHitchProbe(HitchProbe probe);

} // namespace Core::FsMonitor

// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Core::HostThreads {

/// CPU time used by each thread of this process so far, busiest first, with thread names
/// (GPU thread, CPU cores, shader workers...), as report lines. Empty where unsupported.
/// total_cpu_s (optional) receives the summed CPU time of the threads alive now; at exit the
/// GPU and CPU threads are already gone, so callers keep the snapshot with the largest total.
std::string Describe(double uptime_s, size_t max_threads, double* total_cpu_s = nullptr);

} // namespace Core::HostThreads

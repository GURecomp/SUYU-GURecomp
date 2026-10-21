// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

/// One device-contiguous piece of a GPU virtual range.
struct VirtualSegment {
    GPUVAddr gpu_addr;
    DAddr device_addr;
    u32 size;
};

using VirtualSegments = boost::container::small_vector<VirtualSegment, 8>;

/// Caches how a GPU virtual range splits into device-contiguous segments (port of Eden's
/// VirtualRangeCache). Entries are dropped on GPU unmaps (deferred, thread safe) and, unlike
/// Eden, re-checked on every hit: the first and last byte of each segment (or of the whole range
/// when it was not split) must still translate to the same device address, because suyu's
/// MemoryManager only reports mapped <-> unmapped changes, not remaps of mapped pages.
class VirtualRangeCache {
public:
    static constexpr size_t MAX_ENTRIES = 8192;
    static constexpr size_t MAX_DEFERRED = 4096;

    /// Returns the segments of [gpu_addr, gpu_addr + size). Empty when the range has a hole or
    /// cannot be described; a single segment when it is device-contiguous.
    const VirtualSegments* Query(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, u32 size) {
        if (has_deferred.load(std::memory_order_acquire)) {
            ApplyDeferred();
        }
        if (entries.size() > MAX_ENTRIES) {
            entries.clear();
        }
        const size_t as_id = memory.GetID();
        const u64 key = MakeKey(as_id, gpu_addr);
        const auto it = entries.find(key);
        if (it != entries.end() && it->second.as_id == as_id && it->second.gpu_addr == gpu_addr &&
            it->second.size == size && StillValid(memory, it->second)) {
            return &it->second.segments;
        }
        Entry entry{};
        entry.as_id = as_id;
        entry.gpu_addr = gpu_addr;
        entry.size = size;
        const auto ranges = memory.GetSubmappedRange(gpu_addr, size);
        GPUVAddr expected = gpu_addr;
        bool contiguous = true;
        for (const auto& [range_addr, range_size] : ranges) {
            if (range_addr != expected || range_size == 0) {
                contiguous = false;
                break;
            }
            const std::optional<DAddr> device_addr = memory.GpuToCpuAddress(range_addr);
            if (!device_addr || *device_addr == 0) {
                contiguous = false;
                break;
            }
            if (range_size > static_cast<size_t>((std::numeric_limits<u32>::max)())) {
                contiguous = false;
                break;
            }
            entry.segments.push_back(VirtualSegment{
                .gpu_addr = range_addr,
                .device_addr = *device_addr,
                .size = static_cast<u32>(range_size),
            });
            expected += range_size;
        }
        if (!contiguous || expected != gpu_addr + size) {
            entry.segments.clear();
        }
        entry.first_device = Translate(memory, gpu_addr);
        entry.last_device = Translate(memory, gpu_addr + size - 1);
        const auto result = entries.insert_or_assign(key, std::move(entry));
        return &result.first->second.segments;
    }

    /// May be called from any thread (MemoryManager map/unmap path).
    void Unmap(size_t as_id, GPUVAddr gpu_addr, u64 size) {
        if (size == 0) {
            return;
        }
        {
            std::scoped_lock lock{deferred_mutex};
            if (!deferred.empty()) {
                DeferredUnmap& last = deferred.back();
                if (last.as_id == as_id && last.gpu_addr + last.size == gpu_addr) {
                    last.size += size;
                    has_deferred.store(true, std::memory_order_release);
                    return;
                }
            }
            if (deferred.size() >= MAX_DEFERRED) {
                deferred.clear();
                deferred_overflow = true;
            } else {
                deferred.push_back(DeferredUnmap{
                    .as_id = as_id,
                    .gpu_addr = gpu_addr,
                    .size = size,
                });
            }
        }
        has_deferred.store(true, std::memory_order_release);
    }

private:
    struct Entry {
        VirtualSegments segments;
        size_t as_id{};
        GPUVAddr gpu_addr{};
        u32 size{};
        DAddr first_device{};
        DAddr last_device{};
    };

    struct DeferredUnmap {
        size_t as_id;
        GPUVAddr gpu_addr;
        u64 size;
    };

    static u64 MakeKey(size_t as_id, GPUVAddr gpu_addr) {
        return (static_cast<u64>(as_id) << 48) ^ gpu_addr;
    }

    static DAddr Translate(Tegra::MemoryManager& memory, GPUVAddr gpu_addr) {
        const std::optional<DAddr> device_addr = memory.GpuToCpuAddress(gpu_addr);
        return device_addr ? *device_addr : DAddr{0};
    }

    static bool StillValid(Tegra::MemoryManager& memory, const Entry& entry) {
        if (Translate(memory, entry.gpu_addr) != entry.first_device ||
            Translate(memory, entry.gpu_addr + entry.size - 1) != entry.last_device) {
            return false;
        }
        if (entry.segments.size() < 2) {
            return true;
        }
        for (const VirtualSegment& segment : entry.segments) {
            if (Translate(memory, segment.gpu_addr) != segment.device_addr ||
                Translate(memory, segment.gpu_addr + segment.size - 1) !=
                    segment.device_addr + segment.size - 1) {
                return false;
            }
        }
        return true;
    }

    void ApplyDeferred() {
        std::vector<DeferredUnmap> pending;
        bool overflow = false;
        {
            std::scoped_lock lock{deferred_mutex};
            has_deferred.store(false, std::memory_order_release);
            pending.swap(deferred);
            overflow = deferred_overflow;
            deferred_overflow = false;
        }
        if (overflow) {
            entries.clear();
            return;
        }
        if (pending.empty() || entries.empty()) {
            return;
        }
        for (auto it = entries.begin(); it != entries.end();) {
            const Entry& entry = it->second;
            const GPUVAddr entry_end = entry.gpu_addr + entry.size;
            bool overlaps = false;
            for (const DeferredUnmap& unmap : pending) {
                if (unmap.as_id != entry.as_id) {
                    continue;
                }
                if (entry.gpu_addr < unmap.gpu_addr + unmap.size && unmap.gpu_addr < entry_end) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    ankerl::unordered_dense::map<u64, Entry> entries;
    std::vector<DeferredUnmap> deferred;
    std::mutex deferred_mutex;
    std::atomic<bool> has_deferred{false};
    bool deferred_overflow{};
};

} // namespace VideoCommon

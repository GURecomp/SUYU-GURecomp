// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// One source range of a multi-range storage buffer: a piece of a buffer cache buffer.
struct MultiRangeSource {
    VkBuffer handle{};
    VkDeviceSize offset{};
    VkDeviceSize size{};
    u64 write_tick{};
};

struct MultiRangeRef {
    VkBuffer handle{};
    VkDeviceSize size{};
    bool needs_gather{};
};

/// Port of Eden's MultiRangeBufferCache, gather path only: a storage buffer whose GPU range is
/// split over several device ranges is copied into one host buffer, keyed by its GPU address.
/// Eden's sparse-binding alias path is not ported (needs sparse binding and allocator memory
/// locations suyu's Vulkan layer does not expose). Gathered copies are read-only.
class MultiRangeBufferCache final {
public:
    static constexpr size_t MAX_ENTRIES = 512;
    static constexpr VkDeviceSize MAX_GATHER_SIZE = 64ULL * 1024 * 1024;

    MultiRangeBufferCache() = default;

    YUZU_NON_COPYABLE(MultiRangeBufferCache);

    /// Returns the host buffer for `key`; needs_gather is set when its contents must be
    /// (re)copied from the sources. A null handle means: use the ordinary binding.
    [[nodiscard]] MultiRangeRef Get(const Device& device, Scheduler& scheduler,
                                    MemoryAllocator& memory_allocator, u64 key,
                                    std::span<const MultiRangeSource> sources,
                                    VkDeviceSize total);

    /// The gather for `key` was recorded in the current command buffer.
    void MarkGathered(Scheduler& scheduler, u64 key);

    /// Guest data of a source was uploaded: regather on next use.
    void Invalidate(u64 key);

    /// A buffer cache buffer is going away: drop every entry gathered from it.
    void DropOwner(Scheduler& scheduler, VkBuffer owner);

    [[nodiscard]] size_t NumEntries() const noexcept {
        return entries.size();
    }

private:
    struct Retired {
        vk::Buffer gathered;
        u64 tick{};
    };

    struct Entry {
        vk::Buffer gathered;
        std::vector<VkBuffer> owners;
        VkDeviceSize size{};
        u64 geometry{};
        u64 gather_tick{};
        bool dirty{true};
    };

    [[nodiscard]] static u64 HashSources(std::span<const MultiRangeSource> sources);

    void RetireEntry(Scheduler& scheduler, Entry& entry);

    void DrainRetired(Scheduler& scheduler);

    ankerl::unordered_dense::map<u64, Entry> entries;
    std::vector<Retired> retired;
};

} // namespace Vulkan

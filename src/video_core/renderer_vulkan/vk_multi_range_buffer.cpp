// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>

#include "video_core/renderer_vulkan/vk_multi_range_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

u64 MultiRangeBufferCache::HashSources(std::span<const MultiRangeSource> sources) {
    u64 hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](u64 value) {
        hash ^= value;
        hash *= 0x100000001b3ULL;
    };
    for (const MultiRangeSource& source : sources) {
        mix(u64(source.handle));
        mix(u64(source.offset));
        mix(u64(source.size));
    }
    return hash;
}

void MultiRangeBufferCache::RetireEntry(Scheduler& scheduler, Entry& entry) {
    if (!entry.gathered) {
        return;
    }
    // The GPU may still read the gathered buffer: keep it until the current tick is done.
    retired.push_back(Retired{
        .gathered = std::move(entry.gathered),
        .tick = scheduler.CurrentTick(),
    });
}

void MultiRangeBufferCache::DrainRetired(Scheduler& scheduler) {
    std::erase_if(retired, [&scheduler](const Retired& item) {
        return scheduler.IsFree(item.tick);
    });
}

MultiRangeRef MultiRangeBufferCache::Get(const Device& device, Scheduler& scheduler,
                                         MemoryAllocator& memory_allocator, u64 key,
                                         std::span<const MultiRangeSource> sources,
                                         VkDeviceSize total) {
    if (sources.empty() || total == 0 || total > MAX_GATHER_SIZE) {
        return MultiRangeRef{};
    }
    if (!retired.empty()) {
        DrainRetired(scheduler);
    }
    const u64 geometry = HashSources(sources);
    const auto it = entries.find(key);
    if (it != entries.end() && it->second.geometry == geometry && it->second.size == total &&
        it->second.gathered) {
        Entry& entry = it->second;
        // Eden hashes the source write ticks; a source written later in the same (not yet
        // submitted) tick keeps the same tick value, so compare against the gather tick instead.
        const bool written_since = std::any_of(
            sources.begin(), sources.end(), [tick = entry.gather_tick](const auto& source) {
                return source.write_tick >= tick;
            });
        return MultiRangeRef{
            .handle = *entry.gathered,
            .size = entry.size,
            .needs_gather = entry.dirty || written_since,
        };
    }
    if (it != entries.end()) {
        RetireEntry(scheduler, it->second);
        entries.erase(it);
    }
    if (entries.size() >= MAX_ENTRIES) {
        for (auto& item : entries) {
            RetireEntry(scheduler, item.second);
        }
        entries.clear();
    }

    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (device.IsDescriptorBufferEnabled()) {
        // Descriptor buffers address storage buffers by device address.
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkBufferCreateInfo gather_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = total,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    Entry entry{};
    entry.gathered = memory_allocator.CreateBuffer(gather_ci, MemoryUsage::DeviceLocal);
    entry.geometry = geometry;
    entry.size = total;
    entry.dirty = true;
    entry.owners.reserve(sources.size());
    for (const MultiRangeSource& source : sources) {
        entry.owners.push_back(source.handle);
    }
    const MultiRangeRef ref{
        .handle = *entry.gathered,
        .size = entry.size,
        .needs_gather = true,
    };
    entries.emplace(key, std::move(entry));
    return ref;
}

void MultiRangeBufferCache::MarkGathered(Scheduler& scheduler, u64 key) {
    if (const auto it = entries.find(key); it != entries.end()) {
        it->second.dirty = false;
        it->second.gather_tick = scheduler.CurrentTick();
    }
}

void MultiRangeBufferCache::Invalidate(u64 key) {
    if (const auto it = entries.find(key); it != entries.end()) {
        it->second.dirty = true;
    }
}

void MultiRangeBufferCache::DropOwner(Scheduler& scheduler, VkBuffer owner) {
    if (owner == VK_NULL_HANDLE || entries.empty()) {
        return;
    }
    for (auto it = entries.begin(); it != entries.end();) {
        Entry& entry = it->second;
        if (std::find(entry.owners.begin(), entry.owners.end(), owner) == entry.owners.end()) {
            ++it;
            continue;
        }
        RetireEntry(scheduler, entry);
        it = entries.erase(it);
    }
}

} // namespace Vulkan

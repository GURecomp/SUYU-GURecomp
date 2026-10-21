// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

DescriptorBufferRing::DescriptorBufferRing(const Device& device_,
                                           MemoryAllocator& memory_allocator)
    : device{device_} {
    if (!device.IsDescriptorBufferEnabled()) {
        return;
    }
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& props{device.DescriptorBufferProperties()};
    alignment = std::max<VkDeviceSize>(props.descriptorBufferOffsetAlignment, 1);

    const VkDeviceSize max_bound{(std::min)({props.maxSamplerDescriptorBufferRange,
                                             props.maxResourceDescriptorBufferRange,
                                             props.samplerDescriptorBufferAddressSpaceSize,
                                             props.resourceDescriptorBufferAddressSpaceSize,
                                             props.descriptorBufferAddressSpaceSize})};
    const VkDeviceSize chunk_size{Common::AlignDown((std::min)(FRAME_SIZE, max_bound), alignment)};
    if (chunk_size <= alignment) {
        LOG_INFO(Render_Vulkan, "Descriptor buffers: binding limit of {} is unusable, not used",
                 max_bound);
        return;
    }
    chunk_capacity = chunk_size - alignment;
    chunks_per_frame = static_cast<size_t>(FRAME_SIZE / chunk_size);

    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = chunk_size,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    const size_t total_chunks{chunks_per_frame * FRAMES_IN_FLIGHT};
    chunks.reserve(total_chunks);
    chunk_addresses.reserve(total_chunks);
    chunk_hosts.reserve(total_chunks);
    for (size_t index = 0; index < total_chunks; ++index) {
        vk::Buffer buffer{memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Upload)};
        if (!buffer.IsHostVisible() || !buffer.IsHostCoherent()) {
            LOG_INFO(Render_Vulkan,
                     "Descriptor buffers: ring memory is not host visible and coherent, not used");
            chunks.clear();
            chunk_addresses.clear();
            chunk_hosts.clear();
            return;
        }
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Descriptor buffer");
        }
        const VkDeviceAddress raw_address{device.GetLogical().GetBufferDeviceAddress(*buffer)};
        const VkDeviceAddress address{Common::AlignUp(raw_address, alignment)};
        chunk_addresses.push_back(address);
        chunk_hosts.push_back(buffer.Mapped().data() + (address - raw_address));
        chunks.push_back(std::move(buffer));
    }
    LOG_INFO(Render_Vulkan,
             "Descriptor buffers: ring of {} x {} KiB chunks ({} per frame), alignment {}",
             total_chunks, chunk_size / 1024, chunks_per_frame, alignment);
}

DescriptorBufferRing::~DescriptorBufferRing() = default;

void DescriptorBufferRing::TickFrame() {
    if (chunks.empty()) {
        return;
    }
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    chunk_cursor = 0;
    cursor = 0;
    ++generation;
    frame_reused = true;
}

void DescriptorBufferRing::TouchFrame(Scheduler& scheduler) {
    frame_ticks[frame_index] = scheduler.CurrentTick();
}

DescriptorBufferRing::Allocation DescriptorBufferRing::Allocate(Scheduler& scheduler,
                                                                VkDeviceSize size) {
    ASSERT(!chunks.empty());
    if (!CanAllocate(size)) {
        return Allocation{};
    }
    const VkDeviceSize needed{Common::AlignUp(size, alignment)};
    if (frame_reused) {
        // First use of this frame slot since it was last used: wait until the GPU is done with it.
        frame_reused = false;
        scheduler.Wait(frame_ticks[frame_index]);
    }
    if (cursor + needed > chunk_capacity) {
        if (chunk_cursor + 1 < chunks_per_frame) {
            ++chunk_cursor;
        } else {
            ++stall_count;
            if (stall_count <= 20 || stall_count % 1000 == 0) {
                LOG_INFO(Render_Vulkan,
                         "Descriptor buffers: frame memory exhausted, waiting for the GPU (n={})",
                         stall_count);
            }
            scheduler.Finish();
            chunk_cursor = 0;
            ++generation;
        }
        cursor = 0;
    }
    const size_t chunk{frame_index * chunks_per_frame + chunk_cursor};
    const VkDeviceSize offset{cursor};
    cursor += needed;
    frame_ticks[frame_index] = scheduler.CurrentTick();
    return Allocation{
        .host = chunk_hosts[chunk] + offset,
        .offset = offset,
        .chunk = static_cast<u32>(chunk),
        .generation = generation,
    };
}

VkDescriptorBufferBindingInfoEXT DescriptorBufferRing::BindingInfo(u32 chunk) const noexcept {
    return VkDescriptorBufferBindingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .pNext = nullptr,
        .address = chunk_addresses[chunk],
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
    };
}

} // namespace Vulkan

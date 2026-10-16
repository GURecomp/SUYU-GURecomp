// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <memory>
#include <thread>
#include "core/arm/recomp/game_settings.h"

#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_fence_manager.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

InnerFence::InnerFence(Scheduler& scheduler_, bool is_stubbed_)
    : FenceBase{is_stubbed_}, scheduler{scheduler_} {}

InnerFence::~InnerFence() = default;

void InnerFence::Queue() {
    if (is_stubbed) {
        return;
    }
    // Get the current tick so we can wait for it
    wait_tick = scheduler.CurrentTick();
    scheduler.Flush();
}

bool InnerFence::IsSignaled() const {
    if (is_stubbed) {
        return true;
    }
    return scheduler.IsFree(wait_tick);
}

void InnerFence::Wait() {
    if (is_stubbed) {
        return;
    }
    scheduler.Wait(wait_tick);
}

FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                           TextureCache& texture_cache_, BufferCache& buffer_cache_,
                           QueryCache& query_cache_, const Device& device_, Scheduler& scheduler_)
    : GenericFenceManager{rasterizer_, gpu_, texture_cache_, buffer_cache_, query_cache_},
      scheduler{scheduler_} {}

Fence FenceManager::CreateFence(bool is_stubbed) {
    return std::make_shared<InnerFence>(scheduler, is_stubbed);
}

void FenceManager::QueueFence(Fence& fence) {
    fence->Queue();
}

bool FenceManager::IsFenceSignaled(Fence& fence) const {
    return fence->IsSignaled();
}

void InnerFence::WaitSleeping() {
    if (is_stubbed) {
        return;
    }
    for (;;) {
        scheduler.RefreshGpuTick();
        if (scheduler.IsFree(wait_tick)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
}

void FenceManager::WaitFence(Fence& fence) {
    fence->Wait();
}

void FenceManager::WaitFenceSlow(Fence& fence) {
    if (Core::GameSettings::FenceThreadSleep()) {
        fence->WaitSleeping();
    } else {
        fence->Wait();
    }
}

} // namespace Vulkan

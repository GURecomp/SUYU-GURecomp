// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// [Debug] nvn_fsr_backend = 4: FSR 4 for the NVN replacement renderer through the AMD driver's own DX12 ffx-api loader
// (System32 amd_fidelityfx_dx12.dll, loaded at run time; nothing of AMD's is shipped). A D3D12 device on the same adapter
// (LUID of suyu's Vulkan device) owns five shared committed resources (colour, depth, motion vectors, reactive, output)
// that are imported into Vulkan, and one shared fence that is imported as a Vulkan timeline semaphore. The renderer copies
// its existing FSR inputs into the shared images, submits (signal N), the bridge runs ffxDispatch on its D3D12 queue
// (wait N, signal N + 1) and the renderer's next submit waits on N + 1 and copies the output back. See
// FSR4-BRIDGE-DESIGN.md. Headers: FidelityFX SDK v1.1.4 ffx-api (MIT, externals/ffx-api).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan::NvnFsr4 {

struct VkContext {
    VkDevice device{};
    VkPhysicalDevice physical{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    PFN_vkGetPhysicalDeviceProperties2 get_physical_properties2{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_memory_properties{};
    uint32_t ring{4}; // D3D12 command allocators / timestamp pairs (>= the renderer's frames in flight)
    bool debug_checking{}; // FFX_UPSCALE_ENABLE_DEBUG_CHECKING ([Debug] nvn_fsr_debug)
};

enum Slot : uint32_t { kColor = 0, kDepth, kMotion, kReactive, kOutput, kSlotCount };

struct SharedImage {
    VkImage image{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{}, height{};
};

struct DispatchParams {
    bool reactive{};          // bind the reactive image (else none)
    float jitter_x{}, jitter_y{};
    uint32_t render_w{}, render_h{}, output_w{}, output_h{};
    float frame_ms{16.7f};
    float camera_near{}, camera_far{}, fov_y{}, view_to_meters{1.0f};
    bool sharpen{};
    float sharpness{};
    bool reset{};
};

class Bridge {
public:
    using Log = std::function<void(const std::string&)>;

    Bridge();
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    /// DLL, D3D12 device on the Vulkan device's LUID, provider list (highest FSR 4.x), shared fence -> timeline semaphore.
    /// Logs every step. False = FSR 4 cannot be used in this run (reason in LastError()).
    bool Init(const VkContext& vk, Log log);
    bool Initialized() const;

    /// Shared images + ffx context for these sizes / formats (recreated when any differs; the caller must have made Vulkan
    /// and the bridge idle first: see NeedsRecreate / WaitIdle). False = failure (LastError()).
    bool NeedsRecreate(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkFormat color, VkFormat motion) const;
    bool Ensure(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkFormat color, VkFormat motion);
    bool ContextReady() const;
    const SharedImage& Image(Slot s) const;
    /// True once after (re)creation: the Vulkan images are still in VK_IMAGE_LAYOUT_UNDEFINED.
    bool TakeFreshImages();

    VkSemaphore Timeline() const;
    /// Next timeline value to signal (monotonic).
    uint64_t NextValue();

    /// D3D12 side of one frame: Wait(wait_value) (the renderer's submit signals it), ffxDispatch, Signal(signal_value).
    /// False = failure (LastError()); when it returns false, signal_value may never be signalled: do not wait on it.
    bool Dispatch(const DispatchParams& p, uint64_t wait_value, uint64_t signal_value);

    /// Waits up to timeout_ms for every value signalled so far. False = timed out / device removed.
    bool WaitIdle(uint32_t timeout_ms);
    /// After a failure: sets the fence from the CPU to the highest value handed out so no Vulkan wait can hang on it.
    void ReleaseWaiters();
    /// Destroys the ffx context and the shared images (Vulkan and D3D12 must be idle). Init state stays.
    void DestroyContext();
    /// Everything (call with Vulkan idle).
    void Shutdown();

    /// The provider's jitter phase count for these widths (0 = query failed) and offsets (ffxQuery on the context).
    int32_t JitterPhaseCount(uint32_t render_w, uint32_t output_w) const;
    bool JitterOffset(int32_t index, int32_t phase_count, float* x, float* y) const;

    /// "FSR 4.1.1" style name of the chosen provider and the whole provider list for the log.
    const std::string& VersionName() const;
    const std::string& ProviderList() const;
    const std::string& LastError() const;

    /// D3D12 GPU time (timestamps around ffxDispatch) of the dispatches that finished since the last call.
    void TakeGpuStats(double& sum_ms, uint32_t& count, double& max_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan::NvnFsr4

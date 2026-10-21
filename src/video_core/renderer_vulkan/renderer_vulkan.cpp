// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "core/arm/recomp/game_settings.h"
#include "video_core/renderer_vulkan/nvn_vk.h"
#include "video_core/renderer_vulkan/emu_bench.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue() || Core::GameSettings::VulkanValidation()))
    // Create debug messenger if debug is enabled
    , debug_messenger((Settings::values.renderer_debug || Core::GameSettings::VulkanValidation()) ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler) {

    is_headless = (render_window.GetWindowInfo().type == Core::Frontend::WindowSystemType::Headless);

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

    Report();
    NvnVkStart(device, memory_allocator);
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    NvnVkStop();
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    if (is_headless) {
        static unsigned headless_composite_count = 0;
        ++headless_composite_count;
        if (headless_composite_count <= 5 || (headless_composite_count % 60) == 0) {
            LOG_INFO(Render_Vulkan, "Headless Composite #{}, {} framebuffer layers",
                     headless_composite_count, framebuffers.size());
            fprintf(stderr, "[suyu-vulkan] Headless Composite #%u, %zu layers\n",
                    headless_composite_count, framebuffers.size());
            fflush(stderr);
        }

        RenderScreenshot(framebuffers);

        if (!framebuffers.empty()) {
            const Layout::FramebufferLayout layout{render_window.GetFramebufferLayout()};
            headless_width = layout.width;
            headless_height = layout.height;
            const VkDeviceSize buffer_size = headless_width * headless_height * 4;

            auto dst_buffer = RenderToBuffer(framebuffers, layout,
                                             VK_FORMAT_B8G8R8A8_UNORM, buffer_size);
            headless_frame_data.resize(buffer_size);
            std::memcpy(headless_frame_data.data(), dst_buffer.Mapped().data(), buffer_size);
        }

        gpu.RendererFrameEndNotify();
        rasterizer.TickFrame();
        return;
    }

    if (!render_window.IsShown()) {
        return;
    }

    EmuBenchFrame();
    if (Core::GameSettings::NvnVulkanMode() != 0) {
        NvnVkSuyuFrameTick();
        static bool capture_was = false;
        if (capture_was && !NvnVkCaptureActive()) rasterizer.DumpCompareImages();
        capture_was = NvnVkCaptureActive();
        static u32 nvn_compare_frame = 0;
        ++nvn_compare_frame;
        if ((nvn_compare_frame == 600 && Core::GameSettings::GpuDiagnosticsEnabled()) || NvnVkCaptureActive()) {
            // Diagnostics: suyu's own picture of this frame, to compare with the NVN renderer's.
            const Layout::FramebufferLayout layout{render_window.GetFramebufferLayout()};
            const auto buf = RenderToBuffer(framebuffers, layout, VK_FORMAT_B8G8R8A8_UNORM, layout.width * layout.height * 4);
            std::vector<u8> rgba(buf.Mapped().begin(), buf.Mapped().end());
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
            NvnVkSavePng(fmt::format("nvn_suyu_frame_{}.png", NvnVkCaptureActive() ? 1800u : nvn_compare_frame).c_str(), layout.width, layout.height, rgba.data());
        }
    }
    RenderScreenshot(framebuffers);
    NvnVkCompositeProbe(0);
    Frame* frame = present_manager.GetRenderFrame();

    scheduler.RequestOutsideRenderPassOperationContext();
    // Present mode: when our renderer has a frame, suyu's own picture (the guest framebuffer read back and unswizzled on
    // the CPU, about 70% of the GPU thread) is not needed: draw the window with no layers and blit ours over it.
    VkImage src{};
    unsigned sw = 0, sh = 0;
    const bool have_nvn = Core::GameSettings::NvnVulkanMode() == 3 && !NvnVkCaptureActive() && NvnVkPresentImage(src, sw, sh) && sw && sh;
    if (Core::GameSettings::GpuDiagnosticsEnabled() && Core::GameSettings::NvnVulkanMode() == 3) {
        static u32 n_yes = 0, n_no = 0, n_cap = 0;
        ++(have_nvn ? n_yes : n_no);
        n_cap += NvnVkCaptureActive();
        if ((n_yes + n_no) % 300 == 0) {
            NvnVkDumpLine("nvn_composite.txt", fmt::format("composite frames {} without suyu layers, {} with them (unswizzle), capture active {}", n_yes, n_no, n_cap));
        }
    }
    blit_swapchain.DrawToFrame(device, rasterizer, frame, have_nvn ?std::span<const Tegra::FramebufferConfig>{} : framebuffers,
                               render_window.GetFramebufferLayout(), swapchain.GetImageCount(),
                               swapchain.GetImageViewFormat());
    if (Core::GameSettings::NvnVulkanMode() == 3) {
        // Present mode: replace suyu's picture with the frame our NVN renderer drew, centred with its own aspect.
        if (have_nvn || (NvnVkPresentImage(src, sw, sh) && sw && sh)) {
            const VkImage dst = *frame->image;
            const u32 dw = frame->width, dh = frame->height;
            // The guest renders at console size (16:9); the window layout (aspect_ratio setting: Stretch / 21:9 / ...) decides
            // the shape it is shown in, so a wide window is filled instead of pillarboxed.
            const auto& screen = render_window.GetFramebufferLayout().screen;
            s32 x0 = static_cast<s32>(screen.left), y0 = static_cast<s32>(screen.top);
            s32 w = static_cast<s32>(screen.GetWidth()), h = static_cast<s32>(screen.GetHeight());
            if (w <= 0 || h <= 0 || x0 + w > static_cast<s32>(dw) || y0 + h > static_cast<s32>(dh)) {
                const f64 scale = std::min(static_cast<f64>(dw) / sw, static_cast<f64>(dh) / sh);
                w = std::max<s32>(1, static_cast<s32>(sw * scale)), h = std::max<s32>(1, static_cast<s32>(sh * scale));
                x0 = (static_cast<s32>(dw) - w) / 2, y0 = (static_cast<s32>(dh) - h) / 2;
            }
            scheduler.Record([=](vk::CommandBuffer cmdbuf) {
                const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, before);
                const VkImageBlit blit{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       {{0, 0, 0}, {static_cast<s32>(sw), static_cast<s32>(sh), 1}},
                                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       {{x0, y0, 0}, {x0 + w, y0 + h, 1}}};
                cmdbuf.BlitImage(src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, blit, VK_FILTER_LINEAR);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, before);
            });
            // present_nodrain: the read of src ends when this chunk's tick completes (signalled by the Flush below).
            NvnVkPresentImageUsed(scheduler.GetMasterSemaphore(), scheduler.CurrentTick());
        }
    }
    scheduler.Flush(*frame->render_ready);

    present_manager.Present(frame);
    NvnVkCompositeProbe(1);

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(framebuffers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, framebuffers, VideoCore::Capture::Layout, 1,
                            CaptureFormat);
}

} // namespace Vulkan

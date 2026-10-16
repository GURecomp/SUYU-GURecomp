// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <chrono>
#include <cstring>
#pragma comment(lib, "dbghelp.lib")
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#endif
#include "common/assert.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/nvn_vk.h"
#include "video_core/renderer_vulkan/nvn_vk.h"

namespace VideoCommon::GPUThread {

#ifdef _WIN32
namespace {
// [Debug] user\gpu_sample exists: after 20 s, sample this thread's instruction pointer and call sites for 10 s and
// write the hottest functions to user\gpu_sample.txt (names from the build's pdb).
void RunGpuSampler(HANDLE target) {
    std::error_code ec;
    const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir);
    if (!std::filesystem::exists(dir / "gpu_sample", ec)) return;
    std::this_thread::sleep_for(std::chrono::seconds(20));
    const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(mod + reinterpret_cast<IMAGE_DOS_HEADER*>(mod)->e_lfanew);
    const uintptr_t mod_end = mod + nt->OptionalHeader.SizeOfImage;
    std::map<uintptr_t, u32> leaf, incl;
    u32 samples = 0;
    const auto t_end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < t_end) {
        if (SuspendThread(target) == static_cast<DWORD>(-1)) break;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        uintptr_t words[512];
        bool ok = GetThreadContext(target, &ctx) != 0;
        if (ok) {
            std::memcpy(words, reinterpret_cast<void*>(ctx.Rsp), sizeof(words)); // own process, stack of a suspended thread
        }
        ResumeThread(target);
        if (ok) {
            ++samples;
            ++leaf[ctx.Rip];
            std::vector<uintptr_t> seen;
            for (uintptr_t w : words) {
                if (w > mod + 8 && w < mod_end) {
                    const auto* p = reinterpret_cast<const unsigned char*>(w);
                    if (p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF) {
                        if (std::find(seen.begin(), seen.end(), w) == seen.end()) seen.push_back(w);
                    }
                }
            }
            for (uintptr_t w : seen) ++incl[w];
        }
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    const HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, "E:/GUU test/suyu-gpu-wt/build/bin", TRUE);
    const auto name_of = [&](uintptr_t a) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 250;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, a, &disp, sym)) return std::string(sym->Name);
        return std::string("?");
    };
    const auto dump = [&](std::ofstream& out, const char* title, const std::map<uintptr_t, u32>& m) {
        std::map<std::string, u32> by;
        for (const auto& [a, c] : m) by[name_of(a)] += c;
        std::vector<std::pair<u32, std::string>> v;
        for (auto& [n, c] : by) v.emplace_back(c, n);
        std::sort(v.rbegin(), v.rend());
        out << title << " (samples " << samples << ")\n";
        for (size_t i = 0; i < v.size() && i < 45; ++i) out << "  " << (100.0 * v[i].first / std::max(1u, samples)) << "% " << v[i].second << "\n";
    };
    std::ofstream out(dir / "gpu_sample.txt");
    dump(out, "leaf (where the GPU thread was executing)", leaf);
    {
        std::vector<std::pair<u32, uintptr_t>> raw;
        for (const auto& [a, c] : leaf) raw.emplace_back(c, a);
        std::sort(raw.rbegin(), raw.rend());
        out << "raw leaf addresses (module offset)\n";
        for (size_t i = 0; i < raw.size() && i < 12; ++i) out << "  " << std::hex << (raw[i].second - mod) << std::dec << " x" << raw[i].first << "\n";
    }
    dump(out, "inclusive (call sites found on the stack)", incl);
}
} // namespace
#endif

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPU");
#ifdef _WIN32
        {
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            std::thread([self] { RunGpuSampler(self); }).detach();
        }
#endif
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        while (!stop_token.stop_requested()) {
            state.queue.PopWait(next, stop_token);
            if (stop_token.stop_requested()) {
                break;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                Vulkan::NvnVkWaitRenderIdle();
                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
            } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                system.GPU().TickWork();
            } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
            } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
            } else {
                ASSERT(false);
            }
            state.signaled_fence.store(next.fence);
            if (next.block) {
                // We have to lock the write_lock to ensure that the condition_variable wait not get a
                // race between the check and the lock itself.
                std::scoped_lock lk{state.write_lock};
                state.cv.notify_all();
            }
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    state.queue.EmplaceWait(std::move(command_data), fence, block);

    if (block) {
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

} // namespace VideoCommon::GPUThread

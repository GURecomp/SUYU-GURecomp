// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/mod_host.h"
#include "core/arm/recomp/recomp_hooks.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_controller.h"

namespace Core::ModHost {

std::atomic<bool> g_active{false};
u32 g_hook_lo{};
u32 g_hook_span{};
const volatile u8* g_hook_bits{};

namespace {

constexpr const char* kModuleAliases[] = {"rtld", "main", "subsdk0", "sdk"}; // load order
constexpr const char* kDefaultLoader = "Forge/forge.dll";

System* g_system{};
RecompLookupFn g_lookup{};
thread_local const GuestCaller* t_caller{};

std::string g_title_id;
std::string g_game_version;
std::string g_exe_dir;
std::string g_user_dir;
std::string g_mods_dir;
std::string g_loader_dir;

struct Module {
    std::string name;
    std::string alias;
    u32 base;
    u32 span;
};
std::vector<Module> g_modules;

ModHostApi g_api{};
ModHostCallbacks g_cb{};
std::atomic<bool> g_attached{false}; // g_cb is filled in
std::atomic<bool> g_input_captured{false};
std::atomic<bool> g_text_input{false};

// Guest memory for mods: one region mapped at Start, handed out first-fit.
constexpr u32 kGuestHeapSizes[] = {32u << 20, 16u << 20, 8u << 20}; // tried in order
constexpr u32 kGuestMinAlign = 16;
std::mutex g_heap_lock;
u32 g_heap_start{};
u32 g_heap_size{};
std::map<u32, u32> g_heap_free;                    // addr -> size, coalesced
std::map<u32, std::pair<u32, u32>> g_heap_blocks; // addr handed out -> block start, block size

// --- ModHostApi -------------------------------------------------------------------------------

void ApiLog(ModHostLogLevel level, const char* message) {
    const char* m = message ? message : "";
    switch (level) {
    case MODHOST_LOG_DEBUG:
        LOG_DEBUG(Core_ARM, "[mods] {}", m);
        break;
    case MODHOST_LOG_INFO:
        LOG_INFO(Core_ARM, "[mods] {}", m);
        break;
    case MODHOST_LOG_WARN:
        LOG_WARNING(Core_ARM, "[mods] {}", m);
        break;
    default:
        LOG_ERROR(Core_ARM, "[mods] {}", m);
        break;
    }
}

u32 ApiIniGet(const char* section, const char* key, char* out, u32 out_size) {
    const std::string v = section && key ? GameSettings::Value(section, key) : std::string{};
    if (out && out_size) {
        const size_t n = std::min<size_t>(v.size(), out_size - 1);
        std::memcpy(out, v.data(), n);
        out[n] = '\0';
    }
    return static_cast<u32>(v.size());
}

u32 ApiModuleCount() {
    return static_cast<u32>(g_modules.size());
}

bool ApiModuleGet(u32 index, ModHostModule* out) {
    if (!out || index >= g_modules.size() || out->size < sizeof(u32) * 2) {
        return false;
    }
    ModHostModule m{};
    m.size = sizeof(ModHostModule);
    m.version = MODHOST_API_VERSION;
    m.name = g_modules[index].name.c_str();
    m.alias = g_modules[index].alias.c_str();
    m.base = g_modules[index].base;
    m.span = g_modules[index].span;
    // text/rodata/data bounds: 0 = unknown (filled once FindModules reports segments)
    const u32 caller_size = out->size;
    std::memcpy(out, &m, std::min<size_t>(caller_size, sizeof(m)));
    out->size = std::min<u32>(caller_size, sizeof(m));
    return true;
}

Memory::Memory& Mem() {
    return g_system->ApplicationMemory();
}

bool ApiMemValid(u32 addr, u32 size) {
    return size == 0 || Mem().IsValidVirtualAddressRange(addr, size);
}

bool ApiMemRead(u32 addr, void* out, u32 size) {
    return out && ApiMemValid(addr, size) && Mem().ReadBlock(addr, out, size);
}

bool ApiMemWrite(u32 addr, const void* data, u32 size) {
    return data && ApiMemValid(addr, size) && Mem().WriteBlock(addr, data, size);
}

void* ApiMemPtr(u32 addr, u32 size) {
    if (size == 0 || !ApiMemValid(addr, size)) {
        return nullptr;
    }
    u8* first = Mem().GetPointerSilent(addr);
    u8* last = Mem().GetPointerSilent(addr + size - 1);
    return first && last == first + (size - 1) ? first : nullptr;
}

bool ApiIsBlock(u32 pc) {
    return (pc & 3) == 0 && pc - g_hook_lo < g_hook_span && g_lookup && g_lookup(pc) != nullptr;
}

void ApiMark(u32 pc) {
    RecompHooks::Mark(pc); // chained branches now return to the dispatcher there
}

bool ApiCall(u32 function, const u32* args, u32 count, const float* fargs, u32 fcount,
             u32 out_r[2], double* out_d0) {
    const GuestCaller* caller = t_caller;
    if (!caller || (count && !args) || (fcount && !fargs) || fcount > 16 || count > 32) {
        return false;
    }
    u32 r[2]{};
    double d0{};
    if (!caller->call(caller->impl, function, args, count, fargs, fcount, r, &d0)) {
        return false;
    }
    if (out_r) {
        out_r[0] = r[0];
        out_r[1] = r[1];
    }
    if (out_d0) {
        *out_d0 = d0;
    }
    return true;
}

void ApiSetInputCaptured(bool captured) {
    g_input_captured.store(captured);
    // Controllers too: a pad driving the mod's menu shouldn't also move the game.
    Core::HID::EmulatedController::SetInputBlocked(Core::HID::EmulatedController::BlockerMods,
                                                   captured);
}

void ApiSetTextInput(bool enabled) {
    g_text_input.store(enabled);
}

u32 ApiGuestAlloc(u32 size, u32 align) {
    align = std::max(align, kGuestMinAlign);
    if (size == 0 || (align & (align - 1)) != 0 || align > (1u << 20) || size > g_heap_size) {
        return 0;
    }
    size = (size + kGuestMinAlign - 1) & ~(kGuestMinAlign - 1);
    std::scoped_lock lk{g_heap_lock};
    for (auto it = g_heap_free.begin(); it != g_heap_free.end(); ++it) {
        const u32 start = it->first;
        const u32 avail = it->second;
        const u32 addr = (start + align - 1) & ~(align - 1);
        if (addr - start > avail || avail - (addr - start) < size) {
            continue;
        }
        // The block runs from the free range's start (alignment padding included) to addr + size.
        const u32 block = addr - start + size;
        g_heap_free.erase(it);
        if (avail > block) {
            g_heap_free.emplace(start + block, avail - block);
        }
        g_heap_blocks.emplace(addr, std::make_pair(start, block));
        Mem().ZeroBlock(addr, size);
        return addr;
    }
    return 0;
}

void ApiGuestFree(u32 addr) {
    if (addr == 0) {
        return;
    }
    std::scoped_lock lk{g_heap_lock};
    const auto used = g_heap_blocks.find(addr);
    if (used == g_heap_blocks.end()) {
        LOG_WARNING(Core_ARM, "[mods] guest_free({:#x}): not an allocation", addr);
        return;
    }
    u32 start = used->second.first;
    u32 size = used->second.second;
    g_heap_blocks.erase(used);
    // Merge with the free ranges on either side.
    auto next = g_heap_free.lower_bound(start);
    if (next != g_heap_free.end() && start + size == next->first) {
        size += next->second;
        next = g_heap_free.erase(next);
    }
    if (next != g_heap_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            size += prev->second;
            g_heap_free.erase(prev);
        }
    }
    g_heap_free.emplace(start, size);
}

// Maps the mods' guest region (on the emulated CPU thread, before the game's first instruction).
// It sits in the stack region like the main thread's stack: the game finds free space there by
// querying, so it never places anything on top of it.
void MapGuestHeap() {
    auto* process = g_system->ApplicationProcess();
    if (!process) {
        return;
    }
    for (const u32 bytes : kGuestHeapSizes) {
        Kernel::KProcessAddress addr{};
        const Result rc = process->GetPageTable().MapPages(
            std::addressof(addr), bytes / Kernel::PageSize, Kernel::KMemoryState::Stack,
            Kernel::KMemoryPermission::UserReadWrite);
        if (rc.IsSuccess()) {
            g_heap_start = static_cast<u32>(GetInteger(addr));
            g_heap_size = bytes;
            g_heap_free.emplace(g_heap_start, g_heap_size);
            LOG_INFO(Core_ARM, "[mods] guest memory for mods: {:#x}-{:#x} ({} MB)", g_heap_start,
                     g_heap_start + g_heap_size, g_heap_size >> 20);
            return;
        }
    }
    LOG_WARNING(Core_ARM, "[mods] no guest memory for mods (guest_alloc will return 0)");
}

// --- loading ----------------------------------------------------------------------------------

void LoadLoader() {
    std::string rel = GameSettings::Value("Mods", "loader");
    if (rel.empty()) {
        rel = kDefaultLoader;
    }
    if (rel == "none") {
        LOG_INFO(Core_ARM, "[mods] no loader (game_settings.ini [Mods] loader = none)");
        return;
    }
    const auto path = std::filesystem::path{Common::FS::ToU8String(g_mods_dir)} /
                      std::filesystem::path{Common::FS::ToU8String(rel)};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        LOG_INFO(Core_ARM, "[mods] no loader at {} (mods with plugins need Forge PC there)",
                 Common::FS::PathToUTF8String(path));
        return;
    }
    g_loader_dir = Common::FS::PathToUTF8String(path.parent_path());
#ifdef _WIN32
    // The loader's own folder first for its dependencies.
    HMODULE lib = LoadLibraryExW(path.wstring().c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!lib) {
        LOG_ERROR(Core_ARM, "[mods] {} could not be loaded (error {})",
                  Common::FS::PathToUTF8String(path), GetLastError());
        return;
    }
    const auto attach =
        reinterpret_cast<ModHostAttachFn>(GetProcAddress(lib, "modhost_attach"));
#else
    void* lib = dlopen(path.c_str(), RTLD_NOW);
    if (!lib) {
        LOG_ERROR(Core_ARM, "[mods] {} could not be loaded ({})", path.string(), dlerror());
        return;
    }
    const auto attach = reinterpret_cast<ModHostAttachFn>(dlsym(lib, "modhost_attach"));
#endif
    if (!attach) {
        LOG_ERROR(Core_ARM, "[mods] {} has no modhost_attach", Common::FS::PathToUTF8String(path));
        return;
    }
    MapGuestHeap();
    g_api = ModHostApi{
        .size = sizeof(ModHostApi),
        .version = MODHOST_API_VERSION,
        .game_title_id = g_title_id.c_str(),
        .game_version = g_game_version.c_str(),
        .exe_dir = g_exe_dir.c_str(),
        .user_dir = g_user_dir.c_str(),
        .mods_dir = g_mods_dir.c_str(),
        .loader_dir = g_loader_dir.c_str(),
        .log = ApiLog,
        .ini_get = ApiIniGet,
        .module_count = ApiModuleCount,
        .module_get = ApiModuleGet,
        .mem_valid = ApiMemValid,
        .mem_read = ApiMemRead,
        .mem_write = ApiMemWrite,
        .mem_ptr = ApiMemPtr,
        .is_block = ApiIsBlock,
        .mark = ApiMark,
        .call = ApiCall,
        .set_input_captured = ApiSetInputCaptured,
        .set_text_input = ApiSetTextInput,
        .guest_alloc = ApiGuestAlloc,
        .guest_free = ApiGuestFree,
        .guest_heap_start = g_heap_start,
        .guest_heap_size = g_heap_size,
    };
    ModHostCallbacks cb{};
    cb.size = sizeof(ModHostCallbacks);
    if (!attach(&g_api, &cb)) {
        LOG_WARNING(Core_ARM, "[mods] {} declined to attach", Common::FS::PathToUTF8String(path));
        return;
    }
    cb.size = sizeof(ModHostCallbacks);
    g_cb = cb;
    g_attached.store(true, std::memory_order_release);
    g_active.store(g_cb.on_dispatch != nullptr);
    LOG_INFO(Core_ARM, "[mods] loader {} attached (loader ABI {}, exe ABI {})",
             Common::FS::PathToUTF8String(path), g_cb.version, MODHOST_API_VERSION);
}

} // namespace

void Start(System& system, const std::map<u64, std::string>& modules, RecompLookupFn lookup) {
    static std::once_flag once;
    std::call_once(once, [&] {
        g_system = &system;
        g_lookup = lookup;
        g_hook_lo = RecompHooks::Lo();
        g_hook_span = RecompHooks::Span();
        g_hook_bits = RecompHooks::Bits();
        if (!g_hook_bits || !lookup) {
            LOG_ERROR(Core_ARM, "[mods] no hook range / block lookup: mods disabled");
            return;
        }
        g_title_id = fmt::format("{:016X}", system.GetApplicationProcessProgramID());
        const auto user_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir);
        g_user_dir = Common::FS::PathToUTF8String(user_dir);
        g_exe_dir = Common::FS::PathToUTF8String(user_dir.parent_path());
        g_mods_dir = Common::FS::PathToUTF8String(
            Common::FS::GetSuyuPath(Common::FS::SuyuPath::LoadDir) / g_title_id);

        size_t index = 0;
        for (auto it = modules.begin(); it != modules.end(); ++it, ++index) {
            const auto next = std::next(it);
            const u32 base = static_cast<u32>(it->first);
            const u32 end =
                next != modules.end() ? static_cast<u32>(next->first) : base + 0x4000000u;
            g_modules.push_back({it->second,
                                 index < std::size(kModuleAliases) ? kModuleAliases[index] : "",
                                 base, end - base});
        }

        LoadLoader();
        if (g_attached.load() && g_cb.on_game_start) {
            g_cb.on_game_start();
        }
    });
}

void SetCaller(const GuestCaller* caller) {
    t_caller = caller;
}

bool OnDispatchSlow(u32 pc, ModHostCpu* cpu, u32 thread_key) {
    return g_cb.on_dispatch && g_cb.on_dispatch(pc, cpu, thread_key);
}

bool OnPresent(const ModHostPresent& present) {
    if (!g_attached.load(std::memory_order_acquire) || !g_cb.on_present) {
        return false;
    }
    return g_cb.on_present(&present);
}

bool OnInput(const ModHostInput& input) {
    if (!g_attached.load(std::memory_order_acquire) || !g_cb.on_input) {
        return false;
    }
    return g_cb.on_input(&input);
}

void OnFrame() {
    if (g_attached.load(std::memory_order_acquire) && g_cb.on_frame) {
        g_cb.on_frame();
    }
}

void Shutdown() {
    if (g_attached.exchange(false) && g_cb.on_shutdown) {
        g_active.store(false);
        g_cb.on_shutdown();
    }
}

bool InputCaptured() {
    return g_input_captured.load(std::memory_order_relaxed);
}

bool TextInputWanted() {
    return g_text_input.load(std::memory_order_relaxed);
}

} // namespace Core::ModHost

// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// FSR 4 bridge (see nvn_fsr4.h). Windows only. Everything here runs on the renderer thread that owns the NVN command
// buffers; the D3D12 queue is used from that thread only. Plumbing proven by the standalone fsr4-probe (2026-10-19).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

#include "video_core/renderer_vulkan/nvn_fsr4.h"

namespace Vulkan::NvnFsr4 {

namespace {

// fpMessage has no user pointer: one sink for the (single) bridge of the process
Bridge::Log* g_msg_sink = nullptr;

void FfxMessage(uint32_t type, const wchar_t* message) {
    if (!g_msg_sink || !*g_msg_sink || !message) return;
    std::string s;
    for (const wchar_t* p = message; *p; ++p) s += *p < 128 ? static_cast<char>(*p) : '?';
    (*g_msg_sink)(fmt::format("FSR 4 runtime {}: {}", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", s));
}

std::string Hr(HRESULT h) {
    return fmt::format("hr=0x{:08x}", static_cast<uint32_t>(h));
}

std::wstring SystemPath(const wchar_t* file) {
    wchar_t dir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(dir) + L"\\" + file;
}

// the game exe's own folder (Wine/Proton: a driver DLL cannot be installed in System32, so the loader may sit beside the exe)
std::wstring ExeDirPath(const wchar_t* file) {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir.resize(slash + 1);
    return dir + file;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

std::string FileVersion(const std::wstring& path) {
    using PFN_Size = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using PFN_Info = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using PFN_Query = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    HMODULE ver = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ver) return "?";
    const auto size_fn = reinterpret_cast<PFN_Size>(GetProcAddress(ver, "GetFileVersionInfoSizeW"));
    const auto info_fn = reinterpret_cast<PFN_Info>(GetProcAddress(ver, "GetFileVersionInfoW"));
    const auto query_fn = reinterpret_cast<PFN_Query>(GetProcAddress(ver, "VerQueryValueW"));
    std::string out = "?";
    DWORD z = 0;
    const DWORD sz = size_fn ? size_fn(path.c_str(), &z) : 0;
    if (sz && info_fn && query_fn) {
        std::vector<char> buf(sz);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT l = 0;
        if (info_fn(path.c_str(), 0, sz, buf.data()) && query_fn(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &l) && fi) {
            out = fmt::format("{}.{}.{}.{}", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                              LOWORD(fi->dwFileVersionLS));
        }
    }
    FreeLibrary(ver);
    return out;
}

// first "a.b.c" in a provider name -> a * 1e6 + b * 1e3 + c, -1 when there is none
double ParseVersion(const std::string& n) {
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] >= '0' && n[i] <= '9') {
            int a = 0, b = 0, c = 0;
            if (std::sscanf(n.c_str() + i, "%d.%d.%d", &a, &b, &c) >= 1) return a * 1e6 + b * 1e3 + c;
        }
    }
    return -1.0;
}

struct FormatPair {
    VkFormat vk;
    DXGI_FORMAT dxgi;
};

// The Vulkan format of the shared image (copy-compatible with the renderer's image: same texel size and channel order)
// and its D3D12 twin. UNORM colour = gamma-encoded bytes: FSR 4 gets the non-linear (sRGB) flags for it.
FormatPair MapFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return {VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    case VK_FORMAT_B8G8R8A8_UNORM:
        return {VK_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    // sRGB images: FSR 3.1 reads them through an sRGB view (linear values) and writes linear values; FSR 4 does the same
    // here (sRGB SRV, no non-linear flags) so the composite gets the same kind of output
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return {VK_FORMAT_R8G8B8A8_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    case VK_FORMAT_B8G8R8A8_SRGB:
        return {VK_FORMAT_B8G8R8A8_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, DXGI_FORMAT_R10G10B10A2_UNORM};
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return {VK_FORMAT_R16G16B16A16_SFLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    case VK_FORMAT_R32_SFLOAT:
        return {VK_FORMAT_R32_SFLOAT, DXGI_FORMAT_R32_FLOAT};
    case VK_FORMAT_R32G32_SFLOAT:
        return {VK_FORMAT_R32G32_SFLOAT, DXGI_FORMAT_R32G32_FLOAT};
    case VK_FORMAT_R16G16_SFLOAT:
        return {VK_FORMAT_R16G16_SFLOAT, DXGI_FORMAT_R16G16_FLOAT};
    case VK_FORMAT_R8_UNORM:
        return {VK_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM};
    default:
        return {VK_FORMAT_UNDEFINED, DXGI_FORMAT_UNKNOWN};
    }
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

} // namespace

struct Bridge::Impl {
    Log log;
    VkContext vk{};
    std::string error, version_name, provider_list;
    bool initialized{};

    // Vulkan entry points (device level, through vkGetDeviceProcAddr)
    PFN_vkCreateImage vkCreateImage{};
    PFN_vkDestroyImage vkDestroyImage{};
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements{};
    PFN_vkAllocateMemory vkAllocateMemory{};
    PFN_vkFreeMemory vkFreeMemory{};
    PFN_vkBindImageMemory vkBindImageMemory{};
    PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR{};
    PFN_vkCreateSemaphore vkCreateSemaphore{};
    PFN_vkDestroySemaphore vkDestroySemaphore{};
    PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR{};
    VkPhysicalDeviceMemoryProperties mem_props{};

    // loader
    HMODULE ffx_dll{};
    HMODULE d3d12_dll{};
    HMODULE dxgi_dll{};
    PfnFfxCreateContext ffxCreateContext{};
    PfnFfxDestroyContext ffxDestroyContext{};
    PfnFfxQuery ffxQuery{};
    PfnFfxDispatch ffxDispatch{};
    uint64_t version_id{};

    // D3D12
    ID3D12Device* dev{};
    ID3D12CommandQueue* queue{};
    ID3D12Fence* fence{};
    HANDLE event{};
    struct RingSlot {
        ID3D12CommandAllocator* alloc{};
        ID3D12GraphicsCommandList* list{};
        uint64_t signal{};  // value signalled after its last use (0 = never used)
        bool ts_pending{};  // timestamps of that use not read yet
    };
    std::vector<RingSlot> ring;
    uint32_t ring_next{};
    ID3D12QueryHeap* ts_heap{};
    ID3D12Resource* ts_readback{};
    double ts_ms_per_tick{};
    double gpu_sum{}, gpu_max{};
    uint32_t gpu_n{};

    // Vulkan timeline semaphore on the shared fence
    VkSemaphore timeline{};
    uint64_t value{};       // last value handed out
    uint64_t signalled{};   // highest value a submitted D3D12 Signal / Vulkan submit will reach

    // context + images
    ffxContext ctx{};
    uint32_t rw{}, rh{}, ow{}, oh{};
    VkFormat in_color{VK_FORMAT_UNDEFINED}, in_motion{VK_FORMAT_UNDEFINED};
    struct Shared {
        SharedImage img;
        ID3D12Resource* res{};
        VkDeviceMemory mem{};
    };
    std::array<Shared, kSlotCount> shared{};
    bool fresh{};
    bool non_linear{true}; // colour is gamma-encoded UNORM (flags on); false for an sRGB-format input (decoded by the SRV)

    bool Fail(const std::string& why) {
        error = why;
        if (log) log("FSR 4: " + why);
        return false;
    }

    template <class F>
    bool Load(F& fn, const char* name) {
        fn = reinterpret_cast<F>(vk.get_device_proc_addr(vk.device, name));
        return fn != nullptr;
    }

    uint32_t FindMemoryType(uint32_t bits) const {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
        }
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if (bits & (1u << i)) return i;
        }
        return ~0u;
    }

    bool CreateShared(Shared& s, const char* what, VkFormat vk_fmt, DXGI_FORMAT dxgi, uint32_t w, uint32_t h, bool uav) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = dxgi;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.res));
        if (FAILED(hr)) return Fail(fmt::format("{}: D3D12 CreateCommittedResource (DXGI format {} {}x{} shared + simultaneous) failed {}", what,
                                                static_cast<int>(dxgi), w, h, Hr(hr)));
        HANDLE handle{};
        hr = dev->CreateSharedHandle(s.res, nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) return Fail(fmt::format("{}: CreateSharedHandle failed {}", what, Hr(hr)));
        VkExternalMemoryImageCreateInfo eci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        eci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &eci};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = vk_fmt;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(vk.device, &ici, nullptr, &s.img.image);
        if (r != VK_SUCCESS) {
            CloseHandle(handle);
            s.img.image = VK_NULL_HANDLE;
            return Fail(fmt::format("{}: vkCreateImage (external D3D12 resource, VkFormat {}) failed {}", what, static_cast<int>(vk_fmt), static_cast<int>(r)));
        }
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(vk.device, s.img.image, &mr);
        VkMemoryWin32HandlePropertiesKHR hpp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        r = vkGetMemoryWin32HandlePropertiesKHR(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &hpp);
        const uint32_t bits = (r == VK_SUCCESS ? hpp.memoryTypeBits : mr.memoryTypeBits) & mr.memoryTypeBits;
        const uint32_t mt = FindMemoryType(bits);
        if (mt == ~0u) {
            CloseHandle(handle);
            return Fail(fmt::format("{}: no memory type for the import (handle bits {:#x} result {}, image bits {:#x})", what, hpp.memoryTypeBits,
                                    static_cast<int>(r), mr.memoryTypeBits));
        }
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        imp.handle = handle;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
        ded.image = s.img.image;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        r = vkAllocateMemory(vk.device, &mai, nullptr, &s.mem);
        CloseHandle(handle); // an NT handle import does not take ownership of the handle
        if (r != VK_SUCCESS) {
            s.mem = VK_NULL_HANDLE;
            return Fail(fmt::format("{}: vkAllocateMemory (import) failed {}", what, static_cast<int>(r)));
        }
        r = vkBindImageMemory(vk.device, s.img.image, s.mem, 0);
        if (r != VK_SUCCESS) return Fail(fmt::format("{}: vkBindImageMemory failed {}", what, static_cast<int>(r)));
        s.img.format = vk_fmt;
        s.img.width = w;
        s.img.height = h;
        return true;
    }

    void DestroyShared() {
        for (Shared& s : shared) {
            if (s.img.image) vkDestroyImage(vk.device, s.img.image, nullptr);
            if (s.mem) vkFreeMemory(vk.device, s.mem, nullptr);
            SafeRelease(s.res);
            s = Shared{};
        }
    }

    void DestroyCtx() {
        if (ctx && ffxDestroyContext) ffxDestroyContext(&ctx, nullptr);
        ctx = nullptr;
        DestroyShared();
        rw = rh = ow = oh = 0;
        in_color = in_motion = VK_FORMAT_UNDEFINED;
    }

    bool WaitFor(uint64_t v, uint32_t timeout_ms) {
        if (!fence) return false;
        if (fence->GetCompletedValue() >= v) return true;
        ResetEvent(event);
        if (FAILED(fence->SetEventOnCompletion(v, event))) return false;
        WaitForSingleObject(event, timeout_ms);
        return fence->GetCompletedValue() >= v && fence->GetCompletedValue() != UINT64_MAX; // UINT64_MAX = device removed
    }

    void ReadTimestamps(uint32_t idx) {
        RingSlot& s = ring[idx];
        if (!s.ts_pending || !ts_readback) return;
        s.ts_pending = false;
        void* p = nullptr;
        const D3D12_RANGE rg{size_t{idx} * 16, size_t{idx} * 16 + 16};
        if (FAILED(ts_readback->Map(0, &rg, &p)) || !p) return;
        const uint64_t* t = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(p) + size_t{idx} * 16);
        const uint64_t a = t[0], b = t[1];
        const D3D12_RANGE none{0, 0};
        ts_readback->Unmap(0, &none);
        if (b <= a) return;
        const double ms = static_cast<double>(b - a) * ts_ms_per_tick;
        if (ms > 1000.0) return;
        gpu_sum += ms;
        gpu_max = std::max(gpu_max, ms);
        ++gpu_n;
    }

    void ReleaseAll() {
        DestroyCtx();
        if (timeline && vkDestroySemaphore) vkDestroySemaphore(vk.device, timeline, nullptr);
        timeline = VK_NULL_HANDLE;
        for (RingSlot& s : ring) {
            SafeRelease(s.list);
            SafeRelease(s.alloc);
        }
        ring.clear();
        SafeRelease(ts_readback);
        SafeRelease(ts_heap);
        SafeRelease(fence);
        SafeRelease(queue);
        SafeRelease(dev);
        if (event) CloseHandle(event);
        event = nullptr;
        initialized = false;
        // the DLLs stay loaded for the run (the loader may keep provider threads; unloading buys nothing)
    }
};

Bridge::Bridge() : impl{std::make_unique<Impl>()} {}

Bridge::~Bridge() {
    Shutdown();
    if (g_msg_sink == &impl->log) g_msg_sink = nullptr;
}

bool Bridge::Initialized() const {
    return impl->initialized;
}

bool Bridge::Init(const VkContext& vk, Log log) {
    Impl& m = *impl;
    m.log = std::move(log);
    m.vk = vk;
    g_msg_sink = &m.log;
    if (!vk.device || !vk.physical || !vk.get_device_proc_addr || !vk.get_physical_properties2 || !vk.get_physical_memory_properties) {
        return m.Fail("init: missing Vulkan entry points");
    }
    bool ok = m.Load(m.vkCreateImage, "vkCreateImage") && m.Load(m.vkDestroyImage, "vkDestroyImage") &&
              m.Load(m.vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements") && m.Load(m.vkAllocateMemory, "vkAllocateMemory") &&
              m.Load(m.vkFreeMemory, "vkFreeMemory") && m.Load(m.vkBindImageMemory, "vkBindImageMemory") &&
              m.Load(m.vkCreateSemaphore, "vkCreateSemaphore") && m.Load(m.vkDestroySemaphore, "vkDestroySemaphore");
    if (!ok) return m.Fail("init: core Vulkan entry points missing");
    if (!m.Load(m.vkGetMemoryWin32HandlePropertiesKHR, "vkGetMemoryWin32HandlePropertiesKHR") ||
        !m.Load(m.vkImportSemaphoreWin32HandleKHR, "vkImportSemaphoreWin32HandleKHR")) {
        return m.Fail("init: VK_KHR_external_memory_win32 / VK_KHR_external_semaphore_win32 entry points missing (extensions not enabled?)");
    }
    vk.get_physical_memory_properties(vk.physical, &m.mem_props);

    // (1) The DLL in the game folder first (same place on Windows and Linux/Proton, and the user's own FSR 4 DLL
    //     is simply dropped there); it loads its sibling amd_fidelityfx_upscaler_dx12.dll from its own folder
    //     (LOAD_WITH_ALTERED_SEARCH_PATH). Then the AMD driver's loader in System32.
    std::wstring dll;
    for (const wchar_t* name : {L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll"}) {
        const std::wstring cand = ExeDirPath(name);
        if (cand.empty()) continue;
        m.ffx_dll = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m.ffx_dll) {
            dll = cand;
            break;
        }
    }
    DWORD sys_err = 0;
    if (!m.ffx_dll) {
        dll = SystemPath(L"amd_fidelityfx_dx12.dll");
        m.ffx_dll = dll.empty() ? nullptr : LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        sys_err = GetLastError();
    }
    if (!m.ffx_dll) return m.Fail(fmt::format("init: {} not loadable (error {}), and none beside the exe: put amd_fidelityfx_dx12.dll in the game folder", Narrow(dll), sys_err));
    m.ffxCreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m.ffx_dll, "ffxCreateContext"));
    m.ffxDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m.ffx_dll, "ffxDestroyContext"));
    m.ffxQuery = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m.ffx_dll, "ffxQuery"));
    m.ffxDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m.ffx_dll, "ffxDispatch"));
    if (m.log) m.log(fmt::format("FSR 4: loaded {} (file version {}); exports create {} destroy {} query {} dispatch {}", Narrow(dll), FileVersion(dll),
                                 m.ffxCreateContext != nullptr, m.ffxDestroyContext != nullptr, m.ffxQuery != nullptr, m.ffxDispatch != nullptr));
    if (!m.ffxCreateContext || !m.ffxDestroyContext || !m.ffxQuery || !m.ffxDispatch) return m.Fail("init: ffx exports missing");

    // (2) LUID of suyu's Vulkan device -> the same adapter in DXGI
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vk.get_physical_properties2(vk.physical, &p2);
    if (!idp.deviceLUIDValid) return m.Fail(fmt::format("init: Vulkan device {} reports no valid LUID", p2.properties.deviceName));
    LUID luid{};
    std::memcpy(&luid, idp.deviceLUID, sizeof(luid));
    m.d3d12_dll = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    m.dxgi_dll = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using PFN_CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create_factory =
        m.dxgi_dll ? reinterpret_cast<PFN_CreateFactory1>(GetProcAddress(m.dxgi_dll, "CreateDXGIFactory1")) : nullptr;
    const auto create_device =
        m.d3d12_dll ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(m.d3d12_dll, "D3D12CreateDevice")) : nullptr;
    if (!create_factory || !create_device) return m.Fail("init: d3d12.dll / dxgi.dll entry points not found");
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return m.Fail("init: CreateDXGIFactory1 failed " + Hr(hr));
    IDXGIAdapter1* adapter = nullptr;
    hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    SafeRelease(factory);
    if (FAILED(hr)) return m.Fail(fmt::format("init: no DXGI adapter with the Vulkan LUID {:08x}-{:08x} ({})", static_cast<uint32_t>(luid.HighPart),
                                              luid.LowPart, Hr(hr)));
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    const bool luid_match = ad.AdapterLuid.LowPart == luid.LowPart && ad.AdapterLuid.HighPart == luid.HighPart;
    hr = luid_match ? create_device(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m.dev)) : E_FAIL;
    SafeRelease(adapter);
    if (m.log) m.log(fmt::format("FSR 4: Vulkan device {} LUID {:08x}-{:08x} -> DXGI adapter {} LUID {:08x}-{:08x}: {}", p2.properties.deviceName,
                                 static_cast<uint32_t>(luid.HighPart), luid.LowPart, Narrow(ad.Description),
                                 static_cast<uint32_t>(ad.AdapterLuid.HighPart), ad.AdapterLuid.LowPart, luid_match ? "match" : "MISMATCH"));
    if (!luid_match) return m.Fail("init: LUID mismatch");
    if (FAILED(hr) || !m.dev) return m.Fail("init: D3D12CreateDevice failed " + Hr(hr));
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = m.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&m.queue));
    if (FAILED(hr)) return m.Fail("init: CreateCommandQueue failed " + Hr(hr));
    hr = m.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m.fence));
    if (FAILED(hr)) return m.Fail("init: CreateFence(shared) failed " + Hr(hr));
    m.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m.event) return m.Fail("init: CreateEvent failed");
    m.ring.resize(std::max<uint32_t>(2, vk.ring));
    for (auto& s : m.ring) {
        hr = m.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc));
        if (SUCCEEDED(hr)) hr = m.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc, nullptr, IID_PPV_ARGS(&s.list));
        if (FAILED(hr)) return m.Fail("init: command allocator / list failed " + Hr(hr));
        s.list->Close();
    }
    {
        D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>(m.ring.size() * 2), 0};
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = m.ring.size() * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        UINT64 freq = 0;
        if (FAILED(m.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&m.ts_heap))) ||
            FAILED(m.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&m.ts_readback))) ||
            FAILED(m.queue->GetTimestampFrequency(&freq)) || freq == 0) {
            SafeRelease(m.ts_readback);
            SafeRelease(m.ts_heap);
            if (m.log) m.log("FSR 4: no D3D12 timestamps (GPU cost of the dispatch will read 0)");
        } else {
            m.ts_ms_per_tick = 1000.0 / static_cast<double>(freq);
        }
    }

    // (3) providers (needs the device): highest FSR 4.x
    {
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = m.dev;
        q.outputCount = &count;
        ffxReturnCode_t rc = m.ffxQuery(nullptr, &q.header);
        if (rc != FFX_API_RETURN_OK || count == 0) return m.Fail(fmt::format("init: provider count query rc {} count {}", rc, count));
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count, nullptr);
        uint64_t cap = count;
        q.outputCount = &cap;
        q.versionIds = ids.data();
        q.versionNames = names.data();
        rc = m.ffxQuery(nullptr, &q.header);
        if (rc != FFX_API_RETURN_OK) return m.Fail(fmt::format("init: provider list query rc {}", rc));
        int best = -1;
        double best_v = -2.0;
        for (uint64_t i = 0; i < cap && i < count; ++i) {
            const std::string n = names[i] ? names[i] : "";
            const double v = ParseVersion(n);
            m.provider_list += fmt::format("{}[{}] \"{}\" id {:#x}", m.provider_list.empty() ? "" : ", ", i, n, ids[i]);
            if (v >= 4e6 && v > best_v) best_v = v, best = static_cast<int>(i);
        }
        if (m.log) m.log("FSR 4: upscaler providers: " + m.provider_list);
        // Wine/Proton: amdxc64's FSR4_UPGRADE swaps the FSR 3.1 provider for FSR 4 inside the loader, so no 4.x provider is
        // listed; take the highest listed one and let the context-provider log below say what really runs.
        bool upgraded = false;
        if (best < 0) {
            const char* up = std::getenv("FSR4_UPGRADE");
            if (up && *up && *up != '0') {
                for (uint64_t i = 0; i < cap && i < count; ++i) {
                    const double v = ParseVersion(names[i] ? names[i] : "");
                    if (v > best_v) best_v = v, best = static_cast<int>(i);
                }
                upgraded = best >= 0;
            }
        }
        if (best < 0) return m.Fail("init: no FSR 4.x provider listed by the driver loader (needs an RDNA4 GPU and a driver with FSR 4)");
        m.version_id = ids[best];
        m.version_name = names[best] ? names[best] : "FSR 4";
        if (upgraded) m.version_name = "4 (FSR4_UPGRADE over " + m.version_name + ")";
        if (m.version_name.rfind("FSR", 0) != 0) m.version_name = "FSR " + m.version_name;
        if (m.log) m.log(fmt::format("FSR 4: chosen provider [{}] \"{}\" id {:#x} (ffxOverrideVersion)", best, m.version_name, m.version_id));
    }

    // (4) the shared fence as a Vulkan timeline semaphore
    {
        HANDLE fh{};
        hr = m.dev->CreateSharedHandle(m.fence, nullptr, GENERIC_ALL, nullptr, &fh);
        if (FAILED(hr)) return m.Fail("init: CreateSharedHandle(fence) failed " + Hr(hr));
        VkSemaphoreTypeCreateInfo tt{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tt.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tt.initialValue = 0;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tt};
        VkResult r = m.vkCreateSemaphore(vk.device, &sc, nullptr, &m.timeline);
        if (r != VK_SUCCESS) {
            CloseHandle(fh);
            m.timeline = VK_NULL_HANDLE;
            return m.Fail(fmt::format("init: vkCreateSemaphore(timeline) failed {}", static_cast<int>(r)));
        }
        VkImportSemaphoreWin32HandleInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        im.semaphore = m.timeline;
        im.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        im.handle = fh;
        r = m.vkImportSemaphoreWin32HandleKHR(vk.device, &im);
        CloseHandle(fh);
        if (r != VK_SUCCESS) return m.Fail(fmt::format("init: vkImportSemaphoreWin32HandleKHR(D3D12 fence) failed {}", static_cast<int>(r)));
    }
    m.initialized = true;
    if (m.log) m.log("FSR 4: D3D12 device, queue, shared fence (imported as a Vulkan timeline semaphore) ready");
    return true;
}

bool Bridge::NeedsRecreate(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkFormat color, VkFormat motion) const {
    const Impl& m = *impl;
    return !m.ctx || m.rw != rw || m.rh != rh || m.ow != ow || m.oh != oh || m.in_color != color || m.in_motion != motion;
}

bool Bridge::Ensure(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, VkFormat color, VkFormat motion) {
    Impl& m = *impl;
    if (!m.initialized) return m.Fail("not initialised");
    if (!NeedsRecreate(rw, rh, ow, oh, color, motion)) return true;
    m.DestroyCtx();
    const FormatPair cf = MapFormat(color), mf = MapFormat(motion);
    if (cf.vk == VK_FORMAT_UNDEFINED) return m.Fail(fmt::format("colour input VkFormat {} has no shareable twin", static_cast<int>(color)));
    if (mf.vk != VK_FORMAT_R32G32_SFLOAT && mf.vk != VK_FORMAT_R16G16_SFLOAT) {
        return m.Fail(fmt::format("motion vector VkFormat {} is not RG32F / RG16F", static_cast<int>(motion)));
    }
    const double t0 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!m.CreateShared(m.shared[kColor], "colour", cf.vk, cf.dxgi, rw, rh, false) ||
        !m.CreateShared(m.shared[kDepth], "depth", VK_FORMAT_R32_SFLOAT, DXGI_FORMAT_R32_FLOAT, rw, rh, false) ||
        !m.CreateShared(m.shared[kMotion], "motion vectors", mf.vk, mf.dxgi, rw, rh, false) ||
        !m.CreateShared(m.shared[kReactive], "reactive", VK_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, rw, rh, false) ||
        !m.CreateShared(m.shared[kOutput], "output", VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, ow, oh, true)) {
        m.DestroyCtx();
        return false;
    }
    ffxCreateContextDescUpscale cu{};
    cu.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // LDR, gamma-encoded colour (post-tonemap RGBA8, the same bytes FSR 3.1 gets); standard depth; render-size motion
    // vectors without jitter
    m.non_linear = !(cf.vk == VK_FORMAT_R8G8B8A8_SRGB || cf.vk == VK_FORMAT_B8G8R8A8_SRGB);
    cu.flags = (m.non_linear ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0u) | (m.vk.debug_checking ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0u);
    cu.maxRenderSize = {rw, rh};
    cu.maxUpscaleSize = {ow, oh};
    cu.fpMessage = FfxMessage;
    ffxCreateBackendDX12Desc bd{};
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    bd.device = m.dev;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = m.version_id;
    cu.header.pNext = &bd.header;
    bd.header.pNext = &ov.header;
    const ffxReturnCode_t rc = m.ffxCreateContext(&m.ctx, &cu.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !m.ctx) {
        m.ctx = nullptr;
        m.DestroyCtx();
        return m.Fail(fmt::format("ffxCreateContext {}x{} -> {}x{} failed rc {}", rw, rh, ow, oh, rc));
    }
    m.rw = rw, m.rh = rh, m.ow = ow, m.oh = oh;
    m.in_color = color;
    m.in_motion = motion;
    m.fresh = true;
    const double t1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m.log) {
        m.log(fmt::format("FSR 4: context {}x{} -> {}x{} created ({}, create flags {:#x} = {}{}; dispatch flag NON_LINEAR_COLOR_SRGB {}), shared "
                          "images colour VkFormat {} (from {}), depth R32F, motion VkFormat {}, reactive R8, output RGBA8 UAV; {:.1f} ms",
                          rw, rh, ow, oh, m.version_name, cu.flags,
                          m.non_linear ? "NON_LINEAR_COLORSPACE (gamma-encoded UNORM input)" : "linear (sRGB-format input, decoded by the SRV)",
                          m.vk.debug_checking ? " + debug checking" : "", m.non_linear ? "on" : "off", static_cast<int>(cf.vk),
                          static_cast<int>(color), static_cast<int>(mf.vk), t1 - t0));
    }
    return true;
}

bool Bridge::ContextReady() const {
    return impl->ctx != nullptr;
}

const SharedImage& Bridge::Image(Slot s) const {
    return impl->shared[s].img;
}

bool Bridge::TakeFreshImages() {
    const bool f = impl->fresh;
    impl->fresh = false;
    return f;
}

VkSemaphore Bridge::Timeline() const {
    return impl->timeline;
}

uint64_t Bridge::NextValue() {
    return ++impl->value;
}

bool Bridge::Dispatch(const DispatchParams& p, uint64_t wait_value, uint64_t signal_value) {
    Impl& m = *impl;
    if (!m.ctx) return m.Fail("dispatch without a context");
    // the Vulkan submit that signals wait_value was made by the caller
    m.signalled = std::max(m.signalled, wait_value);
    const uint32_t idx = m.ring_next;
    m.ring_next = (m.ring_next + 1) % static_cast<uint32_t>(m.ring.size());
    Impl::RingSlot& s = m.ring[idx];
    if (s.signal && !m.WaitFor(s.signal, 1000)) {
        return m.Fail(fmt::format("the D3D12 fence did not reach {} within 1 s (completed {}, device {}): Vulkan -> D3D12 handoff stuck",
                                  s.signal, m.fence->GetCompletedValue(), Hr(m.dev->GetDeviceRemovedReason())));
    }
    m.ReadTimestamps(idx);
    HRESULT hr = s.alloc->Reset();
    if (SUCCEEDED(hr)) hr = s.list->Reset(s.alloc, nullptr);
    if (FAILED(hr)) return m.Fail("command list reset failed " + Hr(hr));
    if (m.ts_heap) s.list->EndQuery(m.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, idx * 2);
    ffxDispatchDescUpscale du{};
    du.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    du.commandList = s.list;
    // simultaneous-access resources stay in COMMON on the D3D12 side; no transitions (as in the probe)
    du.color = ffxApiGetResourceDX12(m.shared[kColor].res, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    du.depth = ffxApiGetResourceDX12(m.shared[kDepth].res, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    du.motionVectors = ffxApiGetResourceDX12(m.shared[kMotion].res, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    du.exposure = ffxApiGetResourceDX12(nullptr);
    du.reactive = p.reactive ? ffxApiGetResourceDX12(m.shared[kReactive].res, FFX_API_RESOURCE_STATE_COMPUTE_READ) : ffxApiGetResourceDX12(nullptr);
    du.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
    du.output = ffxApiGetResourceDX12(m.shared[kOutput].res, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    du.jitterOffset = {p.jitter_x, p.jitter_y};
    du.motionVectorScale = {1.0f, 1.0f};
    du.renderSize = {p.render_w, p.render_h};
    du.upscaleSize = {p.output_w, p.output_h};
    du.enableSharpening = p.sharpen;
    du.sharpness = p.sharpness;
    du.frameTimeDelta = p.frame_ms;
    du.preExposure = 1.0f;
    du.reset = p.reset;
    du.cameraNear = p.camera_near;
    du.cameraFar = p.camera_far;
    du.cameraFovAngleVertical = p.fov_y;
    du.viewSpaceToMetersFactor = p.view_to_meters;
    du.flags = m.non_linear ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0u;
    const ffxReturnCode_t rc = m.ffxDispatch(&m.ctx, &du.header);
    if (m.ts_heap) {
        s.list->EndQuery(m.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, idx * 2 + 1);
        s.list->ResolveQueryData(m.ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, idx * 2, 2, m.ts_readback, UINT64{idx} * 16);
    }
    hr = s.list->Close();
    // the queue always waits and signals, so the timeline stays consistent; on a failed record nothing is executed
    m.queue->Wait(m.fence, wait_value);
    const bool record_ok = rc == FFX_API_RETURN_OK && SUCCEEDED(hr);
    if (record_ok) {
        ID3D12CommandList* lists[] = {s.list};
        m.queue->ExecuteCommandLists(1, lists);
    }
    const HRESULT sh = m.queue->Signal(m.fence, signal_value);
    if (SUCCEEDED(sh)) {
        s.signal = signal_value;
        s.ts_pending = record_ok && m.ts_heap != nullptr;
        m.signalled = std::max(m.signalled, signal_value);
    }
    if (rc != FFX_API_RETURN_OK) return m.Fail(fmt::format("ffxDispatch rc {}", rc));
    if (FAILED(hr)) return m.Fail("command list close failed " + Hr(hr));
    if (FAILED(sh)) return m.Fail("queue Signal failed " + Hr(sh));
    return true;
}

bool Bridge::WaitIdle(uint32_t timeout_ms) {
    Impl& m = *impl;
    if (!m.fence) return true;
    const bool ok = m.WaitFor(m.signalled, timeout_ms);
    for (uint32_t i = 0; ok && i < m.ring.size(); ++i) m.ReadTimestamps(i);
    return ok;
}

void Bridge::ReleaseWaiters() {
    Impl& m = *impl;
    if (!m.fence) return;
    const uint64_t top = std::max(m.value, m.signalled);
    if (m.fence->GetCompletedValue() < top) {
        m.fence->Signal(top);
        if (m.log) m.log(fmt::format("FSR 4: fence set to {} from the CPU so no Vulkan submit waits on a value that never comes", top));
    }
}

void Bridge::DestroyContext() {
    impl->DestroyCtx();
}

void Bridge::Shutdown() {
    Impl& m = *impl;
    if (m.fence) {
        if (!WaitIdle(1000)) ReleaseWaiters();
    }
    m.ReleaseAll();
}

int32_t Bridge::JitterPhaseCount(uint32_t render_w, uint32_t output_w) const {
    const Impl& m = *impl;
    if (!m.ctx) return 0;
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth = render_w;
    q.displayWidth = output_w;
    int32_t n = 0;
    q.pOutPhaseCount = &n;
    ffxContext c = m.ctx;
    return m.ffxQuery(&c, &q.header) == FFX_API_RETURN_OK ? n : 0;
}

bool Bridge::JitterOffset(int32_t index, int32_t phase_count, float* x, float* y) const {
    const Impl& m = *impl;
    if (!m.ctx) return false;
    ffxQueryDescUpscaleGetJitterOffset q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index = index;
    q.phaseCount = phase_count;
    q.pOutX = x;
    q.pOutY = y;
    ffxContext c = m.ctx;
    return m.ffxQuery(&c, &q.header) == FFX_API_RETURN_OK;
}

const std::string& Bridge::VersionName() const {
    return impl->version_name;
}

const std::string& Bridge::ProviderList() const {
    return impl->provider_list;
}

const std::string& Bridge::LastError() const {
    return impl->error;
}

void Bridge::TakeGpuStats(double& sum_ms, uint32_t& count, double& max_ms) {
    Impl& m = *impl;
    // finished slots that were not reused yet
    for (uint32_t i = 0; i < m.ring.size(); ++i) {
        if (m.ring[i].ts_pending && m.fence && m.fence->GetCompletedValue() >= m.ring[i].signal) m.ReadTimestamps(i);
    }
    sum_ms = m.gpu_sum;
    count = m.gpu_n;
    max_ms = m.gpu_max;
    m.gpu_sum = m.gpu_max = 0.0;
    m.gpu_n = 0;
}

} // namespace Vulkan::NvnFsr4

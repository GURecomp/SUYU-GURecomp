/*
 * Copyright (c) 2026 suyu Emulator Project
 * SPDX-License-Identifier: MIT
 *
 * Global vk* entry points used directly by the vendored FSR port (see the header).
 * Only these symbols are defined; everything else the port uses goes through
 * vkGetDeviceProcAddr already.
 */

#if defined(VK_NO_PROTOTYPES)
#error "the loader shim needs the Vulkan prototypes"
#endif

#include "ffx_vk_loader_shim.h"

#include <cstring>

namespace {

struct Table {
    bool ready = false;
    FfxVkSuyuLoader in{};
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
};

Table g_table;

template <typename T>
T DeviceFn(const FfxVkSuyuLoader& l, const char* name, int& missing) {
    const PFN_vkVoidFunction f = l.getDeviceProcAddr ? l.getDeviceProcAddr(l.device, name) : nullptr;
    if (f == nullptr)
        ++missing;
    return reinterpret_cast<T>(f);
}

} // namespace

extern "C" int ffxVkSuyuLoaderInit(const FfxVkSuyuLoader* loader) {
    if (loader == nullptr)
        return 1;
    int missing = 0;
    const FfxVkSuyuLoader& l = *loader;
    missing += l.device == VK_NULL_HANDLE;
    missing += l.getDeviceProcAddr == nullptr;
    missing += l.getPhysicalDeviceProperties == nullptr;
    missing += l.getPhysicalDeviceProperties2 == nullptr;
    missing += l.getPhysicalDeviceFeatures2 == nullptr;
    missing += l.getPhysicalDeviceMemoryProperties == nullptr;
    missing += l.getPhysicalDeviceFormatProperties == nullptr;
    missing += l.getPhysicalDeviceQueueFamilyProperties == nullptr;
    missing += l.enumerateDeviceExtensionProperties == nullptr;
    Table t{};
    t.in = l;
    t.createImage = DeviceFn<PFN_vkCreateImage>(l, "vkCreateImage", missing);
    t.destroyImage = DeviceFn<PFN_vkDestroyImage>(l, "vkDestroyImage", missing);
    t.allocateMemory = DeviceFn<PFN_vkAllocateMemory>(l, "vkAllocateMemory", missing);
    t.freeMemory = DeviceFn<PFN_vkFreeMemory>(l, "vkFreeMemory", missing);
    t.bindImageMemory = DeviceFn<PFN_vkBindImageMemory>(l, "vkBindImageMemory", missing);
    t.getImageMemoryRequirements =
        DeviceFn<PFN_vkGetImageMemoryRequirements>(l, "vkGetImageMemoryRequirements", missing);
    t.createBuffer = DeviceFn<PFN_vkCreateBuffer>(l, "vkCreateBuffer", missing);
    if (missing != 0)
        return missing;
    t.ready = true;
    g_table = t;
    return 0;
}

// These definitions share their names with the real Vulkan loader's exports. If the executable
// exported them, libvulkan's own entry points would be interposed by these stubs (which return
// nothing until ffxVkSuyuLoaderInit has run). Hidden keeps them private to this program.
#if defined(__GNUC__) || defined(__clang__)
#define SUYU_FFX_SHIM_HIDDEN __attribute__((visibility("hidden")))
#else
#define SUYU_FFX_SHIM_HIDDEN
#endif

extern "C" {

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return g_table.ready ? g_table.in.getDeviceProcAddr(device, pName) : nullptr;
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                         VkPhysicalDeviceProperties* pProperties) {
    if (g_table.ready)
        g_table.in.getPhysicalDeviceProperties(physicalDevice, pProperties);
    else if (pProperties)
        std::memset(pProperties, 0, sizeof(*pProperties));
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                          VkPhysicalDeviceProperties2* pProperties) {
    if (g_table.ready)
        g_table.in.getPhysicalDeviceProperties2(physicalDevice, pProperties);
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceFeatures2* pFeatures) {
    if (g_table.ready)
        g_table.in.getPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                                       VkPhysicalDeviceFeatures* pFeatures) {
    if (pFeatures == nullptr)
        return;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (g_table.ready)
        g_table.in.getPhysicalDeviceFeatures2(physicalDevice, &f2);
    *pFeatures = f2.features;
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                               VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    if (g_table.ready)
        g_table.in.getPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
    else if (pMemoryProperties)
        std::memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
                                                               VkFormatProperties* pFormatProperties) {
    if (g_table.ready)
        g_table.in.getPhysicalDeviceFormatProperties(physicalDevice, format, pFormatProperties);
    else if (pFormatProperties)
        std::memset(pFormatProperties, 0, sizeof(*pFormatProperties));
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties* pQueueFamilyProperties) {
    if (g_table.ready)
        g_table.in.getPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount,
                                                          pQueueFamilyProperties);
    else if (pQueueFamilyPropertyCount)
        *pQueueFamilyPropertyCount = 0;
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                    const char* pLayerName, uint32_t* pPropertyCount,
                                                                    VkExtensionProperties* pProperties) {
    if (!g_table.ready) {
        if (pPropertyCount)
            *pPropertyCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return g_table.in.enumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    return g_table.ready ? g_table.createImage(device, pCreateInfo, pAllocator, pImage)
                         : VK_ERROR_INITIALIZATION_FAILED;
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    if (g_table.ready)
        g_table.destroyImage(device, image, pAllocator);
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                                const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
    return g_table.ready ? g_table.allocateMemory(device, pAllocateInfo, pAllocator, pMemory)
                         : VK_ERROR_INITIALIZATION_FAILED;
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
    if (g_table.ready)
        g_table.freeMemory(device, memory, pAllocator);
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                 VkDeviceSize memoryOffset) {
    return g_table.ready ? g_table.bindImageMemory(device, image, memory, memoryOffset)
                         : VK_ERROR_INITIALIZATION_FAILED;
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                                        VkMemoryRequirements* pMemoryRequirements) {
    if (g_table.ready)
        g_table.getImageMemoryRequirements(device, image, pMemoryRequirements);
    else if (pMemoryRequirements)
        std::memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
}

SUYU_FFX_SHIM_HIDDEN VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
    return g_table.ready ? g_table.createBuffer(device, pCreateInfo, pAllocator, pBuffer)
                         : VK_ERROR_INITIALIZATION_FAILED;
}

} // extern "C"

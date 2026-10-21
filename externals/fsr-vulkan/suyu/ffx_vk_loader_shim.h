/*
 * Copyright (c) 2026 suyu Emulator Project
 * SPDX-License-Identifier: MIT
 *
 * suyu loads Vulkan at run time and never links the Vulkan loader (vulkan-1).
 * The vendored FSR port calls a handful of global vk* functions directly
 * (physical-device queries, a few device calls in the portable wrapper and in
 * AMD's backend). ffx_vk_loader_shim.cpp defines exactly those symbols and
 * forwards them to function pointers handed over here, so no new system
 * dependency appears. Call ffxVkSuyuLoaderInit once before creating an FSR
 * context; until then every shimmed call is a no-op / VK_ERROR_INITIALIZATION_FAILED.
 */

#ifndef FFX_VK_LOADER_SHIM_H
#define FFX_VK_LOADER_SHIM_H

#include <vulkan/vulkan.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct FfxVkSuyuLoader {
    VkDevice device;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties getPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties;
} FfxVkSuyuLoader;

/* Returns 0 on success, else the number of entry points that could not be resolved
 * (the shim is then left uninitialised). */
int ffxVkSuyuLoaderInit(const FfxVkSuyuLoader* loader);

#if defined(__cplusplus)
}
#endif

#endif

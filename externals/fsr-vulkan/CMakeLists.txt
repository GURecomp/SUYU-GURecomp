# SPDX-FileCopyrightText: 2026 suyu Emulator Project
# SPDX-License-Identifier: MIT
#
# Minimal build of the vendored FSR 3.1.4 Vulkan upscaler (FireBurn/FSR-Vulkan subset, see README.md).
# One static library, C11/C++17, checked-in SPIR-V headers (no shader compiler at build time), no Vulkan
# loader dependency (the global vk* calls of the port go through suyu/ffx_vk_loader_shim.cpp).
# Replaces the port's own CMakeLists.txt (tests, probe, FSR4, frame generation and install rules dropped).

set(FFX_1_1_4_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ffx-1.1.4/sdk")
set(FFX_1_1_4_GENERATED_FSR3UPSCALER "${CMAKE_CURRENT_SOURCE_DIR}/generated/ffx-1.1.4/vk/fsr3upscaler")

add_library(ffx_vulkan_fsr3_upscaler STATIC
    # portable public API (ffx_vk_portable.h)
    src/ffx_vk_portable.c
    src/ffx_vk_fsr3_portable.cpp
    # AMD FidelityFX SDK v1.1.4 host: FSR 3.1.4 upscaler scheduler
    "${FFX_1_1_4_ROOT}/src/shared/ffx_assert.cpp"
    "${FFX_1_1_4_ROOT}/src/shared/ffx_message.cpp"
    "${FFX_1_1_4_ROOT}/src/shared/ffx_object_management.cpp"
    "${FFX_1_1_4_ROOT}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    # AMD FidelityFX SDK v1.1.4 Vulkan backend + FSR3 upscaler shader blobs
    "${FFX_1_1_4_ROOT}/src/backends/vk/ffx_vk.cpp"
    "${FFX_1_1_4_ROOT}/src/backends/shared/ffx_shader_blobs.cpp"
    "${FFX_1_1_4_ROOT}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${FFX_1_1_4_ROOT}/src/shared/ffx_breadcrumbs_list.cpp"
    # suyu: global vk* entry points forwarded to suyu's run-time loaded Vulkan
    suyu/ffx_vk_loader_shim.cpp
)
add_library(ffx-vulkan::fsr3-upscaler ALIAS ffx_vulkan_fsr3_upscaler)

set_target_properties(ffx_vulkan_fsr3_upscaler PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_definitions(ffx_vulkan_fsr3_upscaler PRIVATE
    FFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1
    FFX_FSR3UPSCALER=1
    DISABLE_FFX_DEBUG_BREAK=1 # Debug builds: a failed FFX assert must not stop the game in a debugger break
)
target_include_directories(ffx_vulkan_fsr3_upscaler
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/suyu"
    PRIVATE
        "${FFX_1_1_4_ROOT}/include"
        "${FFX_1_1_4_ROOT}/src/shared"
        "${FFX_1_1_4_ROOT}/src/components"
        "${FFX_1_1_4_ROOT}/src/backends/shared"
        "${FFX_1_1_4_ROOT}/src/backends/shared/blob_accessors"
        "${FFX_1_1_4_GENERATED_FSR3UPSCALER}"
)
# Headers only (the loader is never linked).
target_link_libraries(ffx_vulkan_fsr3_upscaler PUBLIC Vulkan::Headers)

if (MSVC)
    # third-party code: keep the build log quiet, never fail the build on its warnings
    target_compile_options(ffx_vulkan_fsr3_upscaler PRIVATE /W0 /WX- /bigobj)
else()
    target_compile_options(ffx_vulkan_fsr3_upscaler PRIVATE -w)
    # Upstream's Win32-only helpers (wcscpy_s etc.) for the AMD C++ sources, as the port does.
    get_target_property(_ffx_sources ffx_vulkan_fsr3_upscaler SOURCES)
    foreach(_ffx_src IN LISTS _ffx_sources)
        if (_ffx_src MATCHES "/sdk/src/.*\\.cpp$")
            set_source_files_properties("${_ffx_src}" PROPERTIES COMPILE_FLAGS
                "-include \"${CMAKE_CURRENT_SOURCE_DIR}/src/ffx_1_1_4_portability.hpp\"")
        endif()
    endforeach()
endif()

# The AMD host headers (jitter helpers ffxFsr3UpscalerGetJitterPhaseCount/Offset) for consumers that want to
# cross-check their own jitter sequence against AMD's; not needed for the portable API itself.
add_library(ffx_vulkan_fsr3_host_headers INTERFACE)
if (MSVC)
    target_include_directories(ffx_vulkan_fsr3_host_headers INTERFACE "${FFX_1_1_4_ROOT}/include")
else()
    # SYSTEM: the AMD headers carry MSVC pragmas and unused statics that suyu's -Werror flags would reject
    target_include_directories(ffx_vulkan_fsr3_host_headers SYSTEM INTERFACE "${FFX_1_1_4_ROOT}/include")
endif()
add_library(ffx-vulkan::fsr3-host-headers ALIAS ffx_vulkan_fsr3_host_headers)

# SPDX-FileCopyrightText: 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Offline check that every program in a shader package translates (Maxwell -> SPIR-V).
add_executable(shader-scan shader_scan.cpp)
target_link_libraries(shader-scan PRIVATE common shader_recompiler)

# Headless Vulkan check: build a real graphics pipeline for every program of a package on
# this machine's GPU, through the same device/profile/layout code the Vulkan backend uses.
add_executable(pipeline-test pipeline_test.cpp)
target_link_libraries(pipeline-test PRIVATE common shader_recompiler video_core)

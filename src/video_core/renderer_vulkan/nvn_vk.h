// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <vulkan/vulkan.h>

namespace Vulkan {

class Device;
class MemoryAllocator;

/// G3c: our own Vulkan renderer for the game's NVN calls (core/arm/recomp/nvn_rec). In shadow mode
/// (game_settings.ini [Debug] nvn_vulkan = shadow) it renders the same frames into images of its own
/// beside suyu's path; nothing of it is shown. Creates nothing unless that mode is on.
void NvnVkStart(const Device& device, MemoryAllocator& allocator);
void NvnVkStop();
/// Waits until the render thread has replayed everything the game submitted so far (no-op without the thread).
void NvnVkWaitRenderIdle();
/// Writes an RGBA8 image as a PNG into suyu's user directory (diagnostics).
/// Present mode (nvn_vulkan = present): the newest frame the game presented, as a finished copy in
/// GENERAL layout that suyu's compositor may read. False until the first frame exists.
bool NvnVkPresentImage(VkImage& image, unsigned& width, unsigned& height);
/// Frame counter shared by the comparison dumps (ticked once per suyu composite).
unsigned NvnVkSuyuFrame();
void NvnVkSuyuFrameTick();
/// True during the one frame after user\capture.txt appeared: the comparison dumps run then.
bool NvnVkCaptureActive();
/// Appends text to a file in suyu's user directory (comparison dumps).
void NvnVkDumpLine(const char* file, const std::string& text);
/// Present-path probe (logging only, present mode): stage 0 at Composite entry, 1 after the present.
void NvnVkCompositeProbe(int stage);
/// Shared probe clock (steady_clock, ms) so all wiggle-hitch logs line up.
double NvnVkNowMs();
void NvnVkSavePng(const char* name, unsigned w, unsigned h, const unsigned char* rgba);

} // namespace Vulkan

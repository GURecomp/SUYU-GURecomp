// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core::PipelineManifest {

/**
 * Pipeline manifests (G2): a game's Vulkan pipeline cache with no game code in it. Each cached
 * shader is stored as an offset and length into the game's own shader package (an MT Framework
 * archive entry in the RomFS), next to the numbers the translator needs (texture types,
 * constant buffer values, environment words) and the pipeline key. Made from play sessions by
 * tools/a32recomp/pipeline_manifest.py, shipped with suyu, and turned back into cache entries
 * at export time from the player's own copy of the game, so every pipeline anyone has met is
 * built at boot instead of mid-game.
 */
struct Result {
    bool ok{};
    size_t pipelines{};  // in the manifest
    size_t added{};      // new to the cache file
    std::string message; // why not, when !ok
};

/// CRC-32 (zlib's), continued from `crc`.
u32 Crc32(u32 crc, const u8* data, size_t size);

/// Rebuilds the manifest's pipelines from the shader package inside `romfs` and adds the ones
/// the cache file doesn't have yet (creating it if needed). The package must be the one the
/// manifest was made from (size + CRC-32 checked).
Result Apply(const std::filesystem::path& manifest, const FileSys::VirtualFile& romfs,
             const std::filesystem::path& cache_file);

} // namespace Core::PipelineManifest

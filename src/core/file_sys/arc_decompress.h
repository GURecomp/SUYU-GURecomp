// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

/**
 * A read-only view of a RomFS tree in which every MT Framework archive ("ARC\0", 80-byte entries,
 * zlib-compressed payloads) is presented with its entries stored uncompressed: each payload is
 * inflated and rewritten as a zlib stream of stored deflate blocks (the engine always inflates
 * entries, raw ones fail to load), repacked in the original order. Archives are
 * inflated only when read and one at a time, so CreateRomFS over this view streams a fully
 * unpacked RomFS without a temporary copy. Anything that isn't a well-formed archive passes
 * through unchanged.
 *
 * Used by the game export's "decompress game archives" option, so inflating an entry while
 * loading is a plain copy. The export keeps this off by default.
 */
VirtualDir DecompressArchives(VirtualDir root);

} // namespace FileSys

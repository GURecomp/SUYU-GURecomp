// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include <zlib.h>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/file_sys/arc_decompress.h"

namespace FileSys {

namespace {

constexpr size_t kHeader = 12;
constexpr size_t kEntry = 80;
constexpr u32 kSizeMask = 0x1fffffff; // size field: top 3 bits are flags, kept as they are
constexpr size_t kStoredBlock = 65535;  // largest deflate stored block

// MT Framework always runs an entry through zlib (raw payloads fail to load), so unpacked entries
// stay zlib streams made of stored (uncompressed) deflate blocks: inflating those is a copy.
size_t StoredSize(size_t n) {
    const size_t blocks = n == 0 ? 1 : (n + kStoredBlock - 1) / kStoredBlock;
    return 2 + n + 5 * blocks + 4; // zlib header, blocks with 5-byte headers, adler32
}

void WriteStored(u8* out, const u8* data, size_t n) {
    *out++ = 0x78; // deflate, 32K window
    *out++ = 0x01; // no compression; (0x78 << 8 | 0x01) % 31 == 0
    size_t done = 0;
    do {
        const size_t len = std::min(kStoredBlock, n - done);
        const bool last = done + len == n;
        *out++ = last ? 1 : 0; // BFINAL, BTYPE 00 = stored
        *out++ = static_cast<u8>(len);
        *out++ = static_cast<u8>(len >> 8);
        *out++ = static_cast<u8>(~len);
        *out++ = static_cast<u8>(~len >> 8);
        std::memcpy(out, data + done, len);
        out += len;
        done += len;
    } while (done < n);
    const uLong adler = adler32(adler32(0, nullptr, 0), data, static_cast<uInt>(n));
    *out++ = static_cast<u8>(adler >> 24);
    *out++ = static_cast<u8>(adler >> 16);
    *out++ = static_cast<u8>(adler >> 8);
    *out++ = static_cast<u8>(adler);
}

struct Entry {
    u32 comp;
    u32 size_flags;
    u32 offset;     // in the source archive
    u32 new_offset; // in the unpacked one
    u32 new_comp;   // stored size in the unpacked one
};

class UnpackedArcFile final : public VfsFile {
public:
    UnpackedArcFile(VirtualFile src_, std::vector<u8> table_, std::vector<Entry> entries_,
                    size_t data_start_, size_t size_)
        : src{std::move(src_)}, table{std::move(table_)}, entries{std::move(entries_)},
          data_start{data_start_}, size{size_} {}

    std::string GetName() const override {
        return src->GetName();
    }
    std::size_t GetSize() const override {
        return size;
    }
    bool Resize(std::size_t) override {
        return false;
    }
    VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= size) {
            return 0;
        }
        length = std::min(length, size - offset);
        std::scoped_lock lk{lock};
        if (buffer.empty() && !Unpack()) {
            return 0;
        }
        std::memcpy(data, buffer.data() + offset, length);
        if (offset + length == size) {
            // The RomFS builder reads each file front to back once: done with this one.
            buffer.clear();
            buffer.shrink_to_fit();
        }
        return length;
    }

private:
    bool Unpack() const {
        buffer.assign(size, 0);
        // Header and entry table (already rewritten), then whatever filled the gap up to the
        // first payload in the source (padding).
        std::memcpy(buffer.data(), table.data(), table.size());
        if (data_start > table.size()) {
            src->Read(buffer.data() + table.size(), data_start - table.size(), table.size());
        }
        std::vector<u8> comp;
        std::vector<u8> plain;
        for (const Entry& e : entries) {
            const u32 out_size = e.size_flags & kSizeMask;
            if (e.comp == out_size) { // already stored uncompressed: copied as it is
                if (src->Read(buffer.data() + e.new_offset, e.comp, e.offset) != e.comp) {
                    return false;
                }
                continue;
            }
            comp.resize(e.comp);
            if (src->Read(comp.data(), e.comp, e.offset) != e.comp) {
                return false;
            }
            plain.resize(out_size);
            uLongf len = out_size;
            if (uncompress(plain.data(), &len, comp.data(), e.comp) != Z_OK || len != out_size) {
                LOG_ERROR(Loader, "{}: an archive entry didn't inflate", src->GetName());
                return false;
            }
            WriteStored(buffer.data() + e.new_offset, plain.data(), out_size);
        }
        return true;
    }

    VirtualFile src;
    std::vector<u8> table; // header + entry table with sizes and offsets rewritten
    std::vector<Entry> entries;
    size_t data_start;
    size_t size;
    mutable std::mutex lock;
    mutable std::vector<u8> buffer;
};

VirtualFile MaybeUnpack(const VirtualFile& file) {
    if (!file || file->GetExtension() != "arc" || file->GetSize() < kHeader) {
        return file;
    }
    u8 head[kHeader];
    if (file->Read(head, kHeader, 0) != kHeader || std::memcmp(head, "ARC\0", 4) != 0) {
        return file;
    }
    u16 count;
    std::memcpy(&count, head + 6, 2);
    const size_t table_size = kHeader + size_t{count} * kEntry;
    if (count == 0 || table_size > file->GetSize()) {
        return file;
    }
    std::vector<u8> table(table_size);
    if (file->Read(table.data(), table_size, 0) != table_size) {
        return file;
    }
    std::vector<Entry> entries(count);
    size_t data_start = file->GetSize();
    bool compressed = false;
    for (u32 k = 0; k < count; ++k) {
        Entry& e = entries[k];
        const u8* t = table.data() + kHeader + k * kEntry;
        std::memcpy(&e.comp, t + 68, 4);
        std::memcpy(&e.size_flags, t + 72, 4);
        std::memcpy(&e.offset, t + 76, 4);
        e.new_comp = e.comp;
        if (size_t{e.offset} + e.comp > file->GetSize() || e.offset < table_size) {
            return file; // not the layout we know
        }
        compressed |= e.comp != (e.size_flags & kSizeMask);
        data_start = std::min<size_t>(data_start, e.offset);
    }
    if (!compressed) {
        return file;
    }
    // Payloads keep their source order and 16-byte alignment, starting where the first did.
    // Compressed entries become stored zlib streams; anything else is copied as it is.
    std::vector<u32> order(count);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return entries[a].offset < entries[b].offset; });
    size_t at = data_start;
    for (const u32 k : order) {
        Entry& e = entries[k];
        const u32 out_size = e.size_flags & kSizeMask;
        if (e.comp != out_size) {
            const size_t stored = StoredSize(out_size);
            if (stored > 0xffffffffu) {
                return file;
            }
            e.new_comp = static_cast<u32>(stored);
        }
        e.new_offset = static_cast<u32>(at);
        at += e.new_comp;
        at = (at + 15) & ~size_t{15};
        if (at > 0xffffffffu) {
            return file; // doesn't fit 32-bit offsets
        }
        u8* t = table.data() + kHeader + k * kEntry;
        std::memcpy(t + 68, &e.new_comp, 4);
        std::memcpy(t + 76, &e.new_offset, 4);
    }
    return std::make_shared<UnpackedArcFile>(file, std::move(table), std::move(entries),
                                             data_start, at);
}

class UnpackedDir final : public ReadOnlyVfsDirectory {
public:
    explicit UnpackedDir(VirtualDir src_) : src{std::move(src_)} {}

    std::vector<VirtualFile> GetFiles() const override {
        auto files = src->GetFiles();
        for (auto& f : files) {
            f = MaybeUnpack(f);
        }
        return files;
    }
    std::vector<VirtualDir> GetSubdirectories() const override {
        auto dirs = src->GetSubdirectories();
        for (auto& d : dirs) {
            d = std::make_shared<UnpackedDir>(d);
        }
        return dirs;
    }
    std::string GetName() const override {
        return src->GetName();
    }
    VirtualDir GetParentDirectory() const override {
        return nullptr;
    }

private:
    VirtualDir src;
};

} // namespace

VirtualDir DecompressArchives(VirtualDir root) {
    return root ? std::make_shared<UnpackedDir>(std::move(root)) : nullptr;
}

} // namespace FileSys

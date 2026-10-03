// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <zlib.h>

#include "common/common_types.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/pipeline_manifest.h"

namespace Core::PipelineManifest {

namespace {

// Formats as written by tools/a32recomp/pipeline_manifest.py (manifest) and
// video_core/shader_environment.cpp (cache).
constexpr std::array<char, 4> kManifestMagic{'S', 'P', 'M', 'F'};
constexpr u32 kManifestVersion = 1;
constexpr std::array<char, 8> kCacheMagic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr size_t kSph = 0x50;
// What followed a program in GPU memory when the cached code is longer than the program.
constexpr std::array<u8, 16> kTerminator{0x0f, 0x00, 0x87, 0xff, 0xff, 0x0f, 0x40, 0xe2,
                                         0x34, 0x12, 0x76, 0x98, 0x01, 0x00, 0x00, 0x00};
constexpr u32 kStageGeometry = 3;
constexpr u32 kStageCompute = 5;
constexpr size_t kComputeKeySize = 24; // ComputePipelineCacheKey

using Bytes = std::vector<u8>;

/// Bounds-checked little endian reader.
struct Reader {
    const Bytes& d;
    size_t p{};
    bool bad{};
    bool Has(size_t n) const {
        return !bad && p + n <= d.size();
    }
    template <typename T>
    T Get() {
        T v{};
        if (!Has(sizeof(T))) {
            bad = true;
            return v;
        }
        std::memcpy(&v, d.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    const u8* Take(size_t n) {
        if (!Has(n)) {
            bad = true;
            return nullptr;
        }
        const u8* at = d.data() + p;
        p += n;
        return at;
    }
};

std::optional<Bytes> ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

/// The named entry of an MT Framework archive ("ARC\0", 80-byte entries), inflated.
std::optional<Bytes> ArcEntry(const Bytes& arc, std::string_view name) {
    if (arc.size() < 12 || std::memcmp(arc.data(), "ARC\0", 4) != 0) {
        return std::nullopt;
    }
    u16 count;
    std::memcpy(&count, arc.data() + 6, 2);
    for (u32 k = 0; k < count; ++k) {
        const size_t e = 12 + size_t{k} * 80;
        if (e + 80 > arc.size()) {
            return std::nullopt;
        }
        const char* entry_name = reinterpret_cast<const char*>(arc.data() + e);
        if (std::string_view(entry_name, strnlen(entry_name, 64)) != name) {
            continue;
        }
        u32 comp, size_flags, offset;
        std::memcpy(&comp, arc.data() + e + 68, 4);
        std::memcpy(&size_flags, arc.data() + e + 72, 4);
        std::memcpy(&offset, arc.data() + e + 76, 4);
        const u32 size = size_flags & 0x1fffffff; // top bits are flags
        if (size_t{offset} + comp > arc.size()) {
            return std::nullopt;
        }
        Bytes out(size);
        uLongf out_len = size;
        if (uncompress(out.data(), &out_len, arc.data() + offset, comp) != Z_OK || out_len != size) {
            return std::nullopt;
        }
        return out;
    }
    return std::nullopt;
}

/// Splits a pipeline cache file into its entries (after the 12-byte header): graphics
/// pipelines with keys of `key_size` bytes, and compute ones.
std::optional<std::vector<Bytes>> CacheEntries(const Bytes& d, size_t key_size) {
    std::vector<Bytes> out;
    Reader r{d, 12};
    while (r.p < d.size()) {
        const size_t begin = r.p;
        const u32 envs = r.Get<u32>();
        u32 stage = 0;
        for (u32 i = 0; i < envs && !r.bad; ++i) {
            const u64 code = r.Get<u64>();
            const u64 ntt = r.Get<u64>(), ntpf = r.Get<u64>(), ncb = r.Get<u64>(),
                      ncr = r.Get<u64>();
            r.Take(6 * 4);
            stage = r.Get<u32>();
            r.Take(static_cast<size_t>(code + ntt * 8 + ntpf * 8 + ncb * 12 + ncr * 12));
            if (stage == kStageCompute) {
                r.Take(12 + 4);
            } else {
                r.Take(kSph);
                if (stage == kStageGeometry) {
                    r.Take(32);
                }
            }
        }
        r.Take(envs == 1 && stage == kStageCompute ? kComputeKeySize : key_size);
        if (r.bad) {
            return std::nullopt;
        }
        out.emplace_back(d.begin() + begin, d.begin() + r.p);
    }
    return out;
}

struct BytesHash {
    size_t operator()(const Bytes& b) const noexcept {
        u64 h = 0xcbf29ce484222325ull;
        for (const u8 c : b) {
            h = (h ^ c) * 0x100000001b3ull;
        }
        return static_cast<size_t>(h);
    }
};

Result Fail(std::string why) {
    Result r;
    r.message = std::move(why);
    return r;
}

} // namespace

u32 Crc32(u32 crc, const u8* data, size_t size) {
    while (size != 0) {
        const uInt n = static_cast<uInt>(std::min<size_t>(size, 1u << 30));
        crc = static_cast<u32>(crc32(crc, data, n));
        data += n;
        size -= n;
    }
    return crc;
}

Result Apply(const std::filesystem::path& manifest, const FileSys::VirtualFile& romfs,
             const std::filesystem::path& cache_file) {
    const auto m = ReadFile(manifest);
    if (!m) {
        return Fail("manifest not readable");
    }
    Reader r{*m};
    const u8* magic = r.Take(4);
    if (!magic || std::memcmp(magic, kManifestMagic.data(), 4) != 0) {
        return Fail("not a pipeline manifest");
    }
    const u32 version = r.Get<u32>(), cache_version = r.Get<u32>(), key_size = r.Get<u32>(),
              spk_size = r.Get<u32>(), spk_crc = r.Get<u32>(), count = r.Get<u32>();
    if (r.bad || version != kManifestVersion) {
        return Fail(fmt::format("manifest version {} not supported", version));
    }
    std::array<std::string, 2> source; // archive path in the RomFS, entry name
    for (auto& s : source) {
        const u16 n = r.Get<u16>();
        const u8* at = r.Take(n);
        if (!at) {
            return Fail("manifest truncated");
        }
        s.assign(reinterpret_cast<const char*>(at), n);
    }

    if (!romfs) {
        return Fail("no RomFS");
    }
    const auto root = FileSys::ExtractRomFS(romfs);
    const auto arc_file = root ? root->GetFileRelative(source[0]) : nullptr;
    if (!arc_file) {
        return Fail(fmt::format("{} not in the RomFS", source[0]));
    }
    const auto arc_vec = arc_file->ReadAllBytes();
    const Bytes arc(arc_vec.begin(), arc_vec.end());
    const auto spk = ArcEntry(arc, source[1]);
    if (!spk) {
        return Fail(fmt::format("{} not readable in {}", source[1], source[0]));
    }
    const u32 crc = static_cast<u32>(crc32(0, spk->data(), static_cast<uInt>(spk->size())));
    if (spk->size() != spk_size || crc != spk_crc) {
        return Fail("the game's shader package differs from the one the manifest was made from "
                    "(other game version?)");
    }

    // Manifest records -> cache entries.
    std::vector<Bytes> entries;
    entries.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        Bytes e;
        const auto put = [&e](const void* p, size_t n) {
            const u8* b = static_cast<const u8*>(p);
            e.insert(e.end(), b, b + n);
        };
        const u32 stages = r.Get<u32>();
        put(&stages, 4);
        for (u32 s = 0; s < stages; ++s) {
            const u32 off = r.Get<u32>(), code_size = r.Get<u32>(), prog = r.Get<u32>();
            const u8 same_sph = r.Get<u8>();
            const u8* counts = r.Take(32);
            const u8* words = r.Take(28);
            if (r.bad || prog > code_size || size_t{off} + prog > spk->size() ||
                (prog != code_size && code_size - prog != kTerminator.size())) {
                return Fail("manifest truncated or inconsistent");
            }
            u64 n[4];
            std::memcpy(n, counts, sizeof(n));
            const u8* tables = r.Take(static_cast<size_t>(n[0] * 8 + n[1] * 8 + n[2] * 12 + n[3] * 12));
            if (!tables && (n[0] | n[1] | n[2] | n[3]) != 0) {
                return Fail("manifest truncated");
            }
            Bytes code(spk->begin() + off, spk->begin() + off + prog);
            if (prog != code_size) {
                code.insert(code.end(), kTerminator.begin(), kTerminator.end());
            }
            const u64 code_size64 = code_size;
            put(&code_size64, 8);
            put(counts, 32);
            put(words, 28);
            put(code.data(), code.size());
            put(tables, static_cast<size_t>(n[0] * 8 + n[1] * 8 + n[2] * 12 + n[3] * 12));
            if (same_sph) {
                put(code.data(), std::min<size_t>(kSph, code.size()));
            } else {
                const u8* sph = r.Take(kSph);
                if (!sph) {
                    return Fail("manifest truncated");
                }
                put(sph, kSph);
            }
        }
        const u8* key = r.Take(key_size);
        if (!key) {
            return Fail("manifest truncated");
        }
        put(key, key_size);
        entries.push_back(std::move(e));
    }

    // Merge into the cache file: keep what's there, add what isn't.
    Bytes out;
    std::unordered_set<Bytes, BytesHash> have;
    if (const auto existing = ReadFile(cache_file); existing && existing->size() >= 12) {
        u32 existing_version;
        std::memcpy(&existing_version, existing->data() + 8, 4);
        const bool same_format = std::memcmp(existing->data(), kCacheMagic.data(), 8) == 0 &&
                                 existing_version == cache_version;
        if (same_format) {
            if (const auto list = CacheEntries(*existing, key_size)) {
                have.insert(list->begin(), list->end());
                out = *existing;
            }
        }
    }
    if (out.empty()) {
        out.insert(out.end(), kCacheMagic.begin(), kCacheMagic.end());
        const auto* v = reinterpret_cast<const u8*>(&cache_version);
        out.insert(out.end(), v, v + 4);
    }
    Result result;
    result.pipelines = entries.size();
    for (auto& e : entries) {
        if (have.insert(e).second) {
            out.insert(out.end(), e.begin(), e.end());
            ++result.added;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_file.parent_path(), ec);
    std::ofstream f(cache_file, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
        return Fail("cannot write the pipeline cache");
    }
    result.ok = true;
    return result;
}

} // namespace Core::PipelineManifest

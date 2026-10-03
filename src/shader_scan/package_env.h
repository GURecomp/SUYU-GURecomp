// SPDX-FileCopyrightText: 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Shared by shader-scan and pipeline-test: a shader recompiler environment over one
// program taken out of a shader package, and the package/index loader.

#pragma once

#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/program_header.h"

namespace ShaderScan {

constexpr u32 kSphSize = sizeof(Shader::ProgramHeader);
// Maxwell puts a scheduling word every 32 bytes of absolute address. Real programs start
// at an address = 0x10 mod 0x20 so the code after the 0x50-byte header is 32-byte aligned;
// place each program the same way.
constexpr u32 kProgramBase = 0x10;

// Anything a real draw would supply (texture types, constant buffer contents) gets a
// neutral default.
class PackageEnvironment final : public Shader::Environment {
public:
    PackageEnvironment(std::vector<u64> code_, Shader::Stage stage_) : code{std::move(code_)} {
        std::memcpy(&sph, code.data(), kSphSize);
        stage = stage_;
        start_address = kProgramBase;
        local_memory_size =
            static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
    }

    u64 ReadInstruction(u32 address) override {
        if (address < kProgramBase) {
            return 0;
        }
        const u32 index = (address - kProgramBase) / sizeof(u64);
        return index < code.size() ? code[index] : 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 2;
    }
    u32 LocalMemorySize() const override {
        return local_memory_size;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::vector<u64> code;
    u32 local_memory_size{};
};

struct PackageProgram {
    size_t n;            // unique program number (first-seen order, as in info.tsv)
    std::string offset;  // hex offset of the program header in the package
    bool is_fragment;
    u32 bytes;
    std::vector<u64> code; // padded with trailing zeros so the CFG never reads past the end
};

/// Reads the package and its index ("<hex offset>\t<sph type>\t<bytes>" per line) and
/// returns each distinct program once. `total` receives the number of index lines, `bad`
/// the lines pointing outside the package.
inline std::vector<PackageProgram> LoadUniquePrograms(const char* package, const char* index,
                                                      size_t& total,
                                                      std::vector<std::string>& bad) {
    std::ifstream pkg_file(package, std::ios::binary);
    const std::string pkg((std::istreambuf_iterator<char>(pkg_file)), {});
    std::ifstream index_file(index);
    std::map<std::string, size_t> seen;
    std::vector<PackageProgram> out;
    total = 0;
    std::string line;
    while (std::getline(index_file, line)) {
        std::istringstream ls(line);
        std::string off_hex;
        u32 sph_type = 0, bytes = 0;
        if (!(ls >> off_hex >> sph_type >> bytes)) {
            continue;
        }
        ++total;
        const size_t offset = std::stoull(off_hex, nullptr, 16);
        if (offset + bytes > pkg.size() || bytes < kSphSize + 8) {
            bad.push_back(off_hex);
            continue;
        }
        std::string blob = pkg.substr(offset, bytes);
        if (seen.contains(blob)) {
            continue;
        }
        seen.emplace(blob, out.size());
        std::vector<u64> code((blob.size() + 7) / 8 + 8, 0);
        std::memcpy(code.data(), blob.data(), blob.size());
        out.push_back({out.size(), off_hex, sph_type == 5, bytes, std::move(code)});
    }
    return out;
}

} // namespace ShaderScan

// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}

namespace Core::NvnRec {

/**
 * NVN recorder (G3c). Every NVN call the renderer needs is captured as plain data (register
 * arguments, stack words, VFP arguments and a snapshot of the state object it points at) into
 * a list per command buffer. A queue submit hands the lists to the sink (our Vulkan renderer).
 * Nothing here knows about Maxwell: the lists are what the game asked for, in NVN terms.
 */
enum class Op : u16 {
    Begin,
    BindProgram,
    BindUniform,
    BindTexture,
    BindVertexBuffer,
    BindVertexAttrib,
    BindVertexStream,
    BindBlend,
    BindColor,
    BindChannelMask,
    BindDepthStencil,
    BindPolygon,
    BindMultisample,
    SetDepthClamp,
    SetViewport,
    SetViewports,
    SetScissor,
    SetScissors,
    SetRenderTargets,
    DrawArrays,
    DrawElements,
    ClearColor,
    ClearDepthStencil,
    SetStencilMask,
    SetStencilRef,
    SetStencilValueMask,
    SetBlendColor,
    SetAlphaRef,
    SetPolygonOffsetClamp,
    SetTexturePool,
    SetSamplerPool,
    Barrier,
    SetPrimitiveRestart,
    SetLineWidth,
    SetPointSize,
    Count,
};

struct Rec {
    Op op;
    u16 snap_words;
    u32 r[4];     // r0..r3 as the call started (r[0] = command buffer)
    u32 sp[4];    // first stack words
    u32 s[4];     // VFP s0..s3 as the call started
    u32 snap[16]; // words of the object the call points at (per op, see nvn_rec.cpp)
    u64 program[2]{}; // BindProgram: code addresses of the vertex and pixel programs
    u32 pobj[12]{};   // BindProgram diagnostics: first words of the program object
    u32 pblock[40]{}; // BindProgram diagnostics: the command words the object points at
    u32 pblock_n{};
};

struct List {
    u32 cmd{};
    std::vector<Rec> recs;
    bool handed{}; // recorder-internal: the records went to a submit and no BeginRecording followed yet
};

/// Called with the lists of one queue submit, in submit order (on the game thread). The lists own their records
/// (handed over, not shared with the recorder).
using Sink = void (*)(Memory::Memory& mem, std::vector<std::unique_ptr<List>>&& lists, u32 queue);
void SetSink(Sink sink);
/// Optional: gives replayed lists back so their record buffers are reused by the next submits (no 2 MB allocation
/// per submit). Frees what the pool does not keep. Any thread.
void RecycleLists(std::vector<std::unique_ptr<List>>&& lists);

/// Called when the game presents a window texture (texture object address).
using PresentSink = void (*)(Memory::Memory& mem, u32 texture);
void SetPresentSink(PresentSink sink);

/// Guest memory of a memory pool object (from MemoryPoolBuilderSetStorage).
bool PoolMemory(u32 pool_obj, u32& cpu, u32& size);
/// Guest address of descriptor `id` (32 bytes each) in a texture / sampler pool object.
bool TextureDescriptor(u32 pool, u32 id, u32& cpu);
bool SamplerDescriptor(u32 pool, u32 id, u32& cpu);
/// Guest memory ranges (address, bytes) written through NVN texture calls since the last call.
void TakeDirty(std::vector<std::pair<u32, u32>>& out);

/// Registers the recorder's entry points with NvnTrace (before guest code runs).
void RegisterAll();
/// nvn_vulkan = off: only the present hook that feeds the Run M frame probe (records nothing).
void RegisterPresentProbeOnly();

/// The recorder instead of a native word builder: called first by the native entry (any
/// outcome), so nothing is missed when the native declines.
void OnCall(const char* name, u32* regs, Memory::Memory& mem);
/// The same with the name resolved once (ResolveCall; -1 = an entry the recorder does not record).
int ResolveCall(const char* name);
void OnCallResolved(int index, const char* name, u32* regs, Memory::Memory& mem);

void Flush(const std::filesystem::path& file);

/// Guest memory behind a GPU virtual address: pools known from MemoryPoolInitialize.
/// Returns the CPU address and the bytes available from it, or false.
bool GpuToCpu(Memory::Memory& mem, u64 gpu_va, u32& cpu, u32& avail);
/// A vertex attribute format code learned from the guest driver's own words (code -> Maxwell word >> 20).
bool LearnedAttrib(u32 code, u32& out);
/// Diagnostic: one line per pool with the VA words at object +24..+64.
void LogPools(Memory::Memory& mem, std::vector<std::string>& out);

} // namespace Core::NvnRec

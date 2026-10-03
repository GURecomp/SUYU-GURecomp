// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <memory>
#include <mutex>

#include "core/arm/recomp/recomp_hooks.h"

namespace Core::RecompHooks {

namespace {
std::atomic<bool> g_init{false};
u32 g_lo{};
u32 g_span{};
std::unique_ptr<std::atomic<u8>[]> g_bits;
} // namespace

void Init(u32 lo, u32 span) {
    static std::once_flag once;
    std::call_once(once, [&] {
        g_bits = std::make_unique<std::atomic<u8>[]>(span / 32 + 1);
        g_lo = lo;
        g_span = span;
        g_init.store(true);
    });
}

void Mark(u32 pc) {
    if (!g_init.load()) {
        return;
    }
    const u32 off = pc - g_lo;
    if (off >= g_span) {
        return;
    }
    g_bits[off >> 5].fetch_or(static_cast<u8>(1u << ((off >> 2) & 7)));
}

const volatile u8* Bits() {
    static_assert(sizeof(std::atomic<u8>) == 1);
    return g_init.load() ? reinterpret_cast<const volatile u8*>(g_bits.get()) : nullptr;
}

u32 Lo() {
    return g_lo;
}

u32 Span() {
    return g_init.load() ? g_span : 0;
}

} // namespace Core::RecompHooks

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"

/// mkw60: records which guest instructions the JIT translates while active. With the code cache
/// cleared at the start, that is every instruction that runs in the window, so two windows in
/// different game states (e.g. MKW minimap on / off) differ in the code only one of them runs.
/// Driven by the debug server tool code_trace.
namespace Core::Nx2CodeTrace {

inline std::atomic<bool> g_active{false};
inline std::mutex g_mutex;
inline std::unordered_set<u64> g_addresses;

inline void Note(u64 vaddr) {
    std::scoped_lock lock{g_mutex};
    g_addresses.insert(vaddr);
}

inline void Start() {
    std::scoped_lock lock{g_mutex};
    g_addresses.clear();
    g_active.store(true, std::memory_order_relaxed);
}

/// Stops recording and returns the instruction addresses, sorted.
inline std::vector<u64> Stop() {
    g_active.store(false, std::memory_order_relaxed);
    std::scoped_lock lock{g_mutex};
    std::vector<u64> out(g_addresses.begin(), g_addresses.end());
    g_addresses.clear();
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace Core::Nx2CodeTrace

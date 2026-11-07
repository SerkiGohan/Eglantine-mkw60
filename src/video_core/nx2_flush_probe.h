// SPDX-License-Identifier: GPL-3.0-or-later
// Serial part, flush work (fork mkw60, measurement only, EDEN_NX2_FLUSH_PROBE=1): what the
// "flush work" before every draw costs. RasterizerVulkan::Draw runs FlushWork (wakes the Vulkan
// worker every 32 draws, submits every 128) and MemoryManager::FlushCaching, which hands the
// guest memory ranges written since the last draw (constant buffer updates, inline uploads) to
// RasterizerVulkan::InnerInvalidation: texture, buffer, query and pipeline cache. Counts calls,
// ranges, bytes and time per part (time stamp counter). One "Flush probe" line every ~10 s.
#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace VideoCommon::Nx2FlushProbe {

enum Part : u32 { FlushWork, FlushCaching, Texture, Buffer, Query, Pipeline, NumParts };
inline constexpr std::array<const char*, NumParts> part_names{
    "FlushWork", "FlushCaching total", "texture cache", "buffer cache", "query cache", "pipeline cache"};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FLUSH_PROBE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

inline u64 Ticks() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct Stats {
    std::array<u64, NumParts> ticks{};
    u64 draws{}, flushes_with_work{}, ranges{}, bytes{}, invalidations_outside_draws{};
    u64 window_ticks{};
    s64 window_ns{};
};

inline Stats& Get() {
    static Stats stats;
    return stats;
}

/// True while RasterizerVulkan::Draw runs FlushCaching (back thread).
inline thread_local bool t_in_draw_flush = false;

inline void Add(Part part, u64 start) {
    Get().ticks[part] += Ticks() - start;
}

inline void NoteInvalidation(size_t ranges, u64 bytes) {
    auto& s = Get();
    if (!t_in_draw_flush) {
        ++s.invalidations_outside_draws;
        return;
    }
    ++s.flushes_with_work;
    s.ranges += ranges;
    s.bytes += bytes;
}

inline void NoteDraw() {
    auto& s = Get();
    ++s.draws;
    if ((s.draws & 4095) != 0) {
        return;
    }
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const u64 now_ticks = Ticks();
    if (s.window_ns == 0) {
        s = {};
        s.window_ns = now_ns;
        s.window_ticks = now_ticks;
        return;
    }
    const s64 window = now_ns - s.window_ns;
    if (window < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(window) / 1e9;
    const double ns_per_tick =
        static_cast<double>(window) / static_cast<double>(now_ticks - s.window_ticks);
    std::string parts;
    for (u32 i = 0; i < NumParts; ++i) {
        parts += fmt::format(" | {} {:.1f} ms/s", part_names[i],
                             static_cast<double>(s.ticks[i]) * ns_per_tick / 1e6 / seconds);
    }
    LOG_INFO(HW_GPU,
             "Flush probe: Draws {:.0f}/s, of which with invalidation {:.0f}/s ({:.1f} ranges, {:.0f} "
             "bytes per invalidation), invalidations outside draws {:.0f}/s{}",
             static_cast<double>(s.draws) / seconds,
             static_cast<double>(s.flushes_with_work) / seconds,
             s.flushes_with_work ? static_cast<double>(s.ranges) / static_cast<double>(s.flushes_with_work) : 0.0,
             s.flushes_with_work ? static_cast<double>(s.bytes) / static_cast<double>(s.flushes_with_work) : 0.0,
             static_cast<double>(s.invalidations_outside_draws) / seconds, parts);
    s = {};
    s.window_ns = now_ns;
    s.window_ticks = now_ticks;
}

} // namespace VideoCommon::Nx2FlushProbe

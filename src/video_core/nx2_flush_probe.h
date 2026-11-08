// SPDX-License-Identifier: GPL-3.0-or-later
// Serial part, flush work (fork mkw60, measurement only, EDEN_NX2_FLUSH_PROBE=1): what the
// "flush work" before every draw costs. RasterizerVulkan::Draw runs FlushWork (wakes the Vulkan
// worker every 32 draws, submits every 128) and MemoryManager::FlushCaching, which hands the
// guest memory ranges written since the last draw (constant buffer updates, inline uploads) to
// RasterizerVulkan::InnerInvalidation: texture, buffer, query and pipeline cache. Counts calls,
// ranges, bytes and time per part (time stamp counter). One "Flush probe" line every ~10 s.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
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

/// Flush optimisations (fork mkw60), bit mask: 1 = merge nearby ranges of a draw's invalidation,
/// 2 = buffer cache fast path for ranges already CPU-modified. Start value EDEN_NX2_FLUSH_OPT
/// (default 1: merging measured ~250 ns less per draw, A/B 8.11.2026), at run time
/// /tmp/eden_flush_opt (read every ~250 ms; for knob_sweep.py flush_opt).
inline std::atomic<u32> g_opt{[] {
    const char* value = std::getenv("EDEN_NX2_FLUSH_OPT");
    return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) & 3U : 1U;
}()};

inline u32 Opt() {
    static std::atomic<u64> calls{0};
    static std::atomic<s64> last_check{0};
    if ((calls.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) [[unlikely]] {
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now - last_check.load(std::memory_order_relaxed) >= 250) {
            last_check.store(now, std::memory_order_relaxed);
            if (std::FILE* file = std::fopen("/tmp/eden_flush_opt", "r")) {
                unsigned value = 0;
                if (std::fscanf(file, "%u", &value) == 1) {
                    const u32 next = value & 3U;
                    if (g_opt.exchange(next, std::memory_order_relaxed) != next) {
                        LOG_INFO(HW_GPU, "Flush opt: {} (1 = merge, 2 = fast path)", next);
                    }
                }
                std::fclose(file);
            }
        }
    }
    return g_opt.load(std::memory_order_relaxed);
}

/// True while RasterizerVulkan::Draw runs FlushCaching (back thread).
inline thread_local bool t_in_draw_flush = false;

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

struct ModeStats {
    std::array<u64, NumParts> ticks{};
    u64 draws{}, flushes_with_work{}, ranges{}, merged_ranges{}, bytes{}, fast_paths{};
};

struct Stats {
    std::array<ModeStats, 4> modes{}; ///< per Opt() value
    u64 invalidations_outside_draws{};
    u64 window_ticks{};
    s64 window_ns{};
};

inline Stats& Global() {
    static Stats stats;
    return stats;
}

inline ModeStats& Get() {
    return Global().modes[g_opt.load(std::memory_order_relaxed)];
}

/// Buffer cache fast path taken (back thread, draw flush).
inline void NoteFastPath() {
    ++Get().fast_paths;
}

inline void NoteMerged(size_t merged) {
    Get().merged_ranges += merged;
}

inline void Add(Part part, u64 start) {
    Get().ticks[part] += Ticks() - start;
}

inline void NoteInvalidation(size_t ranges, u64 bytes) {
    if (!t_in_draw_flush) {
        ++Global().invalidations_outside_draws;
        return;
    }
    auto& s = Get();
    ++s.flushes_with_work;
    s.ranges += ranges;
    s.bytes += bytes;
}

inline void NoteDraw() {
    auto& g = Global();
    ++Get().draws;
    static u64 counter = 0;
    if ((++counter & 4095) != 0) {
        return;
    }
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const u64 now_ticks = Ticks();
    if (g.window_ns == 0) {
        g = {};
        g.window_ns = now_ns;
        g.window_ticks = now_ticks;
        return;
    }
    const s64 window = now_ns - g.window_ns;
    if (window < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(window) / 1e9;
    const double ns_per_tick =
        static_cast<double>(window) / static_cast<double>(now_ticks - g.window_ticks);
    for (u32 mode = 0; mode < g.modes.size(); ++mode) {
        const ModeStats& s = g.modes[mode];
        if (s.draws == 0) {
            continue;
        }
        // Per draw (modes share the window, so per second would mix them).
        const double draws = static_cast<double>(s.draws);
        std::string parts;
        for (u32 i = 0; i < NumParts; ++i) {
            parts += fmt::format(" | {} {:.0f} ns", part_names[i],
                                 static_cast<double>(s.ticks[i]) * ns_per_tick / draws);
        }
        LOG_INFO(HW_GPU,
                 "Flush probe (opt {}): Draws {:.0f} ({:.0f}/s share), with invalidation {:.0f} % | "
                 "ranges per invalidation {:.2f} (merged {:.2f}), {:.0f} bytes | "
                 "fast path {:.2f} per invalidation | per draw:{}",
                 mode, draws, draws / seconds,
                 100.0 * static_cast<double>(s.flushes_with_work) / draws,
                 s.flushes_with_work ? static_cast<double>(s.ranges) / static_cast<double>(s.flushes_with_work) : 0.0,
                 s.flushes_with_work ? static_cast<double>(s.merged_ranges) / static_cast<double>(s.flushes_with_work) : 0.0,
                 s.flushes_with_work ? static_cast<double>(s.bytes) / static_cast<double>(s.flushes_with_work) : 0.0,
                 s.flushes_with_work ? static_cast<double>(s.fast_paths) / static_cast<double>(s.flushes_with_work) : 0.0,
                 parts);
    }
    LOG_INFO(HW_GPU, "Flush probe: invalidations outside draws {:.0f}/s",
             static_cast<double>(g.invalidations_outside_draws) / seconds);
    g = {};
    g.window_ns = now_ns;
    g.window_ticks = now_ticks;
}

} // namespace VideoCommon::Nx2FlushProbe

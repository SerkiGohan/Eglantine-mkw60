// SPDX-License-Identifier: GPL-3.0-or-later
// Fork mkw60, serial part "SynchronizeBuffer": skip the CPU-modified page walk of a buffer range
// that was synchronized before and whose tracker manager has seen no new CPU modification since
// (BufferCache::SynchronizeBuffer, WordManager::nx2_cpu_epoch, BufferBase::nx2_sync_*).
// Switch: start value EDEN_NX2_SYNC_SKIP (default 0 until measured), at run time /tmp/eden_sync_skip
// (0/1, read every ~250 ms; knob_sweep.py sync_skip). Measurement: EDEN_NX2_SYNC_PROBE=1, one
// "Sync probe" line per mode every ~10 s (calls, skipped, uploads, ns per call).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "common/common_types.h"
#include "common/logging.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace VideoCommon::Nx2SyncSkip {

inline std::atomic<u32> g_mode{[] {
    const char* const value = std::getenv("EDEN_NX2_SYNC_SKIP");
    return value != nullptr && value[0] == '1' ? 1U : 0U;
}()};

[[nodiscard]] inline u32 Mode() noexcept {
    static std::atomic<u64> calls{0};
    static std::atomic<s64> last_check{0};
    if ((calls.fetch_add(1, std::memory_order_relaxed) & 4095) == 0) [[unlikely]] {
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now - last_check.load(std::memory_order_relaxed) >= 250) {
            last_check.store(now, std::memory_order_relaxed);
            if (std::FILE* file = std::fopen("/tmp/eden_sync_skip", "r")) {
                int value = 0;
                if (std::fscanf(file, "%d", &value) == 1) {
                    const u32 next = value == 1 ? 1U : 0U;
                    if (g_mode.exchange(next, std::memory_order_relaxed) != next) {
                        LOG_INFO(HW_GPU, "Sync skip: {}", next);
                    }
                }
                std::fclose(file);
            }
        }
    }
    return g_mode.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool ProbeEnabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_SYNC_PROBE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

[[nodiscard]] inline u64 Ticks() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct Stats {
    std::array<u64, 2> calls{}, skipped{}, uploads{}, ticks{};
    u64 window_ticks{};
    s64 window_ms{};
    u64 notes{};
};

inline void Note(u32 mode, bool skipped, bool uploaded, u64 start) {
    static Stats s;
    const u64 now_ticks = Ticks();
    ++s.calls[mode];
    s.skipped[mode] += skipped ? 1 : 0;
    s.uploads[mode] += uploaded ? 1 : 0;
    s.ticks[mode] += now_ticks - start;
    if ((++s.notes & 16383) != 0) {
        return;
    }
    const s64 now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (s.window_ms == 0) {
        s = {};
        s.window_ms = now_ms;
        s.window_ticks = now_ticks;
        return;
    }
    if (now_ms - s.window_ms < 10'000) {
        return;
    }
    const double ns_per_tick =
        static_cast<double>(now_ms - s.window_ms) * 1e6 / static_cast<double>(now_ticks - s.window_ticks);
    for (u32 m = 0; m < 2; ++m) {
        if (s.calls[m] == 0) {
            continue;
        }
        const double calls = static_cast<double>(s.calls[m]);
        LOG_INFO(HW_GPU,
                 "Sync probe skip={}: {} calls, skipped {:.1f} %, with upload {:.2f} %, {:.0f} ns per call",
                 m, s.calls[m], 100.0 * static_cast<double>(s.skipped[m]) / calls,
                 100.0 * static_cast<double>(s.uploads[m]) / calls,
                 static_cast<double>(s.ticks[m]) * ns_per_tick / calls);
    }
    s = {};
    s.window_ms = now_ms;
    s.window_ticks = now_ticks;
}

} // namespace VideoCommon::Nx2SyncSkip

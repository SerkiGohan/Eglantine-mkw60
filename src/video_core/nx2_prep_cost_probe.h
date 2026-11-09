// SPDX-License-Identifier: GPL-3.0-or-later
// B2 step 1 (measurement only, EDEN_NX2_PREP_COST_PROBE=1, fork mkw60): how much of the draw
// preparation has to stay serial on the back thread (guest memory reads at draw time, uploads,
// submission) and how much could run on other threads (docs/mkw60-b2-plan.md).
//
// Reuses the draw zones of the GPU profile (Nx2Profile::Scope) but only inside a draw, only while
// EDEN_NX2_GPU_PROFILE is off, and with the time stamp counter instead of the wall clock: the
// zone profile's two clock reads per zone cost the back thread ~27 %. One "Prep cost probe" line
// every ~10 s with exclusive ms/s per zone, serial/parallel sums and the estimated probe cost.
//
// Included from nx2_profile.h after the zone list (uses Nx2Profile::Zone and zone_names).
#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/nx2_cache_guard.h"
#include "common/logging.h"
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace VideoCommon::Nx2PrepCost {

#if MKW60_PROBES
inline const bool g_timing = [] {
    const char* value = std::getenv("EDEN_NX2_PREP_COST_PROBE");
    return value != nullptr && value[0] == '1';
}();
/// Zone tracking runs for the timing and for the cache guard (B2 step 2).
inline const bool g_enabled = g_timing || Common::Nx2CacheGuard::g_enabled;
#else
inline constexpr bool g_timing = false; // probes not compiled in (common/nx2_probes.h)
inline constexpr bool g_enabled = false;
#endif

inline constexpr u32 None = ~0U;
inline constexpr u32 MaxZones = 64;

inline u64 Ticks() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Data {
    std::array<u64, MaxZones> ticks{}; ///< exclusive
    std::array<u64, MaxZones> count{};
    u32 current{None};
    u64 last{};
    u64 draws{};
    u64 scopes{};
    u64 window_ticks{};
    s64 window_ns{};
};

inline thread_local Data t_data;

/// Zones whose work has to stay on the back thread in draw order (plan, "Split between serial and parallel").
inline bool IsSerial(u32 zone) {
    using Nx2Profile::Zone;
    switch (static_cast<Zone>(zone)) {
    case Zone::SyncDescriptors: // TIC/TSC table from registers + guest memory
    case Zone::ReadHandles:     // handles, SSBO pointers, samplers (guest memory)
    case Zone::FillImageViews:  // TIC entries (guest memory) -> image views, texture up to date?
    case Zone::SyncBuffer:      // CPU-modified pages -> upload
    case Zone::TextureUpload:
    case Zone::Uniforms:        // uniform copy from guest memory into the ring
    case Zone::RenderTargets:   // render target lookup/creation, upload check
    case Zone::FlushWork:       // submission and cache invalidation
        return true;
    default:
        return false;
    }
}

#ifdef _MSC_VER
#define NX2_PREP_NOINLINE __declspec(noinline)
#else
#define NX2_PREP_NOINLINE __attribute__((noinline))
#endif

inline void Report(Data& d) {
    const u64 now_ticks = Ticks();
    const s64 now_ns = NowNs();
    if (d.window_ns == 0) {
        d.window_ticks = now_ticks;
        d.window_ns = now_ns;
        return;
    }
    const s64 window_ns = now_ns - d.window_ns;
    if (window_ns < 10'000'000'000LL) {
        return;
    }
    const double ns_per_tick =
        static_cast<double>(window_ns) / static_cast<double>(now_ticks - d.window_ticks);
    const double seconds = static_cast<double>(window_ns) / 1e9;
    const auto ms_per_s = [&](u64 ticks) {
        return static_cast<double>(ticks) * ns_per_tick / 1e6 / seconds;
    };
    // Cost of one time stamp read, to estimate what the probe itself adds.
    const u64 cal_start = Ticks();
    for (int i = 0; i < 1000; ++i) {
        (void)Ticks();
    }
    const double read_ns = static_cast<double>(Ticks() - cal_start) * ns_per_tick / 1000.0;

    u64 serial{}, parallel{};
    std::string serial_text, parallel_text;
    for (u32 zone = 0; zone < MaxZones && zone < Nx2Profile::zone_names.size(); ++zone) {
        if (d.ticks[zone] == 0) {
            continue;
        }
        const std::string item = fmt::format(" {} {:.1f}", Nx2Profile::zone_names[zone],
                                             ms_per_s(d.ticks[zone]));
        if (IsSerial(zone)) {
            serial += d.ticks[zone];
            serial_text += item;
        } else {
            parallel += d.ticks[zone];
            parallel_text += item;
        }
    }
    const double draws = static_cast<double>(d.draws) / seconds;
    const double scopes = static_cast<double>(d.scopes) / seconds;
    if (g_timing) {
        LOG_INFO(HW_GPU,
                 "Prep cost probe: Draws {:.0f}/s | draw total {:.1f} ms/s ({:.2f} us/draw) | serial "
                 "{:.1f} ms/s:{} | parallelizable {:.1f} ms/s:{} | zones {:.0f}/s, measurement overhead ~{:.1f} "
                 "ms/s (rdtsc {:.1f} ns)",
                 draws, ms_per_s(serial + parallel),
                 draws > 0 ? ms_per_s(serial + parallel) * 1000.0 / draws : 0.0, ms_per_s(serial),
                 serial_text, ms_per_s(parallel), parallel_text, scopes,
                 scopes * 2.0 * read_ns / 1e6, read_ns);
    }
    if (Common::Nx2CacheGuard::g_enabled) {
        namespace Guard = Common::Nx2CacheGuard;
        auto& counts = Guard::t_data.counts;
        std::string text;
        for (u32 site = 0; site < Guard::NumSites; ++site) {
            u64 serial_count{}, parallel_count{};
            std::string zones;
            for (u32 zone = 0; zone < Guard::MaxZones && zone < Nx2Profile::zone_names.size();
                 ++zone) {
                const u64 n = counts[site][zone];
                if (n == 0) {
                    continue;
                }
                if (IsSerial(zone)) {
                    serial_count += n;
                } else {
                    parallel_count += n;
                    zones += fmt::format(" {} {:.0f}", Nx2Profile::zone_names[zone],
                                         static_cast<double>(n) / seconds);
                }
            }
            if (serial_count == 0 && parallel_count == 0) {
                continue;
            }
            text += fmt::format(" | {}: serial {:.0f}/s, parallel {:.0f}/s{}{}",
                                Guard::site_names[site],
                                static_cast<double>(serial_count) / seconds,
                                static_cast<double>(parallel_count) / seconds,
                                zones.empty() ? "" : " (", zones.empty() ? "" : zones + ")");
        }
        LOG_INFO(HW_GPU, "Cache guard: Draws {:.0f}/s{}", draws, text);
        counts = {};
    }
    d.ticks = {};
    d.count = {};
    d.draws = 0;
    d.scopes = 0;
    d.window_ticks = Ticks();
    d.window_ns = NowNs();
}

/// Starts timing `zone` if the thread is inside a draw (or `zone` is the draw itself).
NX2_PREP_NOINLINE inline bool Begin(u32 zone, u32 draw_zone, u32& parent) {
    auto& d = t_data;
    if (d.current == None && zone != draw_zone) {
        return false;
    }
    const u64 now = Ticks();
    if (d.current != None) {
        d.ticks[d.current] += now - d.last;
    }
    Common::Nx2CacheGuard::Transition(d.current);
    Common::Nx2CacheGuard::t_zone = zone;
    parent = d.current;
    d.current = zone;
    d.last = now;
    ++d.scopes;
    return true;
}

NX2_PREP_NOINLINE inline void End(u32 zone, u32 draw_zone, u32 parent) {
    auto& d = t_data;
    const u64 now = Ticks();
    d.ticks[d.current] += now - d.last;
    ++d.count[zone];
    Common::Nx2CacheGuard::Transition(d.current);
    Common::Nx2CacheGuard::t_zone = parent;
    if (parent == None) {
        Common::Nx2CacheGuard::t_data.dirty_flags = nullptr;
    }
    d.current = parent;
    d.last = now;
    if (zone == draw_zone && parent == None) {
        ++d.draws;
        if ((d.draws & 1023) == 0) {
            Report(d);
        }
    }
}

#undef NX2_PREP_NOINLINE

} // namespace VideoCommon::Nx2PrepCost

// SPDX-License-Identifier: GPL-3.0-or-later
// M1 draw-repeat probe (measurement only, EDEN_NX2_DRAW_REPEAT_PROBE=1): how often does a draw use
// exactly the configuration of an earlier one, and what does preparing it cost?
// Per GraphicsPipeline::ConfigureImpl call (back thread):
//   input key  = pipeline, every enabled constant buffer (address, size) of every stage, vertex
//                streams, index buffer, render targets/zeta registers, and the raw texture (TIC)
//                indices the shaders read from the constant buffers;
//   output key = the image views and samplers the texture cache resolved them to.
// Counted: same input as the previous draw / as any draw in the last ~1M keys; for those whether the
// output was the same as last time (if not, resolving can't be skipped on the input alone); and the
// time ConfigureImpl took in each group. One "Draw repeat probe" line every 10 s.
#pragma once

#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::Nx2DrawRepeatProbe {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_DRAW_REPEAT_PROBE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline u64 Mix(u64 h, u64 v) {
    return h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
}

inline u64 HashBytes(u64 h, const void* data, size_t size) {
    return Mix(h, Common::CityHash64(static_cast<const char*>(data), size));
}

/// Back thread only.
struct State {
    std::unordered_map<u64, u64> last_output; ///< input key -> output key of its last draw
    u64 previous_input{};
    u64 draws{}, ns{};
    u64 same_as_previous{}, same_as_previous_ns{};
    u64 seen{}, seen_ns{}, seen_same_output{}, seen_same_output_ns{};
    u64 fresh{}, fresh_ns{};
    s64 last_report{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void Note(u64 input, u64 output, s64 start_ns) {
    auto& s = Get();
    const s64 now = NowNs();
    const u64 ns = static_cast<u64>(now - start_ns);
    ++s.draws;
    s.ns += ns;
    if (input == s.previous_input) {
        ++s.same_as_previous;
        s.same_as_previous_ns += ns;
    }
    s.previous_input = input;
    if (s.last_output.size() > (1U << 20)) {
        s.last_output.clear();
    }
    const auto [it, inserted] = s.last_output.try_emplace(input, output);
    if (inserted) {
        ++s.fresh;
        s.fresh_ns += ns;
    } else {
        ++s.seen;
        s.seen_ns += ns;
        if (it->second == output) {
            ++s.seen_same_output;
            s.seen_same_output_ns += ns;
        }
        it->second = output;
    }
    if (s.last_report == 0) {
        s.last_report = now;
        return;
    }
    if (now - s.last_report < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(now - s.last_report) / 1e9;
    s.last_report = now;
    const auto r = [seconds](u64& v) {
        const double x = static_cast<double>(v) / seconds;
        v = 0;
        return x;
    };
    const auto m = [seconds](u64& v) {
        const double x = static_cast<double>(v) / 1e6 / seconds;
        v = 0;
        return x;
    };
    const double draws = r(s.draws), ms = m(s.ns);
    const double prev = r(s.same_as_previous), prev_ms = m(s.same_as_previous_ns);
    const double seen = r(s.seen), seen_ms = m(s.seen_ns);
    const double same_out = r(s.seen_same_output), same_out_ms = m(s.seen_same_output_ns);
    const double fresh = r(s.fresh), fresh_ms = m(s.fresh_ns);
    LOG_INFO(Render_Vulkan,
             "Draw repeat probe: Draws {:.0f}/s (ConfigureImpl {:.1f} ms/s) | same as previous draw {:.0f}/s "
             "({:.1f} ms/s) | input seen before {:.0f}/s ({:.1f} ms/s), of which same resolution "
             "{:.0f}/s ({:.1f} ms/s) | new {:.0f}/s ({:.1f} ms/s) | keys {}",
             draws, ms, prev, prev_ms, seen, seen_ms, same_out, same_out_ms, fresh, fresh_ms,
             s.last_output.size());
}

} // namespace Vulkan::Nx2DrawRepeatProbe

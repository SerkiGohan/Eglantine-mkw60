// SPDX-License-Identifier: GPL-3.0-or-later
// M2 mutation probe (measurement only, EDEN_NX2_MUTATION_PROBE=1): which draws change shared
// buffer/texture cache state while being prepared? A draw that only looks things up could be
// prepared on another thread; one that uploads or creates something needs the cache to itself.
// Counters are per thread (the split's back prepares draws); GraphicsPipeline::ConfigureImpl
// compares them before and after each draw and reports every 10 s ("Mutation probe").
#pragma once

#include <chrono>
#include <cstdlib>
#include "common/common_types.h"
#include "common/nx2_probes.h"
#include "common/logging.h"
#include <string>
#include <fmt/format.h>

namespace VideoCommon::Nx2MutationProbe {

inline bool Enabled() {
    if constexpr (!Common::kNx2Probes) {
        return false; // probes not compiled in (common/nx2_probes.h)
    }
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MUTATION_PROBE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct Counters {
    u64 buffer_uploads{};      ///< SynchronizeBuffer with something to copy
    u64 buffer_upload_bytes{};
    u64 buffer_creates{};      ///< CreateBuffer
    u64 uniform_streams{};     ///< uniform data copied into the stream buffer (fast path)
    u64 texture_uploads{};     ///< RefreshContents of a CPU-modified image
    u64 texture_creates{};     ///< new image, image view or sampler
};

inline thread_local Counters t_counters{};

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Per-draw groups (a draw counts in the first that applies).
enum Group : u32 { Pure, UniformOnly, BufferUpload, BufferCreate, TextureUpload, TextureCreate, NumGroups };
inline constexpr const char* group_names[NumGroups] = {
    "lookup only", "uniform copy only", "buffer upload", "new buffer", "texture upload", "new texture/view/sampler"};

struct DrawStats {
    u64 draws[NumGroups]{};
    u64 ns[NumGroups]{};
    u64 upload_bytes{};
    u64 uniform_streams{};
    s64 last_report{};
};
inline thread_local DrawStats t_draws{};

/// After a draw was prepared: `before` is t_counters from its start.
inline void NoteDraw(const Counters& before, s64 start_ns) {
    const Counters& now_c = t_counters;
    const s64 now = NowNs();
    Group group = Pure;
    if (now_c.texture_creates != before.texture_creates) {
        group = TextureCreate;
    } else if (now_c.texture_uploads != before.texture_uploads) {
        group = TextureUpload;
    } else if (now_c.buffer_creates != before.buffer_creates) {
        group = BufferCreate;
    } else if (now_c.buffer_uploads != before.buffer_uploads) {
        group = BufferUpload;
    } else if (now_c.uniform_streams != before.uniform_streams) {
        group = UniformOnly;
    }
    auto& d = t_draws;
    ++d.draws[group];
    d.ns[group] += static_cast<u64>(now - start_ns);
    d.upload_bytes += now_c.buffer_upload_bytes - before.buffer_upload_bytes;
    d.uniform_streams += now_c.uniform_streams - before.uniform_streams;
    if (d.last_report == 0) {
        d.last_report = now;
        return;
    }
    if (now - d.last_report < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(now - d.last_report) / 1e9;
    d.last_report = now;
    u64 total = 0;
    for (u32 g = 0; g < NumGroups; ++g) {
        total += d.draws[g];
    }
    std::string text;
    for (u32 g = 0; g < NumGroups; ++g) {
        text += fmt::format(" | {} {:.0f}/s ({:.1f} %, {:.1f} ms/s)", group_names[g],
                            static_cast<double>(d.draws[g]) / seconds,
                            total ? 100.0 * static_cast<double>(d.draws[g]) / static_cast<double>(total) : 0.0,
                            static_cast<double>(d.ns[g]) / 1e6 / seconds);
        d.draws[g] = 0;
        d.ns[g] = 0;
    }
    LOG_INFO(HW_GPU, "Mutation probe: Draws {:.0f}/s{} || buffer uploads {:.1f} MiB/s, uniform copies {:.0f}/s",
             static_cast<double>(total) / seconds, text,
             static_cast<double>(d.upload_bytes) / seconds / 1048576.0,
             static_cast<double>(d.uniform_streams) / seconds);
    d.upload_bytes = 0;
    d.uniform_streams = 0;
}

} // namespace VideoCommon::Nx2MutationProbe

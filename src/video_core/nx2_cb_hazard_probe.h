// SPDX-License-Identifier: GPL-3.0-or-later
// B1 step 1 (measurement only, EDEN_NX2_CB_HAZARD_PROBE=1): would a draw prepared later (on another
// thread) read guest memory that the game rewrote in between? Between draws the game updates constant
// buffers in guest memory (Maxwell3D::ProcessCBMultiData). For every such write this counts whether it
// lands on memory one of the recent draws read while being prepared, how many draws back that draw was,
// what it read there (uniform range, texture handle, storage buffer pointer) and whether the write
// changed the value. Back thread only. One "CB hazard probe" line every 10 s.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2CbHazardProbe {

enum Kind : u32 { Uniform = 0, Handle = 1, StoragePointer = 2, NumKinds = 3 };
inline constexpr const char* kind_names[NumKinds] = {"Uniform", "Handle", "SSBO pointer"};
/// Distance buckets in draws: 1, 2-8, 9-32, 33-128.
inline constexpr u32 NumBuckets = 4;
inline constexpr const char* bucket_names[NumBuckets] = {"1", "2-8", "9-32", "33-128"};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_CB_HAZARD_PROBE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct State {
    static constexpr u64 Granule = 64;       ///< bytes
    static constexpr u64 MaxGranules = 256;   ///< per read range (16 KiB); longer ranges are cut
    std::unordered_map<u64, u64> last_read;  ///< granule -> (draw << 2) | kind
    u64 draw{};                              ///< draws begun so far
    u64 writes{}, write_bytes{}, changed_writes{};
    u64 hits[NumKinds][NumBuckets]{};
    u64 changed_hits[NumKinds][NumBuckets]{};
    u64 misses{}, truncated{};
    s64 last_report{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void BeginDraw() {
    ++Get().draw;
}

/// The draw being prepared read [address, address + size).
inline void NoteRead(u64 address, u64 size, Kind kind) {
    auto& s = Get();
    if (size == 0) {
        return;
    }
    u64 first = address / State::Granule;
    u64 last = (address + size - 1) / State::Granule;
    if (last - first >= State::MaxGranules) {
        last = first + State::MaxGranules - 1;
        ++s.truncated;
    }
    if (s.last_read.size() > (1U << 22)) {
        s.last_read.clear();
    }
    const u64 tag = (s.draw << 2) | kind;
    for (u64 g = first; g <= last; ++g) {
        s.last_read[g] = tag;
    }
}

inline void MaybeReport(State& s) {
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (s.last_report == 0) {
        s.last_report = now;
        return;
    }
    if (now - s.last_report < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(now - s.last_report) / 1e9;
    s.last_report = now;
    std::string text;
    for (u32 k = 0; k < NumKinds; ++k) {
        text += fmt::format(" | {}:", kind_names[k]);
        for (u32 b = 0; b < NumBuckets; ++b) {
            text += fmt::format(" {} draws back {:.0f}/s (changed {:.0f}/s)", bucket_names[b],
                                static_cast<double>(s.hits[k][b]) / seconds,
                                static_cast<double>(s.changed_hits[k][b]) / seconds);
            s.hits[k][b] = 0;
            s.changed_hits[k][b] = 0;
        }
    }
    LOG_INFO(HW_GPU,
             "CB hazard probe: Draws {:.0f}/s | CB writes {:.0f}/s ({:.1f} KiB/s, value changed "
             "{:.0f}/s) | no hit (or > 128 draws) {:.0f}/s{} | clipped read ranges {}",
             static_cast<double>(s.draw) / seconds, static_cast<double>(s.writes) / seconds,
             static_cast<double>(s.write_bytes) / 1024.0 / seconds,
             static_cast<double>(s.changed_writes) / seconds, static_cast<double>(s.misses) / seconds,
             text, s.truncated);
    s.draw = 0;
    s.writes = s.write_bytes = s.changed_writes = s.misses = s.truncated = 0;
    s.last_read.clear();
}

/// A constant buffer write of [address, address + size) between draws; `changed`: the bytes differ
/// from what guest memory held.
inline void NoteWrite(u64 address, u64 size, bool changed) {
    auto& s = Get();
    ++s.writes;
    s.write_bytes += size;
    s.changed_writes += changed ? 1 : 0;
    u64 best_draw = 0;
    u32 best_kind = 0;
    bool hit = false;
    const u64 first = address / State::Granule;
    const u64 last = (address + std::max<u64>(size, 1) - 1) / State::Granule;
    for (u64 g = first; g <= last; ++g) {
        const auto it = s.last_read.find(g);
        if (it == s.last_read.end()) {
            continue;
        }
        const u64 draw = it->second >> 2;
        if (!hit || draw > best_draw) {
            best_draw = draw;
            best_kind = static_cast<u32>(it->second & 3);
            hit = true;
        }
    }
    const u64 distance = hit ? s.draw - best_draw + 1 : ~u64{0};
    if (!hit || distance > 128) {
        ++s.misses;
    } else {
        const u32 bucket = distance <= 1 ? 0 : distance <= 8 ? 1 : distance <= 32 ? 2 : 3;
        ++s.hits[best_kind][bucket];
        s.changed_hits[best_kind][bucket] += changed ? 1 : 0;
    }
    MaybeReport(s);
}

} // namespace VideoCommon::Nx2CbHazardProbe

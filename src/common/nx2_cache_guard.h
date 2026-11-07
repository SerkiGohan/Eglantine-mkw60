// SPDX-License-Identifier: GPL-3.0-or-later
// B2 step 2 (measurement only, EDEN_NX2_CACHE_GUARD=1, fork mkw60): which shared state the draw
// preparation writes, and in which part of it (docs/mkw60-b2-plan.md, "Places that assume a
// single thread"). The parts are the draw zones of the GPU profile, tracked by
// VideoCommon::Nx2PrepCost (video_core/nx2_prep_cost_probe.h), which sets t_zone on the thread
// that prepares a draw (~0 outside draws). The goal for B3: no write in a part that is to run
// in parallel, except state that becomes per preparer. Logged with the "Prep cost probe" line.
#pragma once

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include "common/common_types.h"

namespace Common::Nx2CacheGuard {

enum Site : u32 {
    LruTouch,        ///< LeastRecentlyUsedCache::Touch that moved an item (first use this frame)
    LruInsertFree,   ///< LeastRecentlyUsedCache::Insert / Free
    SlotInsert,      ///< SlotVector::insert (may move all objects when it grows)
    SlotErase,       ///< SlotVector::erase
    DescriptorQueue, ///< GuestDescriptorQueue Acquire / Add* (shared payload ring)
    SchedulerRecord, ///< Scheduler::Record (one command stream)
    StagingRequest,  ///< StagingBufferPool::Request (shared staging ring)
    DescTableMemo,   ///< DescriptorTable::Read stored a new TIC/TSC entry (per channel memo)
    BindingState,    ///< BufferCache channel_state bindings updated (index/vertex/uniform/storage/texel/xfb)
    ImageViewInsert, ///< TextureCache: new image view for a TIC entry (channel image_views map)
    SamplerCreate,   ///< TextureCache: new sampler for a TSC entry
    ImageModified,   ///< TextureCache::MarkModification (image flags)
    PipelineState,   ///< PipelineCache: current graphics pipeline changed
    DirtyCleared,    ///< Maxwell3D dirty flags cleared (count of flags)
    DirtySet,        ///< Maxwell3D dirty flags set (count of flags)
    NumSites,
};

inline constexpr std::array<const char*, NumSites> site_names{
    "LRU touch",       "LRU insert/erase", "slot insert",  "slot erase",
    "descriptor queue", "scheduler record", "staging request", "TIC/TSC cache",
    "buffer binding state", "new image view", "new sampler", "image changed",
    "pipeline state", "dirty cleared", "dirty set",
};

inline constexpr u32 NoZone = ~0U;
inline constexpr u32 MaxZones = 64;

inline const bool g_enabled = [] {
    const char* value = std::getenv("EDEN_NX2_CACHE_GUARD");
    return value != nullptr && value[0] == '1';
}();

struct Data {
    std::array<std::array<u64, MaxZones>, NumSites> counts{};
    const void* dirty_flags{}; ///< Maxwell3D dirty.flags of the draw being prepared (32 bytes)
    std::array<u64, 4> dirty_snapshot{};
};

inline thread_local u32 t_zone = NoZone;

/// P2 (fork mkw60): set on the shadow preparer thread. Shared caches must not change there:
/// uploads, new buffers, written-range marks, image modification marks and host commands are
/// skipped; whatever would have been needed sets t_shadow_needs_serial.
inline thread_local bool t_shadow_prep = false;
inline thread_local bool t_shadow_needs_serial = false;
inline thread_local Data t_data;

/// Counts a write at `site` in the current draw zone (nothing outside draws).
inline void Note(Site site, u64 amount = 1) {
    if (g_enabled && t_zone != NoZone) [[unlikely]] {
        t_data.counts[site][t_zone] += amount;
    }
}

/// The draw being prepared uses these dirty flags (std::bitset<256>); snapshot them.
inline void SetDirtyFlags(const void* flags) {
    if (!g_enabled) {
        return;
    }
    t_data.dirty_flags = flags;
    std::memcpy(t_data.dirty_snapshot.data(), flags, sizeof(t_data.dirty_snapshot));
}

/// Called on every zone change: flags changed since the last change belong to `zone`.
inline void Transition(u32 zone) {
    auto& d = t_data;
    if (d.dirty_flags == nullptr || zone == NoZone) {
        return;
    }
    std::array<u64, 4> now;
    std::memcpy(now.data(), d.dirty_flags, sizeof(now));
    for (size_t i = 0; i < now.size(); ++i) {
        const u64 changed = now[i] ^ d.dirty_snapshot[i];
        if (changed != 0) [[unlikely]] {
            d.counts[DirtyCleared][zone] +=
                static_cast<u64>(std::popcount(changed & d.dirty_snapshot[i]));
            d.counts[DirtySet][zone] += static_cast<u64>(std::popcount(changed & now[i]));
        }
    }
    d.dirty_snapshot = now;
}

} // namespace Common::Nx2CacheGuard

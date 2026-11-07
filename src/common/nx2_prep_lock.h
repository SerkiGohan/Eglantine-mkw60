// SPDX-License-Identifier: GPL-3.0-or-later
// P2b (fork mkw60, docs/mkw60-b3-snapshot-design.md): preparers on other threads read the buffer and
// texture caches without their locks. Structural changes must not happen meanwhile: a SlotVector
// growing (moves every object), an object being erased or moved out of its slot (buffer, image,
// image view, framebuffer deletion). Those take this lock exclusively; preparers hold it shared
// while they work. Off (no cost but a flag test) unless a lock-free shadow preparer runs.
// g_epoch counts erasures: a preparer whose snapshot ids (image views, samplers) may have been
// erased since the snapshot was taken skips its job.
#pragma once

#include <atomic>
#include <shared_mutex>
#include "common/common_types.h"

namespace Common::Nx2PrepLock {

inline std::shared_mutex g_mutex;
inline std::atomic<bool> g_enabled{false};
inline std::atomic<u64> g_epoch{0};
inline thread_local bool t_reader = false;
inline thread_local int t_write_depth = 0; ///< nested structural changes lock once

/// Exclusive while a structural change runs (nothing when off, or on a preparer thread).
class WriteScope {
public:
    WriteScope()
        : locked{g_enabled.load(std::memory_order_relaxed) && !t_reader && t_write_depth == 0} {
        if (locked) [[unlikely]] {
            g_mutex.lock();
        }
        ++t_write_depth;
    }
    ~WriteScope() {
        --t_write_depth;
        if (locked) [[unlikely]] {
            g_mutex.unlock();
        }
    }
    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;

private:
    bool locked;
};

inline void NoteErase() {
    if (g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
        g_epoch.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Common::Nx2PrepLock

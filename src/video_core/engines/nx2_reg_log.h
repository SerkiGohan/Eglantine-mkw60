// SPDX-License-Identifier: GPL-3.0-or-later
// P3 step S1 (fork mkw60, docs/mkw60-p3-plan.md): register change log for draw preparers.
//
// The thread that owns the 3D registers (back thread) keeps the draw replica (B1) up to date before
// every draw (Maxwell3D::Nx2SyncReplica). With EDEN_NX2_PREP_REGLOG=1 it also appends every
// register it copies into the replica to an ordered log, (index, value). A preparer on another
// thread keeps its own replica: it starts from a full copy (baseline) taken by the owner at a known
// log position and then replays the log up to the position of the draw it prepares. That gives it
// the draw's registers without copying 14 KiB per draw (B1 measured ~19 changed registers per draw).
// EDEN_NX2_PREP_REGLOG=2 additionally keeps the old full copy in the shadow job and compares.
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "common/common_types.h"

namespace Tegra::Engines::Nx2RegLog {

/// 0 = off, 1 = preparers replay the log, 2 = 1 and compare with a full register copy (test).
inline u32 Mode() {
    static const u32 mode = [] {
        const char* value = std::getenv("EDEN_NX2_PREP_REGLOG");
        return value != nullptr && (value[0] == '1' || value[0] == '2') ? static_cast<u32>(value[0] - '0')
                                                                       : 0U;
    }();
    return mode;
}

struct Entry {
    u32 index;
    u32 value;
};

/// Ring of 2^20 entries (8 MiB): ~55,000 draws at ~19 registers each before an entry is reused.
inline constexpr u64 Capacity = u64{1} << 20;

template <typename Regs>
struct Baseline {
    Regs regs;    ///< the owner's replica after the draw at `position`
    u64 position; ///< log entries up to here are contained in `regs`
};

/// One per Maxwell3D. Append/Publish/TakeBaseline run on the owner thread only.
template <typename Regs>
class Log {
public:
    void Append(u32 index, u32 value) {
        if (ring.empty()) [[unlikely]] {
            ring.resize(Capacity);
        }
        ring[local_head & (Capacity - 1)] = {index, value};
        ++local_head;
    }

    /// End of a draw's changes: makes them visible, and takes a baseline if a preparer asked for one.
    void Publish(const Regs& replica) {
        head.store(local_head, std::memory_order_release);
        if (want_baseline.exchange(false, std::memory_order_acq_rel)) [[unlikely]] {
            auto baseline = std::make_shared<Baseline<Regs>>();
            std::memcpy(&baseline->regs, &replica, sizeof(Regs));
            baseline->position = local_head;
            std::scoped_lock lock{baseline_mutex};
            latest_baseline = std::move(baseline);
        }
    }

    /// Owner thread: log position after the current draw's changes.
    [[nodiscard]] u64 Position() const {
        return local_head;
    }

    // Preparer side
    void RequestBaseline() {
        want_baseline.store(true, std::memory_order_release);
    }
    [[nodiscard]] std::shared_ptr<const Baseline<Regs>> LatestBaseline() {
        std::scoped_lock lock{baseline_mutex};
        return latest_baseline;
    }
    [[nodiscard]] u64 PublishedHead() const {
        return head.load(std::memory_order_acquire);
    }
    [[nodiscard]] const Entry& At(u64 position) const {
        return ring[position & (Capacity - 1)];
    }

private:
    std::vector<Entry> ring;
    u64 local_head{};
    std::atomic<u64> head{};
    std::atomic<bool> want_baseline{true};
    std::mutex baseline_mutex;
    std::shared_ptr<const Baseline<Regs>> latest_baseline;
};

/// A preparer's own register set, replayed from the log.
template <typename Regs>
class Replica {
public:
    enum class Result { Ready, NoBaseline, Overrun };

    /// Brings the replica to `target` (the log position of the draw to prepare). Ready: `Regs()`
    /// holds that draw's registers. Otherwise the job must be skipped; a new baseline is requested.
    Result CatchUp(Log<Regs>& log, u64 target) {
        if (!regs || cursor > target) {
            // No replica yet, or it is ahead of this draw (jobs out of order): start from a baseline.
            if (!AdoptBaseline(log, target)) {
                return Result::NoBaseline;
            }
        }
        const u64 start = cursor;
        for (u64 position = cursor; position < target; ++position) {
            const Entry& entry = log.At(position);
            regs->reg_array[entry.index] = entry.value;
        }
        // The owner may have reused entries while they were read: then this replica is unusable.
        if (log.PublishedHead() - start > Capacity) [[unlikely]] {
            regs.reset();
            log.RequestBaseline();
            return Result::Overrun;
        }
        cursor = target;
        replayed += target - start;
        return Result::Ready;
    }

    [[nodiscard]] const Regs& Get() const {
        return *regs;
    }
    u64 replayed{}; ///< entries applied (statistics)
    u64 baselines{};

private:
    bool AdoptBaseline(Log<Regs>& log, u64 target) {
        auto baseline = log.LatestBaseline();
        if (!baseline || baseline->position > target) {
            log.RequestBaseline();
            return false;
        }
        if (!regs) {
            regs = std::make_unique<Regs>();
        }
        std::memcpy(regs.get(), &baseline->regs, sizeof(Regs));
        cursor = baseline->position;
        ++baselines;
        return true;
    }

    std::unique_ptr<Regs> regs;
    u64 cursor{};
};

} // namespace Tegra::Engines::Nx2RegLog

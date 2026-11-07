// SPDX-License-Identifier: GPL-3.0-or-later
// Wait statistics for the Switch 2 analysis (EDEN_NX2_WAIT_STATS=1).
// Measures how long threads wait at certain places and writes a summary to the log every
// 10 s ("Wait stats: ..."). Without the environment variable it costs only one check of a
// static bool per call.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace Common::Nx2WaitStats {

// ---------------------------------------------------------------------------
// 1) Measuring sites
// ---------------------------------------------------------------------------
enum class Site : u32 {
    Idle0, Idle1, Idle2, Idle3, Idle4, Idle5, Idle6, Idle7, // core had nothing to do
    SpinLockSleep,   // kernel spinlock (e.g. the scheduler lock) had to sleep
    SyncptGuardWait, // waiting for the global lock of the SyncpointManager
    SyncptActions,   // time Increment() spends running callbacks under the lock
    SyncptWait,      // someone waits until the GPU reaches a syncpoint
    SlotLatency,     // game asks "tell me when the GPU is at X" -> until the notification
    SlotImmediate,   // ...the same request, but the GPU was already there (done at once)
    BackDelay,       // syncpoint increment: queued by the front GPU thread -> executed at the back
    ListsWriters,    // GPU-written command lists: the front waits until the back ran the writer
    ListsGpuCopy,    // ...: the front waits until the real GPU finished copying the list
    ListsFlush,      // ...: fallback without a copy: drain everything and write back
    ListsSubmitDelay, // part of "gpu copy": the work was not even submitted to the GPU yet
    ListsGpuExec,     // part of "gpu copy": from submission until done (GPU work + wake-up)
    WorkerBacklog,    // every submission: flush on the back thread -> VulkanWorker recorded everything
    VkSubmit,         // every submission: duration of the vkQueueSubmit call itself (driver + kernel)
    SubmitFence,      // submissions triggered by a fence (FenceManager)
    SubmitDraws,      // ... by the submission interval (every N draws, FlushWork)
    SubmitLists,      // ... by copies back of GPU-written command lists
    SubmitCommands,   // ... by FlushCommands (among others fences with cache write-back)
    SubmitDeferred,   // ... deferred submissions collected (upstream batching, FlushDeferred)
    SubmitOther,      // ... everything else (Present, Finish, ...)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Site::Count)> Names{
    "idle0", "idle1", "idle2", "idle3", "idle4", "idle5", "idle6", "idle7",
    "spinlock sleep", "syncpt guard wait", "syncpt actions", "syncpt wait",
    "slot latency", "slot immediate", "back delay",
    "lists wait writers", "lists wait gpu copy", "lists flush fallback",
    "  ..of which submit delay", "  ..of which gpu exec",
    "worker backlog", "vkQueueSubmit",
    "submit by fence", "submit by draws", "submit by lists", "submit by flushcmds", "submit deferred", "submit other",
};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_WAIT_STATS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// 2) Collection point: total time and count per measuring site, plus the largest callback list
// ---------------------------------------------------------------------------
struct State {
    std::array<std::atomic<u64>, static_cast<size_t>(Site::Count)> ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(Site::Count)> calls{};
    std::atomic<u64> max_actions{0};
    std::atomic<s64> last_report{0};
};

inline State& Get() {
    static State state;
    return state;
}

// ---------------------------------------------------------------------------
// 3) Every 10 s exactly ONE thread writes the summary (compare_exchange)
// ---------------------------------------------------------------------------
inline void MaybeReport(s64 now) {
    auto& state = Get();
    s64 last = state.last_report.load(std::memory_order_relaxed);
    if (last == 0) {
        state.last_report.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 10'000'000'000LL ||
        !state.last_report.compare_exchange_strong(last, now)) {
        return;
    }
    const double seconds = static_cast<double>(now - last) / 1e9;
    std::string line;
    for (size_t i = 0; i < Names.size(); ++i) {
        const u64 ns = state.ns[i].exchange(0, std::memory_order_relaxed);
        const u64 calls = state.calls[i].exchange(0, std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        line += fmt::format(" | {} {:.0f} ms/s ({:.0f}/s, avg {:.0f} us)", Names[i],
                            static_cast<double>(ns) / 1e6 / seconds,
                            static_cast<double>(calls) / seconds,
                            static_cast<double>(ns) / 1e3 / static_cast<double>(calls));
    }
    line += fmt::format(" | max syncpt actions {}",
                        state.max_actions.exchange(0, std::memory_order_relaxed));
    LOG_INFO(Kernel, "Wait stats:{}", line);
}

inline void Add(Site site, s64 start_ns) {
    const s64 now = NowNs();
    auto& state = Get();
    const auto index = static_cast<size_t>(site);
    state.ns[index].fetch_add(static_cast<u64>(now - start_ns), std::memory_order_relaxed);
    state.calls[index].fetch_add(1, std::memory_order_relaxed);
    MaybeReport(now);
}

// Record a duration directly (for measurements that a Scope cannot cover)
inline void AddDuration(Site site, s64 ns) {
    auto& state = Get();
    const auto index = static_cast<size_t>(site);
    state.ns[index].fetch_add(static_cast<u64>(ns > 0 ? ns : 0), std::memory_order_relaxed);
    state.calls[index].fetch_add(1, std::memory_order_relaxed);
}

// When was GPU tick X submitted to the graphics card? (ring buffer, 4096 ticks)
inline std::array<std::atomic<s64>, 4096>& SubmitTimes() {
    static std::array<std::atomic<s64>, 4096> times{};
    return times;
}
inline void NoteSubmit(u64 tick) {
    if (Enabled()) {
        SubmitTimes()[tick & 4095].store(NowNs(), std::memory_order_release);
    }
}
inline s64 SubmitTime(u64 tick) {
    return SubmitTimes()[tick & 4095].load(std::memory_order_acquire);
}

// When did the back thread finish tick X (Scheduler::Flush)? Same ring buffer layout.
inline std::array<std::atomic<s64>, 4096>& FlushTimes() {
    static std::array<std::atomic<s64>, 4096> times{};
    return times;
}
inline void NoteFlush(u64 tick) {
    if (Enabled()) {
        FlushTimes()[tick & 4095].store(NowNs(), std::memory_order_release);
    }
}
inline s64 FlushTime(u64 tick) {
    return FlushTimes()[tick & 4095].load(std::memory_order_acquire);
}

// EXPERIMENT: wait for the copy of a GPU-written command list actively (polling) first,
// before letting the operating system put the thread to sleep. Saves the wake-up time.
// Toggle in game:  touch /tmp/eden_list_spin  (on)  /  rm /tmp/eden_list_spin  (off)
inline bool ListSpinEnabled() {
    static std::atomic<bool> active{false};
    static std::atomic<u32> calls{0};
    if ((calls.fetch_add(1, std::memory_order_relaxed) & 63) == 0) {
        std::error_code ec;
        const bool want = std::filesystem::exists("/tmp/eden_list_spin", ec);
        if (active.exchange(want, std::memory_order_relaxed) != want) {
            LOG_INFO(Kernel, "List wait spin: {}", want ? "ON" : "off");
        }
    }
    return active.load(std::memory_order_relaxed);
}

// Who triggered the next submission? Set right before scheduler.Flush() on the same thread
// and evaluated in Scheduler::SubmitExecution (both on the back GPU thread).
inline thread_local Site t_submit_reason = Site::SubmitOther;
inline void TagNextSubmit(Site reason) {
    t_submit_reason = reason;
}
inline void CountSubmit() {
    if (Enabled()) {
        AddDuration(t_submit_reason, 0);
    }
    t_submit_reason = Site::SubmitOther;
}

inline void NoteActions(u64 count) {
    auto& max = Get().max_actions;
    u64 current = max.load(std::memory_order_relaxed);
    while (count > current && !max.compare_exchange_weak(current, count)) {
    }
}

// Measures the lifetime of the object: { Scope s{Site::X}; ...measured code... }
class Scope {
public:
    explicit Scope(Site site_) : site{site_}, active{Enabled()} {
        if (active) {
            start = NowNs();
        }
    }
    ~Scope() {
        if (active) {
            Add(site, start);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Site site;
    bool active;
    s64 start{};
};

} // namespace Common::Nx2WaitStats

// SPDX-License-Identifier: GPL-3.0-or-later
// Wait statistics for the Switch 2 analysis (EDEN_NX2_WAIT_STATS=1).
// Measures how long threads wait at certain places and writes a summary to the log every
// 10 s ("Wait stats: ..."). Without the environment variable it costs only one check of a
// static bool per call.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

// ---------------------------------------------------------------------------
// 2b) Timeline (nx2-waittrace): touch /tmp/eden_trace -> records every event with a timestamp
//     for EDEN_NX2_TRACE_SECONDS (default 5) seconds and then writes
//     ~/eden-trace-<date>-<time>.json (Chrome trace format, open in ui.perfetto.dev).
//     Needs EDEN_NX2_WAIT_STATS=1. The file /tmp/eden_trace is deleted at start, so every
//     touch records exactly one timeline.
// ---------------------------------------------------------------------------
namespace Trace {

// Names: 0..Site::Count-1 are the measuring sites above, then events of their own
enum class Extra : u16 {
    PresentFence = static_cast<u16>(Site::Count), // present: the game blocks until the frame is done
    PresentComposite,                             // present: compose the image
    PresentPace,                                  // present: wait for the frame clock (vblank)
    GpuTick,                                      // graphics card works on submission (tick) N
    // nx2-waitzones: waits of the front (GPU) and back (GPUBack) GPU threads
    FrontIdle,     // front: command queue empty, waits for the game
    FrontPending,  // front: channel waits for a CPU semaphore, yield/1 ms sleep
    FrontDrain,    // front: waits until the back has processed everything (GpuSplit::Drain)
    FrontRingFull, // front: ring full, waits for space (GpuSplit::WaitForSpace)
    BackIdle,      // back: ring empty, waits for the front thread
    Count,
};

inline const char* NameOf(u16 id) {
    static constexpr std::array<const char*, 9> extra{
        "present: wait for frame", "present: composite", "present: vblank pacing", "GPU executes",
        "front: wait for game",    "front: wait for semaphore (sleep)",
        "front: wait for back (drain)", "front: wait for ring space", "back: wait for front"};
    if (id < Names.size()) {
        return Names[id];
    }
    const u16 index = static_cast<u16>(id - static_cast<u16>(Site::Count));
    return index < extra.size() ? extra[index] : "?";
}

// Tracks of their own for waits that span threads
enum class Track : u8 { Self = 0, GpuHardware, SlotWait, BackDelay, WorkerBacklog, Count };

inline const char* TrackName(Track track) {
    switch (track) {
    case Track::GpuHardware:
        return "Graphics card (submissions in flight)";
    case Track::SlotWait:
        return "Game waits for GPU notification (slot)";
    case Track::BackDelay:
        return "Latency: queued at the front -> executed at the back";
    case Track::WorkerBacklog:
        return "Latency: flush -> recorded by the VulkanWorker";
    default:
        return "?";
    }
}

inline Track TrackFor(u16 id) {
    if (id == static_cast<u16>(Extra::GpuTick)) {
        return Track::GpuHardware;
    }
    switch (static_cast<Site>(id)) {
    case Site::SlotLatency:
        return Track::SlotWait;
    case Site::BackDelay:
        return Track::BackDelay;
    case Site::WorkerBacklog:
        return Track::WorkerBacklog;
    default:
        return Track::Self;
    }
}

struct Event {
    s64 start;
    s64 end;
    u64 arg;
    u16 name;
    u8 track;
    u8 instant;
};

struct ThreadBuffer {
    std::mutex mutex;
    std::vector<Event> events;
    s64 tid{};
    std::string name;
};

struct Recorder {
    std::mutex registry_mutex;
    std::vector<std::shared_ptr<ThreadBuffer>> buffers;
    std::atomic<bool> active{false};
    std::atomic<s64> start_ns{0};
    std::atomic<s64> stop_ns{0};
    std::atomic<s64> next_poll_ns{0};
};

inline Recorder& GetRecorder() {
    static Recorder recorder;
    return recorder;
}

inline s64 Seconds() {
    static const s64 seconds = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_SECONDS");
        const long parsed = value != nullptr ? std::strtol(value, nullptr, 10) : 5;
        return static_cast<s64>(std::clamp<long>(parsed, 1, 60));
    }();
    return seconds;
}

inline ThreadBuffer& LocalBuffer() {
    thread_local std::shared_ptr<ThreadBuffer> buffer = [] {
        auto created = std::make_shared<ThreadBuffer>();
#ifdef __linux__
        created->tid = static_cast<s64>(::syscall(SYS_gettid));
        char name[32]{};
        if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) {
            created->name = name;
        }
#else
        created->tid = static_cast<s64>(std::hash<std::thread::id>{}(std::this_thread::get_id()) &
                                        0x7fffffff);
#endif
        if (created->name.empty()) {
            created->name = fmt::format("thread {}", created->tid);
        }
        auto& recorder = GetRecorder();
        std::scoped_lock lk{recorder.registry_mutex};
        recorder.buffers.push_back(created);
        return created;
    }();
    return *buffer;
}

inline std::string EscapeJson(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

inline void WriteFile(s64 t0) {
    auto& recorder = GetRecorder();
    std::vector<std::shared_ptr<ThreadBuffer>> buffers;
    {
        std::scoped_lock lk{recorder.registry_mutex};
        buffers = recorder.buffers;
    }
    const char* home = std::getenv("HOME");
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::string path =
        fmt::format("{}/eden-trace-{}.json", home != nullptr ? home : "/tmp", stamp);
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        LOG_ERROR(Kernel, "Trace: cannot write {}", path);
        return;
    }
    constexpr s64 pseudo_tid_base = 900000;
    size_t count = 0;
    std::fputs("{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n", file);
    std::fputs("{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,"
               "\"args\":{\"name\":\"Eglantine\"}}",
               file);
    for (u8 track = 1; track < static_cast<u8>(Track::Count); ++track) {
        const s64 tid = pseudo_tid_base + track;
        std::fprintf(file,
                     ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%lld,"
                     "\"args\":{\"name\":\"%s\"}}"
                     ",\n{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":%lld,"
                     "\"args\":{\"sort_index\":%d}}",
                     static_cast<long long>(tid), TrackName(static_cast<Track>(track)),
                     static_cast<long long>(tid), -10 + track);
    }
    for (const auto& buffer : buffers) {
        std::vector<Event> events;
        {
            std::scoped_lock lk{buffer->mutex};
            events.swap(buffer->events);
        }
        if (events.empty()) {
            continue;
        }
        std::fprintf(file,
                     ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%lld,"
                     "\"args\":{\"name\":\"%s\"}}",
                     static_cast<long long>(buffer->tid), EscapeJson(buffer->name).c_str());
        for (const Event& event : events) {
            const s64 tid = event.track == 0 ? buffer->tid : pseudo_tid_base + event.track;
            const double ts = static_cast<double>(event.start - t0) / 1000.0;
            if (event.instant) {
                std::fprintf(file,
                             ",\n{\"name\":\"%s\",\"ph\":\"i\",\"s\":\"t\",\"ts\":%.3f,"
                             "\"pid\":1,\"tid\":%lld}",
                             NameOf(event.name), ts, static_cast<long long>(tid));
            } else {
                const double dur = static_cast<double>(event.end - event.start) / 1000.0;
                std::fprintf(file,
                             ",\n{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,"
                             "\"pid\":1,\"tid\":%lld,\"args\":{\"n\":%llu}}",
                             NameOf(event.name), ts, dur, static_cast<long long>(tid),
                             static_cast<unsigned long long>(event.arg));
            }
            ++count;
        }
    }
    std::fputs("\n]}\n", file);
    std::fclose(file);
    LOG_INFO(Kernel, "Trace: {} events written to {}", count, path);
}

inline bool Active() {
    return GetRecorder().active.load(std::memory_order_relaxed);
}

// Check the switch file at most every 200 ms; when the time is up, stop recording and write
inline void Poll(s64 now) {
    auto& recorder = GetRecorder();
    if (recorder.active.load(std::memory_order_relaxed)) {
        s64 stop = recorder.stop_ns.load(std::memory_order_relaxed);
        if (now >= stop && recorder.stop_ns.compare_exchange_strong(stop, 0) && stop != 0) {
            recorder.active.store(false, std::memory_order_relaxed);
            const s64 t0 = recorder.start_ns.load(std::memory_order_relaxed);
            LOG_INFO(Kernel, "Trace: recording finished, writing file ...");
            // Writing takes time: own thread, wait briefly until entries in progress are done
            std::thread([t0] {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                WriteFile(t0);
            }).detach();
        }
        return;
    }
    s64 next = recorder.next_poll_ns.load(std::memory_order_relaxed);
    if (now < next || !recorder.next_poll_ns.compare_exchange_strong(next, now + 200'000'000)) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists("/tmp/eden_trace", ec)) {
        return;
    }
    std::filesystem::remove("/tmp/eden_trace", ec);
    {
        // Drop leftovers of an earlier recording
        std::scoped_lock lk{recorder.registry_mutex};
        for (const auto& buffer : recorder.buffers) {
            std::scoped_lock buffer_lk{buffer->mutex};
            buffer->events.clear();
        }
    }
    recorder.start_ns.store(now, std::memory_order_relaxed);
    recorder.stop_ns.store(now + Seconds() * 1'000'000'000LL, std::memory_order_relaxed);
    recorder.active.store(true, std::memory_order_relaxed);
    LOG_INFO(Kernel, "Trace: recording started ({} s)", Seconds());
}

inline void Push(const Event& event) {
    ThreadBuffer& buffer = LocalBuffer();
    std::scoped_lock lk{buffer.mutex};
    buffer.events.push_back(event);
}

/// Section [start, end] on the matching track (own thread or special track)
inline void Complete(u16 name, s64 start, s64 end, u64 arg = 0) {
    Poll(end);
    if (!Active()) {
        return;
    }
    const Track track = TrackFor(name);
    // Very short waits on the own thread (e.g. 30,000/s "lists wait writers" with 0 us)
    // only make the file big
    if (track == Track::Self && end - start < 2'000 &&
        name < static_cast<u16>(Site::Count)) {
        return;
    }
    Push(Event{start, end, arg, name, static_cast<u8>(track), 0});
}

inline void Complete(Site site, s64 start, s64 end) {
    Complete(static_cast<u16>(site), start, end);
}

inline void Complete(Extra extra, s64 start, s64 end, u64 arg = 0) {
    Complete(static_cast<u16>(extra), start, end, arg);
}

/// Point in time (e.g. a submission to the GPU) on the own thread
inline void Instant(Site site) {
    const s64 now = NowNs();
    Poll(now);
    if (!Active()) {
        return;
    }
    Push(Event{now, now, 0, static_cast<u16>(site), 0, 1});
}

/// nx2-waitzones: measure a wait section. Without EDEN_NX2_WAIT_STATS, Begin() costs only a
/// bool check; End() adds it to the timeline from 5 us on (shorter ones are too many, e.g. the
/// spinning of the back thread between two publications).
inline s64 BeginWait() {
    return Enabled() ? NowNs() : 0;
}

// Trigger of a drain (argument "n" of "front: wait for back (drain)"), see trace_summary.py:
// 0 other, 1 MME DMA read (Maxwell3D::FrontDmaRead), 2 semaphore acquire (Puller), 3 acquire
// resume (Puller::CanResumeAcquire), 4 MapInOrder (gpu_thread), 5 list fallback
// (FlushGpuWrittenCommands), 6 Sync Memory Operations, 7 front takeover, 8 FrontVerify, 9 split stop
inline void EndWait(Extra what, s64 start, u64 arg = 0) {
    if (start == 0) {
        return;
    }
    const s64 now = NowNs();
    if (now - start >= 5'000) {
        Complete(what, start, now, arg);
    } else {
        Poll(now);
    }
}

} // namespace Trace

inline void Add(Site site, s64 start_ns) {
    const s64 now = NowNs();
    auto& state = Get();
    const auto index = static_cast<size_t>(site);
    state.ns[index].fetch_add(static_cast<u64>(now - start_ns), std::memory_order_relaxed);
    state.calls[index].fetch_add(1, std::memory_order_relaxed);
    Trace::Complete(site, start_ns, now);
    MaybeReport(now);
}

// Record a duration directly (for measurements that a Scope cannot cover)
inline void AddDuration(Site site, s64 ns) {
    auto& state = Get();
    const auto index = static_cast<size_t>(site);
    state.ns[index].fetch_add(static_cast<u64>(ns > 0 ? ns : 0), std::memory_order_relaxed);
    state.calls[index].fetch_add(1, std::memory_order_relaxed);
    // Timeline: submission trigger as a point in time, worker backlog as a section on its own track
    if (site >= Site::SubmitFence && site <= Site::SubmitOther) {
        Trace::Instant(site);
    } else if ((site == Site::WorkerBacklog || site == Site::ListsGpuCopy) && ns > 0) {
        // nx2-waitzones: also the list wait for the GPU copy (ends at the call)
        const s64 now = NowNs();
        Trace::Complete(site, now - ns, now);
    }
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

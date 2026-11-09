// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>

#include <atomic>
#include <chrono>
#include <cstdio>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif
#include "common/common_types.h"
#include "common/nx2_probes.h"
#include "common/logging.h"

namespace VideoCommon {

/// What a buffer cache runtime offers for stream uniform reuse.
///  - StreamUniformRef: a small trivially copyable description of one stream allocation (the
///    host buffer, its offset and whatever else the runtime needs to add it to a descriptor
///    payload again, e.g. the device address of the descriptor buffer path).
///  - LastStreamUniformRef(): the allocation made by the latest BindMappedUniformBuffer call.
///  - RebindStreamUniform(ref, size): adds `ref` to the descriptor payload exactly as
///    BindMappedUniformBuffer added the allocation the first time.
template <class R>
concept StreamUniformReusable = requires(R& runtime, u32 size) {
    typename R::StreamUniformRef;
    requires std::is_trivially_copyable_v<typename R::StreamUniformRef>;
    { runtime.LastStreamUniformRef() } -> std::same_as<typename R::StreamUniformRef>;
    runtime.RebindStreamUniform(runtime.LastStreamUniformRef(), size);
    { runtime.CurrentTick() } -> std::convertible_to<u64>;
};

/// EDEN_NX2_UBOREUSE=0 turns the reuse off (every small uniform buffer is copied, as before).
[[nodiscard]] inline bool UniformStreamReuseEnabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_UBOREUSE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

template <class Ref>
class UniformStreamReuse {
public:
    static constexpr u32 MaxBytes = 4096; // BufferCache's DEFAULT_SKIP_CACHE_SIZE
    static constexpr size_t NumStages = 5;
    static constexpr size_t NumSlots = 18; // Maxwell constant buffers per stage

    struct Slot {
        Ref ref{};
        u64 tick{};
        u32 size{}; // 0: nothing streamed yet
        std::array<u8, MaxBytes> bytes{};
    };

    void Reset() noexcept {
        for (auto& stage : slots) {
            for (Slot& slot : stage) {
                slot.size = 0;
                slot.tick = 0;
            }
        }
    }

    std::array<std::array<Slot, NumSlots>, NumStages> slots{};
    std::array<u8, MaxBytes> scratch{};
};

template <class Ref>
[[nodiscard]] inline UniformStreamReuse<Ref>& UniformStreamReuseState() noexcept {
    static UniformStreamReuse<Ref> state;
    return state;
}

/// Forgets every slot. Call it when a buffer cache (and so its runtime, scheduler and stream
/// buffer) is created: ticks restart with a new scheduler and the old references are dead.
template <class Runtime>
inline void ResetUniformStreamReuse() noexcept {
    if constexpr (StreamUniformReusable<Runtime>) {
        UniformStreamReuseState<typename Runtime::StreamUniformRef>().Reset();
    }
}

/// Binds the small uniform buffer at `device_addr` for (stage, cbuf `index`) through the stream
/// buffer, reusing the slot's previous allocation when the bytes and the command buffer are the
/// same. Returns false (nothing bound) when the buffer does not qualify, so the caller binds it
/// the classic way.
/// Fork mkw60 (measurement only, EDEN_NX2_UBO_PROBE=1): how often the reuse check pays off.
/// One "UBO probe" line every ~10 s: binds, bytes, same bytes in the same command buffer
/// (rebound), same bytes but a new command buffer, different bytes; size classes.
/// Fork mkw60: compare and copy straight from guest memory instead of through a scratch copy
/// (one copy less per bind, ~14 ns less per bind on the Mario circuit, A/B 8.11.2026). Start value
/// EDEN_NX2_UBO_DIRECT (default 1, 0 = old way), at run time /tmp/eden_ubo_direct (0/1, read every
/// ~250 ms; knob_sweep.py ubo_direct).
inline std::atomic<u32> g_ubo_direct{[] {
    const char* const value = std::getenv("EDEN_NX2_UBO_DIRECT");
    return value != nullptr && value[0] == '0' ? 0U : 1U;
}()};

[[nodiscard]] inline u32 UboDirect() noexcept {
    // Per thread, not atomic: a shared atomic counter here (~520,000 binds a second) was the
    // single hottest instruction of the uniform bind (perf, 9.11.2026).
    static thread_local u32 calls = 0;
    static std::atomic<s64> last_check{0};
    if ((calls++ & 4095) == 0) [[unlikely]] {
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (now - last_check.load(std::memory_order_relaxed) >= 250) {
            last_check.store(now, std::memory_order_relaxed);
            if (std::FILE* file = std::fopen("/tmp/eden_ubo_direct", "r")) {
                int value = 0;
                if (std::fscanf(file, "%d", &value) == 1) {
                    const u32 next = value == 1 ? 1U : 0U;
                    if (g_ubo_direct.exchange(next, std::memory_order_relaxed) != next) {
                        LOG_INFO(HW_GPU, "UBO direct: {}", next);
                    }
                }
                std::fclose(file);
            }
        }
    }
    return g_ubo_direct.load(std::memory_order_relaxed);
}

[[nodiscard]] inline u64 UboTicks() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct UboProbe {
    u64 binds{}, bytes{}, rebound{}, same_other_tick{}, changed{}, first{};
    std::array<u64, 2> mode_binds{}; ///< per UboDirect()
    std::array<u64, 2> mode_ticks{};
    u64 window_ticks{};
    u64 direct_fallbacks{};           ///< not contiguous in host memory
    std::array<u64, 5> size_classes{}; ///< <=64, <=256, <=1024, <=4096, >4096 (not here)
    s64 last_report{};
};

[[nodiscard]] inline bool UboProbeEnabled() noexcept {
    if constexpr (!Common::kNx2Probes) {
        return false; // probes not compiled in (common/nx2_probes.h)
    }
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_UBO_PROBE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

inline UboProbe& UboProbeState() noexcept {
    static UboProbe probe;
    return probe;
}

inline void UboProbeNote(u32 size, int outcome) {
    UboProbe& probe = UboProbeState();
    ++probe.binds;
    probe.bytes += size;
    probe.size_classes[size <= 64 ? 0 : size <= 256 ? 1 : size <= 1024 ? 2 : 3]++;
    switch (outcome) {
    case 0: ++probe.rebound; break;
    case 1: ++probe.same_other_tick; break;
    case 2: ++probe.changed; break;
    default: ++probe.first; break;
    }
    if ((probe.binds & 16383) != 0) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (probe.last_report == 0) {
        probe = {};
        probe.last_report = now;
        probe.window_ticks = UboTicks();
        return;
    }
    if (now - probe.last_report < 10'000) {
        return;
    }
    const double seconds = static_cast<double>(now - probe.last_report) / 1000.0;
    const double ns_per_tick =
        seconds * 1e9 / static_cast<double>(UboTicks() - probe.window_ticks);
    for (u32 mode = 0; mode < 2; ++mode) {
        if (probe.mode_binds[mode] != 0) {
            LOG_INFO(HW_GPU, "UBO probe direct={}: {} binds, {:.0f} ns per bind{}", mode,
                     probe.mode_binds[mode],
                     static_cast<double>(probe.mode_ticks[mode]) * ns_per_tick /
                         static_cast<double>(probe.mode_binds[mode]),
                     mode == 1 ? fmt::format(" (not contiguous: {})", probe.direct_fallbacks)
                               : std::string{});
        }
    }
    const double binds = static_cast<double>(probe.binds);
    LOG_INFO(HW_GPU,
             "UBO probe: Binds {:.0f}/s, {:.1f} MiB/s ({:.0f} bytes per bind) | same + same "
             "command buffer (rebound) {:.1f} %, same but other command buffer {:.1f} %, "
             "changed {:.1f} %, first time {:.1f} % | sizes <=64 {:.0f} %, <=256 {:.0f} %, "
             "<=1K {:.0f} %, <=4K {:.0f} %",
             binds / seconds, static_cast<double>(probe.bytes) / 1048576.0 / seconds,
             static_cast<double>(probe.bytes) / binds, 100.0 * static_cast<double>(probe.rebound) / binds,
             100.0 * static_cast<double>(probe.same_other_tick) / binds,
             100.0 * static_cast<double>(probe.changed) / binds,
             100.0 * static_cast<double>(probe.first) / binds,
             100.0 * static_cast<double>(probe.size_classes[0]) / binds,
             100.0 * static_cast<double>(probe.size_classes[1]) / binds,
             100.0 * static_cast<double>(probe.size_classes[2]) / binds,
             100.0 * static_cast<double>(probe.size_classes[3]) / binds);
    probe = {};
    probe.last_report = now;
    probe.window_ticks = UboTicks();
}

template <class Runtime, class DeviceMemory>
[[nodiscard]] bool BindSmallUniformReusing(Runtime& runtime, DeviceMemory& device_memory,
                                           size_t stage, u32 index, u32 binding_index,
                                           u64 device_addr, u32 size) {
    using Reuse = UniformStreamReuse<typename Runtime::StreamUniformRef>;
    if (size == 0 || size > Reuse::MaxBytes || stage >= Reuse::NumStages ||
        index >= Reuse::NumSlots) {
        return false;
    }
    const bool probe = UboProbeEnabled();
    const u64 start = probe ? UboTicks() : 0;
    Reuse& reuse = UniformStreamReuseState<typename Runtime::StreamUniformRef>();
    typename Reuse::Slot& slot = reuse.slots[stage][index];
    const u32 direct = UboDirect();

    // Direct: read the guest bytes where they are, if they are contiguous in host memory.
    const u8* source = nullptr;
    if (direct != 0) {
        const u8* const first = device_memory.template GetPointer<u8>(device_addr);
        const u8* const last = device_memory.template GetPointer<u8>(device_addr + size - 1);
        if (first != nullptr && last == first + (size - 1)) {
            source = first;
        } else if (probe) [[unlikely]] {
            ++UboProbeState().direct_fallbacks;
        }
    }
    if (source == nullptr) {
        u8* const scratch = reuse.scratch.data();
        device_memory.ReadBlockUnsafe(device_addr, scratch, size);
        source = scratch;
    }
    const bool same_bytes = slot.size == size && std::memcmp(slot.bytes.data(), source, size) == 0;
    if (probe) [[unlikely]] {
        const int outcome = same_bytes ? (slot.tick == runtime.CurrentTick() ? 0 : 1)
                                       : (slot.size == 0 ? 3 : 2);
        UboProbeNote(size, outcome);
    }
    if (same_bytes && slot.tick == runtime.CurrentTick()) {
        runtime.RebindStreamUniform(slot.ref, size);
    } else {
        if (!same_bytes) {
            // The slot keeps a consistent copy (the guest may be written meanwhile); the stream
            // allocation gets exactly these bytes.
            slot.size = size;
            std::memcpy(slot.bytes.data(), source, size);
        }
        const std::span<u8> span = runtime.BindMappedUniformBuffer(stage, binding_index, size);
        std::memcpy(span.data(), slot.bytes.data(), size);
        slot.ref = runtime.LastStreamUniformRef();
        slot.tick = runtime.CurrentTick();
    }
    if (probe) [[unlikely]] {
        UboProbe& state = UboProbeState();
        ++state.mode_binds[direct];
        state.mode_ticks[direct] += UboTicks() - start;
    }
    return true;
}

} // namespace VideoCommon

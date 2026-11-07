// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <variant>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, size_t frame_payload_size_,
                                             bool supports_descriptor_buffer_)
    : device{device_}, frame_payload_size{frame_payload_size_},
      supports_descriptor_buffer{supports_descriptor_buffer_},
      payload(std::make_unique_for_overwrite<DescriptorUpdateEntry[]>(frame_payload_size_ * FRAMES_IN_FLIGHT))
{
    payload_start = payload.get();
    payload_cursor = payload.get();
}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    payload_start = payload.get() + frame_index * frame_payload_size;
    payload_cursor = payload_start;
}

void UpdateDescriptorQueue::Acquire(Scheduler& scheduler, size_t required_entries,
                                    bool use_descriptor_buffer_) {
    Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
    use_descriptor_buffer = supports_descriptor_buffer && use_descriptor_buffer_;
    static constexpr size_t DEFAULT_REQUIRED_ENTRIES = 0x400;
    const size_t reserve = required_entries > 0 ? required_entries : DEFAULT_REQUIRED_ENTRIES;
    ASSERT_MSG(reserve < frame_payload_size, "Descriptor reservation {} >= frame capacity {}",
               reserve, frame_payload_size);
    const size_t used = static_cast<size_t>(std::distance(payload_start, payload_cursor));
    if (used + reserve >= frame_payload_size) {
        LOG_WARNING(Render_Vulkan, "Payload overflow (used={}, reserve={}, capacity={})",
                    used, reserve, frame_payload_size);
        scheduler.WaitWorker();
        payload_cursor = payload_start;
    }
    upload_start = payload_cursor;
    if (Nx2WriterEnabled() && t_reservation == nullptr) [[unlikely]] {
        if (required_entries == 0) {
            ++writer_stats.fallbacks;
            return;
        }
        reservation.begin = payload_cursor;
        reservation.cursor = payload_cursor;
        reservation.end = payload_cursor + required_entries;
        payload_cursor = reservation.end;
        t_reservation = &reservation;
    }
}

bool UpdateDescriptorQueue::Nx2WriterEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_DESC_WRITER");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void UpdateDescriptorQueue::EndReservation() noexcept {
    if (t_reservation != &reservation) {
        return;
    }
    t_reservation = nullptr;
    auto& stats = writer_stats;
    ++stats.draws;
    const auto written = reservation.cursor - reservation.begin;
    const auto reserved = reservation.end - reservation.begin;
    stats.entries += static_cast<u64>(written);
    if (written > reserved) [[unlikely]] {
        ++stats.overflows;
        // Keep the shared cursor behind what was written (one thread: nothing else wrote there).
        payload_cursor = reservation.cursor;
    } else if (written < reserved) {
        ++stats.short_writes;
    }
    if ((stats.draws & 4095) != 0) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (stats.last_report == 0) {
        stats = {};
        stats.last_report = now;
        return;
    }
    if (now - stats.last_report < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(now - stats.last_report) / 1e9;
    LOG_INFO(Render_Vulkan,
             "Descriptor writer: Draws {:.0f}/s, entries {:.1f}/draw | more than reserved {:.0f}/s, "
             "fewer {:.0f}/s, without reservation {:.0f}/s",
             static_cast<double>(stats.draws) / seconds,
             static_cast<double>(stats.entries) / static_cast<double>(stats.draws),
             static_cast<double>(stats.overflows) / seconds,
             static_cast<double>(stats.short_writes) / seconds,
             static_cast<double>(stats.fallbacks) / seconds);
    stats = {};
    stats.last_report = now;
}

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <variant>
#include <vector>
#include "common/nx2_cache_guard.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct DescriptorAddress {
    VkDeviceAddress address;
    VkDeviceSize range;
    VkFormat format;
};

union DescriptorUpdateEntry {
    DescriptorUpdateEntry() = default;
    DescriptorUpdateEntry(VkDescriptorImageInfo image_) : image{image_} {}
    DescriptorUpdateEntry(VkDescriptorBufferInfo buffer_) : buffer{buffer_} {}
    DescriptorUpdateEntry(VkBufferView texel_buffer_) : texel_buffer{texel_buffer_} {}
    DescriptorUpdateEntry(DescriptorAddress address_) : address{address_} {}
    std::monostate empty{};
    VkDescriptorImageInfo image;
    VkDescriptorBufferInfo buffer;
    VkBufferView texel_buffer;
    DescriptorAddress address;
};

class UpdateDescriptorQueue final {
    // This should be plenty for the vast majority of cases. Most desktop platforms only
    // provide up to 3 swapchain images.
    static constexpr size_t FRAMES_IN_FLIGHT = 8;

public:
    static constexpr size_t GUEST_FRAME_PAYLOAD_SIZE = 0x80000;
    static constexpr size_t COMPUTE_FRAME_PAYLOAD_SIZE = 0x20000;

    explicit UpdateDescriptorQueue(const Device& device_, size_t frame_payload_size_,
                                   bool supports_descriptor_buffer_ = false);
    ~UpdateDescriptorQueue();

    [[nodiscard]] bool UsesDescriptorBuffer() const noexcept {
        return use_descriptor_buffer;
    }

    void TickFrame();
    void Acquire(Scheduler& scheduler, size_t required_entries = 0,
                 bool use_descriptor_buffer_ = false);

    const DescriptorUpdateEntry* UpdateData() const noexcept {
        return t_reservation == &reservation ? reservation.begin : upload_start;
    }

    /// B2 step 3, piece 3 (fork mkw60, EDEN_NX2_DESC_WRITER=1): a draw reserves exactly the
    /// entries its pipeline needs when it acquires, and writes them through its own cursor
    /// instead of the shared one. Parallel preparers can then fill their ranges at the same time;
    /// only the reservation (and the frame wrap) stays shared. EndReservation checks that the draw
    /// wrote exactly what it reserved (more would land in the next draw's range).
    static bool Nx2WriterEnabled();
    struct Reservation {
        DescriptorUpdateEntry* begin{};
        DescriptorUpdateEntry* cursor{};
        DescriptorUpdateEntry* end{};
    };
    static inline thread_local Reservation* t_reservation{};
    /// Reserves `entries` for the current draw (call right after Acquire, end with EndReservation).
    void Reserve(size_t entries);
    void EndReservation() noexcept;

    /// P1 (EDEN_NX2_PREP_SPLIT=1): this queue's entries go to `buffer` until EndCapture, which
    /// returns how many were written (the serial part makes the uniform entries before the
    /// preparation acquires). AddEntries copies such entries into the current draw's range.
    void BeginCapture(DescriptorUpdateEntry* buffer, size_t capacity) noexcept {
        capture = {buffer, buffer, buffer + capacity};
        t_reservation = &capture;
    }
    size_t EndCapture() noexcept {
        if (t_reservation == &capture) {
            t_reservation = nullptr;
        }
        return static_cast<size_t>(capture.cursor - capture.begin);
    }
    void AddEntries(const DescriptorUpdateEntry* entries, size_t count) noexcept {
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue, count); // B2 cache guard
        DescriptorUpdateEntry*& cursor = Cursor();
        std::copy_n(entries, count, cursor);
        cursor += count;
    }

    void AddSampledImage(VkImageView image_view, VkSampler sampler) {
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
        *(Cursor()++) = VkDescriptorImageInfo{
            .sampler = sampler,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
    }

    void AddImage(VkImageView image_view) {
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
        *(Cursor()++) = VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
    }

    void AddBuffer(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
        *(Cursor()++) = VkDescriptorBufferInfo{
            .buffer = buffer,
            .offset = offset,
            .range = size,
        };
    }

    void AddBuffer(VkBuffer buffer, VkDeviceAddress base_address, VkDeviceSize offset,
                   VkDeviceSize size) {
        if (!use_descriptor_buffer) {
            AddBuffer(buffer, offset, size);
            return;
        }
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
        *(Cursor()++) = DescriptorAddress{
            .address = base_address == 0 ? 0 : base_address + offset,
            .range = base_address == 0 ? VK_WHOLE_SIZE : size,
            .format = VK_FORMAT_UNDEFINED,
        };
    }

    void AddTexelBuffer(VkBufferView texel_buffer) {
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
        *(Cursor()++) = texel_buffer;
    }

    void AddTexelBuffer(VkBufferView texel_buffer, VkDeviceAddress base_address,
                        VkDeviceSize offset, VkDeviceSize size, VkFormat format) {
        if (!use_descriptor_buffer) {
            AddTexelBuffer(texel_buffer);
            return;
        }
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::DescriptorQueue); // B2 cache guard
        *(Cursor()++) = DescriptorAddress{
            .address = base_address == 0 ? 0 : base_address + offset,
            .range = base_address == 0 ? VK_WHOLE_SIZE : size,
            .format = format,
        };
    }

private:
    DescriptorUpdateEntry*& Cursor() noexcept {
        // Only this queue's reservation: compute passes inside a draw use other queues.
        return t_reservation == &reservation || t_reservation == &capture ? t_reservation->cursor
                                                                           : payload_cursor;
    }

    Reservation reservation{};
    Reservation capture{};
    struct WriterStats {
        u64 draws{}, fallbacks{}, overflows{}, short_writes{}, entries{};
        s64 last_report{};
    } writer_stats{};

    const Device& device;
    const size_t frame_payload_size;
    const bool supports_descriptor_buffer;
    bool use_descriptor_buffer{false};
    size_t frame_index{0};
    DescriptorUpdateEntry* payload_cursor = nullptr;
    DescriptorUpdateEntry* payload_start = nullptr;
    const DescriptorUpdateEntry* upload_start = nullptr;
    std::unique_ptr<DescriptorUpdateEntry[]> payload;
};

// TODO: should these be separate classes instead?
using GuestDescriptorQueue = UpdateDescriptorQueue;
using ComputePassDescriptorQueue = UpdateDescriptorQueue;

} // namespace Vulkan

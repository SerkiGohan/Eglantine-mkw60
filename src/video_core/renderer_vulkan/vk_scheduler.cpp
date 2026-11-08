// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <string>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "common/nx2_wait_stats.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {
/// P3 S3 statistics (back thread; worker waits counted atomically).
struct Nx2BlockStats {
    u64 blocks{}, draws{}, early{}, main_fresh{};
    std::unordered_map<const char*, u64> reasons; ///< what ended the blocks
    std::atomic<u64> worker_waits{};
    s64 last_report{};
};
Nx2BlockStats g_nx2_block_stats;
thread_local std::shared_ptr<Scheduler::Nx2Block> t_nx2_block_owner;

} // namespace

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)} {

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Flush");
    }
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    return signal_value;
}

bool Scheduler::DeferFlush() {
    // Fences come one per compute release: ~11,500 a second in Hyrule Warriors: Age of
    // Imprisonment fights, each a queue submission (~22,000 a second with the others), which
    // kept the Vulkan worker busy submitting. Batched within 500 us: ~10-15% more fps there.
    // Command list copies after dispatches were ~9,000 more a second, of which the front waited
    // for about 5%.
    static const s64 interval_us = [] {
        const char* value = std::getenv("EDEN_NX2_FENCE_FLUSH_US");
        return value == nullptr ? s64{500} : static_cast<s64>(std::atoll(value));
    }();
    if (interval_us <= 0 || !VideoCommon::GpuSplit::OnBackThread() ||
        std::chrono::steady_clock::now() - last_flush >= std::chrono::microseconds{interval_us}) {
        return false;
    }
    flush_pending = true;
    return true;
}

void Scheduler::FlushDeferred() {
    if (flush_pending) {
        Common::Nx2WaitStats::TagNextSubmit(Common::Nx2WaitStats::Site::SubmitDeferred);
        Flush();
    }
}

bool Scheduler::WaitSubmitted(u64 tick) {
    if (IsSubmitted(tick)) {
        return true;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    split.RequestIdleHook();
    // No time limit: the back can spend seconds on one entry (a large shader's translation),
    // and a fence reported as submitted before it is would release its downloads too early.
    const auto start = std::chrono::steady_clock::now();
    auto next_log = start + std::chrono::seconds{2};
    for (u32 spins = 0; !IsSubmitted(tick); ++spins) {
        if (!split.BackRunning()) {
            return IsSubmitted(tick);
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= next_log) {
            next_log = now + std::chrono::seconds{2};
            LOG_WARNING(Render_Vulkan, "Waited {} s for the GPU thread to submit tick {}",
                        std::chrono::duration_cast<std::chrono::seconds>(now - start).count(),
                        tick);
            split.RequestIdleHook();
        }
        if (spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds{50});
        }
    }
    return true;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Finish");
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
}

void Scheduler::WaitWorker() {
    Nx2CloseBlockEarly(); // P3 S3
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork() {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::DispatchWork");
    }
    if (chunk && !chunk->Empty()) {
        // Waking the worker is a system call (~2% of the GPU thread at one dispatch per 8
        // draws): skip it while the worker is still busy, it pops the queue before waiting.
        bool wake;
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
            wake = worker_waiting;
        }
        if (wake) {
            event_cv.notify_all();
        }
        AcquireNewChunk();
    }
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    const bool after_compute = std::exchange(compute_writes_pending, false);
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count,
            after_compute](vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        if (after_compute) {
            // Draws read compute output as indirect arguments, vertices and resources; nothing
            // else orders those reads after the dispatch.
            static constexpr VkMemoryBarrier COMPUTE_TO_GRAPHICS{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, COMPUTE_TO_GRAPHICS);
        }
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass();
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer) {
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

void Scheduler::RequestOutsideRenderPassOperationContext() {
    EndRenderPass();
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    Nx2EnsureMainFresh(); // P3 S3b
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    Nx2EnsureMainFresh(); // P3 S3b
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    Nx2EnsureMainFresh(); // P3 S3b
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] {
                worker_waiting = !TryPopQueue(work);
                return !worker_waiting;
            });
            worker_waiting = false;

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    Nx2CloseBlockEarly(); // P3 S3: a submission ends the open block
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    // Wait statistics: the time at which the back thread finishes this submission
    Common::Nx2WaitStats::NoteFlush(signal_value);
    Common::Nx2WaitStats::CountSubmit();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        // Here in the VulkanWorker all commands before it are recorded: how long did that take?
        if (Common::Nx2WaitStats::Enabled()) {
            const s64 flushed = Common::Nx2WaitStats::FlushTime(signal_value);
            const s64 now = Common::Nx2WaitStats::NowNs();
            if (flushed > 0 && now - flushed < 1'000'000'000) {
                Common::Nx2WaitStats::AddDuration(Common::Nx2WaitStats::Site::WorkerBacklog,
                                                  now - flushed);
            }
        }
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        VkResult submit_result;
        {
            const Common::Nx2WaitStats::Scope submit_scope{Common::Nx2WaitStats::Site::VkSubmit};
            submit_result = master_semaphore->SubmitQueue(cmdbuf, upload_cmdbuf, signal_semaphore,
                                                          wait_semaphore, signal_value);
        }
        switch (const VkResult result = submit_result) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    });
    chunk->MarkSubmit();
    DispatchWork();
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    if (t_nx2_block == nullptr && nx2_main_stale) {
        nx2_main_stale = false;
        ++g_nx2_block_stats.main_fresh;
    }
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    // Pause, don't reset: a submission can land between a guest's counter reset and its report,
    // and resetting here would drop the samples counted so far (the report then reads too low).
    query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
}

void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            return;
        }

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });

        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
    }


namespace {
void Nx2ReportBlocks() {
    auto& stats = g_nx2_block_stats;
    if ((stats.blocks & 1023) != 0) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (stats.last_report == 0) {
        stats.blocks = stats.draws = stats.early = stats.main_fresh = 0;
        stats.worker_waits.store(0, std::memory_order_relaxed);
        stats.last_report = now;
        return;
    }
    if (now - stats.last_report < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(now - stats.last_report) / 1e9;
    LOG_INFO(Render_Vulkan,
             "Prep record: blocks {:.0f}/s, {:.1f} draws/block | closed early {:.0f}/s | worker "
             "waited {:.0f}/s | main stream reset {:.0f}/s",
             static_cast<double>(stats.blocks) / seconds,
             stats.blocks ? static_cast<double>(stats.draws) / static_cast<double>(stats.blocks) : 0.0,
             static_cast<double>(stats.early) / seconds,
             static_cast<double>(stats.worker_waits.exchange(0, std::memory_order_relaxed)) / seconds,
             static_cast<double>(stats.main_fresh) / seconds);
    // What ended the blocks: demangled command types (the recording function is in the name).
    std::vector<std::pair<const char*, u64>> top(stats.reasons.begin(), stats.reasons.end());
    std::ranges::sort(top, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t i = 0; i < top.size() && i < 6; ++i) {
        // "... [with T = Record(...) [with T = Recorder::Function(args)::<lambda(...)>]...]":
        // the innermost type names the recording function; keep its name without arguments.
        std::string name = top[i].first;
        if (const size_t at = name.rfind("[with T = "); at != std::string::npos) {
            name = name.substr(at + 10);
        }
        if (const size_t at = name.find("::<lambda"); at != std::string::npos) {
            name.resize(at);
        }
        if (const size_t at = name.find('('); at != std::string::npos) {
            name.resize(at);
        }
        if (name.size() > 70) {
            name = name.substr(name.size() - 70);
        }
        text += fmt::format(" | {:.0f}/s {}", static_cast<double>(top[i].second) / seconds, name);
    }
    LOG_INFO(Render_Vulkan, "Prep record block end by:{}", text);
    stats.reasons.clear();
    stats.blocks = stats.draws = stats.early = stats.main_fresh = 0;
    stats.last_report = now;
}
} // namespace

struct Scheduler::Nx2Context {
    GraphicsPipeline* graphics_pipeline{};
    bool is_rescaling{};
    bool rescaling_defined{};
    bool needs_state_enable_refresh{};
    u32 descriptor_buffer_chunk{};
    bool descriptor_buffer_bound{};
    StateTracker::Nx2Scalars tracker{};
};

void Scheduler::Nx2SaveContext(Nx2Context& context) const {
    context.graphics_pipeline = state.graphics_pipeline;
    context.is_rescaling = state.is_rescaling;
    context.rescaling_defined = state.rescaling_defined;
    context.needs_state_enable_refresh = state.needs_state_enable_refresh;
    context.descriptor_buffer_chunk = state.descriptor_buffer_chunk;
    context.descriptor_buffer_bound = state.descriptor_buffer_bound;
    context.tracker = state_tracker.Nx2Save();
}

void Scheduler::Nx2LoadContext(const Nx2Context& context) {
    state.graphics_pipeline = context.graphics_pipeline;
    state.is_rescaling = context.is_rescaling;
    state.rescaling_defined = context.rescaling_defined;
    state.needs_state_enable_refresh = context.needs_state_enable_refresh;
    state.descriptor_buffer_chunk = context.descriptor_buffer_chunk;
    state.descriptor_buffer_bound = context.descriptor_buffer_bound;
    state_tracker.Nx2Load(context.tracker);
}

void Scheduler::Nx2EnterBlock(u32 max_draws) {
    if (t_nx2_block != nullptr) {
        return;
    }
    if (!nx2_main_context) {
        nx2_main_context = std::make_unique<Nx2Context>();
        nx2_block_context = std::make_unique<Nx2Context>();
    }
    ++g_nx2_block_stats.draws;
    if (t_nx2_paused_block && t_nx2_block_draws < max_draws) {
        // Continue the paused block with its own state.
        Nx2SaveContext(*nx2_main_context);
        Nx2LoadContext(*nx2_block_context);
        t_nx2_block_owner = std::move(t_nx2_paused_block);
        t_nx2_block = t_nx2_block_owner.get();
        ++t_nx2_block_draws;
        return;
    }
    Nx2CompletePaused();
    t_nx2_block_max = max_draws;
    Nx2BeginNewBlock();
}

void Scheduler::Nx2BeginNewBlock() {
    auto block = std::make_shared<Nx2Block>();
    RecordWithUploadBuffer([this, block](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        if (!block->done.load(std::memory_order_acquire)) {
            g_nx2_block_stats.worker_waits.fetch_add(1, std::memory_order_relaxed);
            block->done.wait(false, std::memory_order_acquire);
        }
        for (auto& block_chunk : block->chunks) {
            block_chunk->ExecuteAll(cmdbuf, upload_cmdbuf);
        }
        std::scoped_lock rl{reserve_mutex};
        for (auto& block_chunk : block->chunks) {
            chunk_reserve.emplace_back(std::move(block_chunk));
        }
        block->chunks.clear();
    });
    // A new block knows nothing about the GPU state: everything is set again inside it.
    Nx2SaveContext(*nx2_main_context);
    t_nx2_block_owner = std::move(block);
    t_nx2_block = t_nx2_block_owner.get();
    InvalidateState();
    t_nx2_block_draws = 1;
}

void Scheduler::Nx2PauseBlock() {
    if (t_nx2_block == nullptr) {
        return;
    }
    Nx2SaveContext(*nx2_block_context);
    Nx2LoadContext(*nx2_main_context);
    nx2_main_stale = true; // the GPU now has the block's state
    t_nx2_paused_block = std::move(t_nx2_block_owner);
    t_nx2_block = nullptr;
    if (t_nx2_block_draws >= t_nx2_block_max) {
        Nx2CompletePaused("full");
    }
}

void Scheduler::Nx2CompletePaused(const char* reason) {
    if (!t_nx2_paused_block) {
        return;
    }
    ++g_nx2_block_stats.reasons[reason];
    const auto block = std::move(t_nx2_paused_block);
    block->done.store(true, std::memory_order_release);
    block->done.notify_all();
    ++g_nx2_block_stats.blocks;
    Nx2ReportBlocks();
}

void Scheduler::Nx2EndBlock() {
    Nx2PauseBlock();
    Nx2CompletePaused("submit/wait");
}

void Scheduler::Nx2CloseBlockEarly() {
    if (t_nx2_block != nullptr) [[unlikely]] {
        ++g_nx2_block_stats.early;
    }
    Nx2EndBlock();
}

std::unique_ptr<Scheduler::CommandChunk> Scheduler::Nx2TakeChunk() {
    std::scoped_lock rl{reserve_mutex};
    if (chunk_reserve.empty()) {
        return std::make_unique<CommandChunk>();
    }
    auto taken = std::move(chunk_reserve.back());
    chunk_reserve.pop_back();
    return taken;
}

void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/host1x/syncpoint_manager.h"
#include "common/nx2_wait_stats.h"

namespace Tegra {

namespace Host1x {

namespace {
// Measures only the WAIT for the lock, not the time it is held.
s64 GuardWaitStart() {
    return Common::Nx2WaitStats::Enabled() ? Common::Nx2WaitStats::NowNs() : 0;
}
void GuardWaitEnd(s64 start) {
    if (start != 0) {
        Common::Nx2WaitStats::Add(Common::Nx2WaitStats::Site::SyncptGuardWait, start);
    }
}
} // Anonymous namespace

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    const s64 wait_start = GuardWaitStart();
    std::scoped_lock lk(guard);
    GuardWaitEnd(wait_start);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        action();
        return {};
    }
    Common::Nx2WaitStats::NoteActions(action_storage.size() + 1);
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, std::move(action));
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    const s64 wait_start = GuardWaitStart();
    std::scoped_lock lk(guard);
    GuardWaitEnd(wait_start);

    // We want to ensure the iterator still exists prior to erasing it
    // Otherwise, if an invalid iterator was passed in then it could lead to UB
    // It is important to avoid UB in that case since the deregister isn't called from a locked
    // context
    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it == handle) {
            action_storage.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id]);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id]);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage) {
    auto new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};

    const s64 wait_start = GuardWaitStart();
    std::scoped_lock lk(guard);
    GuardWaitEnd(wait_start);
    {
        const Common::Nx2WaitStats::Scope actions_scope{Common::Nx2WaitStats::Site::SyncptActions};
        auto it = action_storage.begin();
        while (it != action_storage.end()) {
            if (it->expected_value > new_value) {
                break;
            }
            it->action();
            it = action_storage.erase(it);
        }
    }
    wait_cv.notify_all();
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    const Common::Nx2WaitStats::Scope wait_scope{Common::Nx2WaitStats::Site::SyncptWait};
    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra

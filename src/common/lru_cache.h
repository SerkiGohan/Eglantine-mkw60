// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <deque>
#include <memory>
#include <type_traits>
#include <vector>

#include "common/common_types.h"
#include "common/nx2_cache_guard.h"

namespace Common {

/// B2 step 3, piece 4 (fork mkw60, EDEN_NX2_LRU_DEFER=1): while a draw is prepared, LRU touches are
/// collected per thread and applied at the end of the draw (parallel preparers must not relink
/// the shared list). Nx2LruDefer::Flush applies them; Free drops pending touches of its item.
namespace Nx2LruDefer {
inline const bool g_enabled = [] {
    const char* value = std::getenv("EDEN_NX2_LRU_DEFER");
    return value != nullptr && value[0] == '1';
}();
struct Entry {
    void* cache;
    size_t id;
    u64 tick;
    void (*apply)(void*, size_t, u64);
};
inline thread_local bool t_active = false;
inline thread_local std::vector<Entry> t_log;
inline u64 g_deferred = 0; ///< statistics (back thread)
inline u64 g_dropped = 0;
inline void Flush() {
    t_active = false;
    for (const Entry& entry : t_log) {
        entry.apply(entry.cache, entry.id, entry.tick);
    }
    g_deferred += t_log.size();
    t_log.clear();
}
} // namespace Nx2LruDefer

template <class Traits>
class LeastRecentlyUsedCache {
    using ObjectType = typename Traits::ObjectType;
    using TickType = typename Traits::TickType;

    struct Item {
        ObjectType obj;
        TickType tick;
        Item* next{};
        Item* prev{};
    };

public:
    LeastRecentlyUsedCache() : first_item{}, last_item{} {}
    ~LeastRecentlyUsedCache() = default;

    size_t Insert(ObjectType obj, TickType tick) {
        const auto new_id = Build();
        auto& item = item_pool[new_id];
        item.obj = obj;
        item.tick = tick;
        Attach(item);
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::LruInsertFree); // B2 cache guard
        return new_id;
    }

    void Touch(size_t id, TickType tick) {
        if (Nx2LruDefer::t_active) [[unlikely]] {
            Nx2LruDefer::t_log.push_back({this, id, static_cast<u64>(tick), &ApplyDeferredTouch});
            return;
        }
        TouchNow(id, tick);
    }

    static void ApplyDeferredTouch(void* cache, size_t id, u64 tick) {
        static_cast<LeastRecentlyUsedCache*>(cache)->TouchNow(id, static_cast<TickType>(tick));
    }

    void TouchNow(size_t id, TickType tick) {
        auto& item = item_pool[id];
        if (item.tick >= tick) {
            return;
        }
        item.tick = tick;
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::LruTouch); // B2 cache guard
        if (&item == last_item) {
            return;
        }
        Detach(item);
        Attach(item);
    }

    void Free(size_t id) {
        Common::Nx2CacheGuard::Note(Common::Nx2CacheGuard::LruInsertFree); // B2 cache guard
        if (!Nx2LruDefer::t_log.empty()) [[unlikely]] {
            auto& log = Nx2LruDefer::t_log;
            const auto before = log.size();
            std::erase_if(log, [&](const Nx2LruDefer::Entry& entry) {
                return entry.cache == this && entry.id == id;
            });
            Nx2LruDefer::g_dropped += before - log.size();
        }
        auto& item = item_pool[id];
        Detach(item);
        item.prev = nullptr;
        item.next = nullptr;
        free_items.push_back(id);
    }

    template <typename Func>
    void ForEachItemBelow(TickType tick, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result_t<Func, ObjectType>, bool>;
        Item* iterator = first_item;
        while (iterator) {
            if (static_cast<s64>(tick) - static_cast<s64>(iterator->tick) < 0) {
                return;
            }
            Item* next = iterator->next;
            if constexpr (RETURNS_BOOL) {
                if (func(iterator->obj)) {
                    return;
                }
            } else {
                func(iterator->obj);
            }
            iterator = next;
        }
    }

private:
    size_t Build() {
        if (free_items.empty()) {
            const size_t item_id = item_pool.size();
            auto& item = item_pool.emplace_back();
            item.next = nullptr;
            item.prev = nullptr;
            return item_id;
        }
        const size_t item_id = free_items.front();
        free_items.pop_front();
        auto& item = item_pool[item_id];
        item.next = nullptr;
        item.prev = nullptr;
        return item_id;
    }

    void Attach(Item& item) {
        if (!first_item) {
            first_item = &item;
        }
        if (!last_item) {
            last_item = &item;
        } else {
            item.prev = last_item;
            last_item->next = &item;
            item.next = nullptr;
            last_item = &item;
        }
    }

    void Detach(Item& item) {
        if (item.prev) {
            item.prev->next = item.next;
        }
        if (item.next) {
            item.next->prev = item.prev;
        }
        if (&item == first_item) {
            first_item = item.next;
            if (first_item) {
                first_item->prev = nullptr;
            }
        }
        if (&item == last_item) {
            last_item = item.prev;
            if (last_item) {
                last_item->next = nullptr;
            }
        }
    }

    std::deque<Item> item_pool;
    std::deque<size_t> free_items;
    Item* first_item{};
    Item* last_item{};
};

} // namespace Common

#pragma once

#include "core/sim_time.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

namespace netsim {

// Deterministic discrete-event queue: entries run in time order, and entries
// due at the same time run in the order they were scheduled. Cancelled
// entries are skipped when they reach the front.
template<class Payload>
class Scheduler {
public:
    struct Handle {
        std::uint64_t id = 0;  // 0: no entry
        explicit operator bool() const { return id != 0; }
    };
    struct Due {
        SimTime time;
        Payload payload;
    };

    SimTime now() const { return now_; }
    bool empty() const { return pending_.empty(); }
    std::size_t size() const { return pending_.size(); }

    Handle schedule(SimTime at, Payload payload) {
        assert(at >= now_);
        const std::uint64_t id = ++last_id_;
        queue_.push({at, id, std::move(payload)});
        pending_.insert(id);
        return {id};
    }

    // True if the entry was still pending.
    bool cancel(Handle& handle) {
        const bool erased = handle && pending_.erase(handle.id) > 0;
        handle = {};
        return erased;
    }

    // Removes the next pending entry and advances the clock to its time.
    std::optional<Due> pop() {
        while (!queue_.empty()) {
            Entry entry = queue_.top();
            queue_.pop();
            if (pending_.erase(entry.id) == 0) continue;  // cancelled
            now_ = entry.time;
            return Due{entry.time, std::move(entry.payload)};
        }
        return std::nullopt;
    }

private:
    struct Entry {
        SimTime time;
        std::uint64_t id;
        Payload payload;
    };
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const {
            return a.time > b.time || (a.time == b.time && a.id > b.id);
        }
    };
    std::priority_queue<Entry, std::vector<Entry>, Later> queue_;
    // Membership only, never iterated: hashing order cannot affect results.
    std::unordered_set<std::uint64_t> pending_;
    std::uint64_t last_id_ = 0;
    SimTime now_{};
};

} // namespace netsim

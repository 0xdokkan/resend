#pragma once

#include "core/sim_time.hpp"

#include <cassert>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace netsim {

// Deterministic discrete-event queue: entries run in time order, and entries
// due at the same time run in the order they were scheduled. Cancelling
// removes the entry at once, so every pending entry is a live one.
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
    bool empty() const { return queue_.empty(); }
    std::size_t size() const { return queue_.size(); }
    // Time of the next pending entry, without removing it.
    std::optional<SimTime> next_time() const {
        if (queue_.empty()) return std::nullopt;
        return queue_.begin()->first.time;
    }

    Handle schedule(SimTime at, Payload payload) {
        assert(at >= now_);
        const std::uint64_t id = ++last_id_;
        queue_.emplace(Key{at, id}, std::move(payload));
        times_.emplace(id, at);
        return {id};
    }

    // True if the entry was still pending.
    bool cancel(Handle& handle) {
        bool erased = false;
        if (handle) {
            if (const auto it = times_.find(handle.id); it != times_.end()) {
                queue_.erase(Key{it->second, handle.id});
                times_.erase(it);
                erased = true;
            }
        }
        handle = {};
        return erased;
    }

    // Removes the next pending entry and advances the clock to its time.
    std::optional<Due> pop() {
        if (queue_.empty()) return std::nullopt;
        auto node = queue_.extract(queue_.begin());
        times_.erase(node.key().id);
        now_ = node.key().time;
        return Due{now_, std::move(node.mapped())};
    }

    // Moves the clock forward to `at`, which must not skip a pending entry.
    // Entries scheduled afterwards start from there.
    void advance_to(SimTime at) {
        assert(queue_.empty() || queue_.begin()->first.time >= at);
        if (at > now_) now_ = at;
    }

private:
    struct Key {
        SimTime time;
        std::uint64_t id;
        auto operator<=>(const Key&) const = default;
    };
    std::map<Key, Payload> queue_;
    // Lookup only, never iterated: hashing order cannot affect results.
    std::unordered_map<std::uint64_t, SimTime> times_;
    std::uint64_t last_id_ = 0;
    SimTime now_{};
};

} // namespace netsim

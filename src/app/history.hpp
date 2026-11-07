#pragma once

#include "scenario/edit.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace resend {

// Undo and redo for the network being edited. A step holds the edits it
// made and the edits that take them back, both in the order to make them;
// netsim::edit::inverse() computes the latter exactly, so undoing gives
// back the very same scenario.
class History {
public:
    struct Step {
        std::vector<netsim::Edit> forward, backward;
        std::string label;  // what the step did, e.g. "Move PC-A"
        std::uint64_t state = 0;
    };
    // Steps kept: the oldest ones are forgotten first.
    static constexpr std::size_t limit = 500;

    // Records a step and forgets what could be redone. A step pushed as part
    // of a gesture joins the previous one if that was part of the same
    // gesture, so a whole drag is undone at once.
    void push(std::vector<netsim::Edit> forward, std::vector<netsim::Edit> backward, std::string label, bool gesture = false);
    // Ends the gesture in progress: the next step stands on its own.
    void seal() { open_ = false; }
    void clear();

    // The step undo would take back, or redo would make again.
    const Step* undoable() const { return done_ > 0 ? &steps_[done_ - 1] : nullptr; }
    const Step* redoable() const { return done_ < steps_.size() ? &steps_[done_] : nullptr; }
    void undone();
    void redone();

    // Identifies the scenario the history has reached: equal values mean the
    // same scenario, e.g. the one last saved. 0 before any step.
    std::uint64_t state() const { return done_ > 0 ? steps_[done_ - 1].state : first_; }
    std::size_t size() const { return steps_.size(); }

private:
    std::deque<Step> steps_;
    std::size_t done_ = 0;  // steps before this index are made
    std::uint64_t last_state_ = 0;
    std::uint64_t first_ = 0;  // the state before the oldest step kept
    bool open_ = false;        // the last step is a gesture still going on
};

} // namespace resend

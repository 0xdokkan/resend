#include "app/history.hpp"

#include <iterator>
#include <utility>
#include <variant>

namespace resend {
namespace {
// A drag moves a card many times: only the last position of a run of moves
// of one card matters.
void collapse_moves(std::vector<netsim::Edit>& edits) {
    std::vector<netsim::Edit> kept;
    for (auto& edit : edits) {
        const auto* move = std::get_if<netsim::MoveDevice>(&edit);
        const auto* previous = kept.empty() ? nullptr : std::get_if<netsim::MoveDevice>(&kept.back());
        if (move && previous && previous->device == move->device) kept.back() = std::move(edit);
        else kept.push_back(std::move(edit));
    }
    edits = std::move(kept);
}
} // namespace

void History::push(std::vector<netsim::Edit> forward, std::vector<netsim::Edit> backward, std::string label, bool gesture) {
    const bool join = gesture && open_ && done_ == steps_.size() && done_ > 0 && steps_.back().label == label;
    steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(done_), steps_.end());
    if (join) {
        auto& step = steps_.back();
        step.forward.insert(step.forward.end(), std::make_move_iterator(forward.begin()), std::make_move_iterator(forward.end()));
        backward.insert(backward.end(), std::make_move_iterator(step.backward.begin()), std::make_move_iterator(step.backward.end()));
        step.backward = std::move(backward);
        collapse_moves(step.forward);
        collapse_moves(step.backward);
        step.state = ++last_state_;
    } else {
        steps_.push_back({std::move(forward), std::move(backward), std::move(label), ++last_state_});
        if (steps_.size() > limit) {
            first_ = steps_.front().state;
            steps_.pop_front();
        }
        done_ = steps_.size();
    }
    open_ = gesture;
}

void History::clear() {
    steps_.clear();
    done_ = 0;
    first_ = ++last_state_;
    open_ = false;
}

void History::undone() {
    if (done_ > 0) --done_;
    open_ = false;
}

void History::redone() {
    if (done_ < steps_.size()) ++done_;
    open_ = false;
}

} // namespace resend

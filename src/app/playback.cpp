#include "app/playback.hpp"

#include <algorithm>
#include <cstdint>

namespace resend {

namespace {
class SimulationTimeline final : public Timeline {
public:
    explicit SimulationTimeline(netsim::Simulation& simulation) : simulation_(simulation) {}
    const netsim::Simulation& simulation() const override { return simulation_; }
    std::optional<netsim::SimTime> next_event_time() const override { return simulation_.next_event_time(); }
    void run_until(netsim::SimTime time) override { simulation_.run_until(time); }

private:
    netsim::Simulation& simulation_;
};
} // namespace

PlaybackController::PlaybackController(Timeline& timeline) : timeline_(timeline), view_(timeline.simulation().now()) {}

PlaybackController::PlaybackController(netsim::Simulation& simulation)
    : own_(std::make_unique<SimulationTimeline>(simulation)), timeline_(*own_), view_(simulation.now()) {}

void PlaybackController::play() {
    dwell_ = {};
    mode_ = finished() && view_ >= calm() ? Mode::Paused : Mode::Playing;
}

netsim::SimTime PlaybackController::calm() const {
    const auto& records = timeline_.simulation().trace().records();
    return records.empty() ? netsim::SimTime{} : records.back().time + afterglow;
}

void PlaybackController::pause() { mode_ = Mode::Paused; }

void PlaybackController::step() {
    const auto next = timeline_.next_event_time();
    mode_ = Mode::Paused;
    if (!next) return;
    if (*next <= view_ || reduced_motion_) {
        show(std::max(*next, view_));
        return;
    }
    mode_ = Mode::Stepping;
}

void PlaybackController::reset() {
    mode_ = Mode::Paused;
    view_ = timeline_.simulation().now();
    carry_ = 0;
    gap_end_.reset();
    dwell_ = {};
    first_active_ = 0;
}

void PlaybackController::seek(netsim::SimTime time) {
    mode_ = Mode::Paused;
    if (time > view_) show(time);
}

void PlaybackController::set_speed(double speed) {
    speed_ = std::clamp(speed, min_speed, max_speed);
    gap_end_.reset();  // the idle rate depends on the speed
}

void PlaybackController::set_reduced_motion(bool reduced) {
    reduced_motion_ = reduced;
    dwell_ = {};
}

void PlaybackController::show(netsim::SimTime time) {
    view_ = time;
    carry_ = 0;
    timeline_.run_until(view_);
}

void PlaybackController::end_of_step() {
    if (mode_ == Mode::Stepping) mode_ = Mode::Paused;
}

double PlaybackController::rate_until(netsim::SimTime next) {
    const double rate = base_rate * speed_;
    if (!in_flight().empty() || view_ < calm()) {
        gap_end_.reset();
        return rate;
    }
    if (gap_end_ != next) {
        gap_end_ = next;
        const double longest = static_cast<double>(idle_limit.count()) / speed_;
        gap_rate_ = std::max(rate, static_cast<double>((next - view_).count()) / longest);
    }
    return gap_rate_;
}

bool PlaybackController::tick(RealTime elapsed) {
    bool changed = false;
    double budget = static_cast<double>(elapsed.count());
    while (mode_ != Mode::Paused) {
        const auto next = timeline_.next_event_time();
        if (!next) {
            // Nothing left: the afterglow, then a pause.
            const auto end = calm();
            if (view_ >= end) {
                mode_ = Mode::Paused;
                break;
            }
            if (reduced_motion_) {
                // The last instant stays as long as any other.
                dwell_ += RealTime{static_cast<std::int64_t>(budget)};
                if (static_cast<double>(dwell_.count()) < static_cast<double>(reduced_dwell.count()) / speed_) break;
                dwell_ = {};
                show(end);
            } else {
                const double move = budget * base_rate * speed_ + carry_;
                const auto whole = static_cast<std::int64_t>(move);
                const double rest = move - static_cast<double>(whole);
                show(std::min(end, view_ + netsim::SimTime{whole}));
                carry_ = rest;
            }
            changed = true;
            if (view_ >= end) mode_ = Mode::Paused;
            break;
        }
        if (*next <= view_) {
            // Due at the displayed instant, e.g. right after a ping starts.
            show(view_);
            changed = true;
            end_of_step();
            continue;
        }
        if (reduced_motion_) {
            dwell_ += RealTime{static_cast<std::int64_t>(budget)};
            budget = 0;
            if (static_cast<double>(dwell_.count()) < static_cast<double>(reduced_dwell.count()) / speed_) break;
            dwell_ = {};
            show(*next);
            changed = true;
            end_of_step();
            continue;
        }
        const double rate = rate_until(*next);
        // The afterglow ends before the gap is compressed: stop there and
        // spend the rest of the tick at the faster pace.
        const auto end = calm();
        const bool glowing = view_ < end && end < *next && in_flight().empty();
        const double distance = static_cast<double>(((glowing ? end : *next) - view_).count());
        const double move = budget * rate + carry_;
        if (glowing && move >= distance) {
            budget = std::max(0.0, budget - (distance - carry_) / rate);
            view_ = end;
            carry_ = 0;
            timeline_.run_until(view_);
            changed = true;
            continue;
        }
        if (move >= distance) {
            // The next event is reached within this tick: process it and
            // spend what is left of the tick on what follows.
            budget = std::max(0.0, budget - (distance - carry_) / rate);
            show(*next);
            changed = true;
            end_of_step();
            continue;
        }
        const auto whole = static_cast<std::int64_t>(move);
        carry_ = move - static_cast<double>(whole);
        if (whole > 0) {
            view_ += netsim::SimTime{whole};
            timeline_.run_until(view_);  // nothing is due before `next`: moves the clock only
            changed = true;
        }
        break;
    }
    return changed;
}

std::vector<PlaybackController::InFlight> PlaybackController::in_flight() const {
    const auto& all = timeline_.simulation().trace().transmissions();
    if (first_active_ > all.size()) first_active_ = 0;  // the trace was cleared
    while (first_active_ < all.size() && all[first_active_].arrival <= view_) ++first_active_;
    std::vector<InFlight> result;
    for (std::size_t index = first_active_; index < all.size() && all[index].start <= view_; ++index) {
        const auto& item = all[index];
        if (item.arrival <= view_) continue;
        const auto total = static_cast<double>((item.arrival - item.start).count());
        result.push_back({index, static_cast<double>((view_ - item.start).count()) / total});
    }
    return result;
}

} // namespace resend

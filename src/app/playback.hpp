#pragma once

#include "sim/simulation.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace resend {

// What the playback drives: a simulation, or a workspace that also makes the
// commands recorded for later instants when their time comes.
class Timeline {
public:
    virtual ~Timeline() = default;
    virtual const netsim::Simulation& simulation() const = 0;
    virtual std::optional<netsim::SimTime> next_event_time() const = 0;
    // Runs every event and command due at or before `time`, then moves the clock there.
    virtual void run_until(netsim::SimTime time) = 0;
};

// Turns real time into simulated time for the animation. The simulation
// never runs ahead of the instant on screen: it is advanced to the displayed
// time, so the tables and the event list always describe what is shown.
// Playback decides when the simulation runs, never what it computes.
class PlaybackController {
public:
    using RealTime = std::chrono::nanoseconds;
    // Simulated time per real time at 1×: a 10 ms hop takes 600 ms.
    static constexpr double base_rate = 10.0 / 600.0;
    // With nothing on the cables, waiting for the next event (a timeout, for
    // example) takes at most this long at 1×, whatever the simulated wait.
    static constexpr RealTime idle_limit = std::chrono::milliseconds(800);
    // With reduced motion, events appear one instant at a time, this long
    // each at 1×, and nothing moves along the cables.
    static constexpr RealTime reduced_dwell = std::chrono::milliseconds(700);
    static constexpr double min_speed = 0.25, max_speed = 4;

    struct InFlight {
        std::size_t transmission;  // index in the trace's transmissions
        double progress;           // 0 at the sending port, towards 1 on arrival
    };

    explicit PlaybackController(Timeline& timeline);
    explicit PlaybackController(netsim::Simulation& simulation);

    void play();
    void pause();
    // Plays up to the next event, processes it and pauses. Events already
    // due run at once; with reduced motion the step is immediate.
    void step();
    // Paused at the simulation's current time; call after it is rebuilt.
    void reset();
    // Shows `time` at once, processing every event up to it. Only forwards:
    // going back means replaying the simulation.
    void seek(netsim::SimTime time);
    // Advances the animation by `elapsed` real time. True if the displayed
    // time moved or events were processed.
    bool tick(RealTime elapsed);

    bool playing() const { return mode_ != Mode::Paused; }
    bool finished() const { return !timeline_.next_event_time(); }
    netsim::SimTime now() const { return view_; }
    double speed() const { return speed_; }
    void set_speed(double speed);
    bool reduced_motion() const { return reduced_motion_; }
    void set_reduced_motion(bool reduced);
    // Transmissions on a cable at the displayed time, in the order they started.
    std::vector<InFlight> in_flight() const;

private:
    enum class Mode { Paused, Playing, Stepping };
    void show(netsim::SimTime time);
    double rate_until(netsim::SimTime next);
    void end_of_step();

    // A simulation given on its own is wrapped here.
    std::unique_ptr<Timeline> own_;
    Timeline& timeline_;
    Mode mode_ = Mode::Paused;
    netsim::SimTime view_{};
    double speed_ = 1;
    bool reduced_motion_ = false;
    double carry_ = 0;  // part of a nanosecond not shown yet
    // The idle gap being compressed: where it ends and how fast it goes.
    std::optional<netsim::SimTime> gap_end_;
    double gap_rate_ = 0;
    RealTime dwell_{};
    // Transmissions before this index have all arrived.
    mutable std::size_t first_active_ = 0;
};

} // namespace resend

#pragma once

#include "app.h"
#include "app/playback.hpp"
#include "explain/explain.hpp"
#include "scenario/scenario.hpp"
#include "sim/simulation.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace resend {

// Connects the simulation and the scenario it runs to the Slint window:
// callbacks in, models out. Before anything has happened an edit simply
// gives a new network; once the simulation has run, edits happen to the
// running network at the instant on screen. The callbacks capture `this`,
// so the controller must stay in place.
class AppController {
public:
    explicit AppController(slint::ComponentHandle<AppWindow> ui);
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    const netsim::Scenario& scenario() const { return simulation_.scenario(); }
    const netsim::Simulation& simulation() const { return simulation_; }
    const netsim::explain::Explainer& explainer() const { return explainer_; }
    const PlaybackController& playback() const { return playback_; }
    // Replaces the network, e.g. with a scenario file given at startup.
    void open(netsim::Scenario scenario);
    // Moves the animation forward. The frame timer passes the real time
    // elapsed; tests and snapshots pass a chosen one.
    void advance_playback(std::chrono::nanoseconds elapsed);
    // Shows a simulated instant at once, paused (snapshots, tests).
    void seek(netsim::SimTime time);

private:
    void bind_callbacks();
    void status(const std::string& value);
    std::string table_text() const;
    void refresh_topology();
    void refresh_scene();
    void refresh(bool edit_fields = false);
    // Makes an edit: on a new simulation if nothing has happened yet, on the
    // running one otherwise. Says why if it was refused.
    bool change(const netsim::Edit& edit, const std::string& message);
    // A new simulation of `scenario`, with the events and playback cleared.
    void rebuild(netsim::Scenario scenario, const std::string& message);
    // Applies a playback command and refreshes what it changed.
    template<class Command>
    void playback_command(Command command);
    void sync_playback();
    void select_frame(int pdu);

    slint::ComponentHandle<AppWindow> ui_;
    netsim::Simulation simulation_{netsim::sample_lan()};
    netsim::explain::Explainer explainer_;
    PlaybackController playback_{simulation_};
    std::shared_ptr<slint::VectorModel<NodeView>> nodes_;
    std::shared_ptr<slint::VectorModel<LinkView>> links_;
    std::shared_ptr<slint::VectorModel<PacketView>> packets_;
    std::shared_ptr<slint::VectorModel<MarkerView>> markers_;
    std::shared_ptr<slint::VectorModel<TraceView>> traces_;
    slint::Timer frame_timer_;
    std::chrono::steady_clock::time_point last_frame_;
    // Port activity per row of nodes_, to update only rows that change.
    std::vector<unsigned> shown_activity_;
    int selected_ = 1;
    int connection_source_ = -1;
    int selected_event_ = -1;
    int selected_pdu_ = -1;
    bool follow_events_ = true;
};

} // namespace resend

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

// Connects the scenario being edited and its simulation to the Slint window:
// callbacks in, models out. Every edit that matters to the network rebuilds
// the simulation from the scenario. The callbacks capture `this`, so the
// controller must stay in place.
class AppController {
public:
    explicit AppController(slint::ComponentHandle<AppWindow> ui);
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    const netsim::Scenario& scenario() const { return scenario_; }
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
    // After an edit: rebuilds the simulation if it succeeded, or explains
    // why it was refused.
    void edited(netsim::Problem problem, const std::string& message);
    void rebuild();
    // Applies a playback command and refreshes what it changed.
    template<class Command>
    void playback_command(Command command);
    void sync_playback();
    void select_frame(int pdu);

    slint::ComponentHandle<AppWindow> ui_;
    netsim::Scenario scenario_ = netsim::sample_lan();
    netsim::Simulation simulation_{scenario_};
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

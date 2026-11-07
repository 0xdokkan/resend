#pragma once

#include "app.h"
#include "app/playback.hpp"
#include "network.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace resend {

// Connects the engine to the Slint window: callbacks in, models out.
// The callbacks capture `this`, so the controller must stay in place.
class AppController {
public:
    explicit AppController(slint::ComponentHandle<AppWindow> ui);
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    const netsim::Engine& engine() const { return engine_; }
    const PlaybackController& playback() const { return playback_; }
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
    void mutation_result(bool success, const std::string& message);
    // Applies a playback command and refreshes what it changed.
    template<class Command>
    void playback_command(Command command);
    void sync_playback();
    void select_frame(int pdu);

    slint::ComponentHandle<AppWindow> ui_;
    netsim::Engine engine_;
    PlaybackController playback_{engine_};
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

#pragma once

#include "app.h"
#include "network.hpp"

#include <memory>
#include <string>

namespace resend {

// Connects the engine to the Slint window: callbacks in, models out.
// The callbacks capture `this`, so the controller must stay in place.
class AppController {
public:
    explicit AppController(slint::ComponentHandle<AppWindow> ui);
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    const netsim::Engine& engine() const { return engine_; }

private:
    void bind_callbacks();
    void status(const std::string& value);
    void stop();
    std::string table_text() const;
    void refresh_topology();
    void refresh(bool edit_fields = false);
    void mutation_result(bool success, const std::string& message);
    void advance();

    slint::ComponentHandle<AppWindow> ui_;
    netsim::Engine engine_;
    std::shared_ptr<slint::VectorModel<NodeView>> nodes_;
    std::shared_ptr<slint::VectorModel<LinkView>> links_;
    std::shared_ptr<slint::VectorModel<TraceView>> traces_;
    slint::Timer timer_;
    int selected_ = 1;
    int connection_source_ = -1;
    int selected_event_ = -1;
    bool follow_events_ = true;
};

} // namespace resend

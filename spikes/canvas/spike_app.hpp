#pragma once

#include "canvas_controller.hpp"
#include "spike.h"

#include <map>
#include <memory>
#include <vector>

namespace spike {

// Forwards Slint input to the controller and copies back only what changed.
class SpikeApp {
public:
    SpikeApp(slint::ComponentHandle<SpikeWindow> ui, bool per_link_paths);
    SpikeApp(const SpikeApp&) = delete;
    SpikeApp& operator=(const SpikeApp&) = delete;

    CanvasController& canvas() { return canvas_; }
    void small_scene();
    void stress_scene(int node_count, int link_count, int event_count);
    void fit();
    void sync(unsigned changes);

private:
    void bind();
    void rebuild_models();
    void update_node(int id);
    void update_paths();
    NodeRow node_row(const Node& n) const;
    PortRow port_row(PortRef port) const;
    LinkRow link_row(const Link& l) const;

    slint::ComponentHandle<SpikeWindow> ui_;
    CanvasController canvas_;
    bool per_link_paths_;
    std::shared_ptr<slint::VectorModel<NodeRow>> nodes_;
    std::shared_ptr<slint::VectorModel<PortRow>> ports_;
    std::shared_ptr<slint::VectorModel<LinkRow>> links_;
    std::shared_ptr<slint::VectorModel<EventRow>> events_;
    std::map<int, std::size_t> node_row_;   // node id -> row
    std::map<int, std::size_t> port_row_;   // node id -> first port row
    std::vector<PortRef> port_refs_;        // port row -> port
    Hit shown_selection_, shown_hover_;
};

} // namespace spike

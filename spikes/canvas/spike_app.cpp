#include "spike_app.hpp"

#include <cstdint>
#include <string>

namespace spike {
namespace {
std::string text(const slint::SharedString& value) { return std::string(value.data(), value.size()); }

Key to_key(std::string_view text) {
    namespace codes = slint::platform::key_codes;
    const auto is = [&](std::u8string_view code) {
        return text == std::string_view(reinterpret_cast<const char*>(code.data()), code.size());
    };
    if (is(codes::Delete) || is(codes::Backspace)) return Key::Delete;
    if (is(codes::Escape)) return Key::Escape;
    if (is(codes::LeftArrow)) return Key::Left;
    if (is(codes::RightArrow)) return Key::Right;
    if (is(codes::UpArrow)) return Key::Up;
    if (is(codes::DownArrow)) return Key::Down;
    if (text == "+" || text == "=") return Key::ZoomIn;
    if (text == "-") return Key::ZoomOut;
    if (text == "f" || text == "0") return Key::Fit;
    return Key::Other;
}
} // namespace

SpikeApp::SpikeApp(slint::ComponentHandle<SpikeWindow> ui, bool per_link_paths)
    : ui_(std::move(ui)), per_link_paths_(per_link_paths),
      nodes_(std::make_shared<slint::VectorModel<NodeRow>>()),
      ports_(std::make_shared<slint::VectorModel<PortRow>>()),
      links_(std::make_shared<slint::VectorModel<LinkRow>>()),
      events_(std::make_shared<slint::VectorModel<EventRow>>()) {
    ui_->set_nodes(nodes_);
    ui_->set_ports(ports_);
    ui_->set_links(links_);
    ui_->set_events(events_);
    ui_->set_per_link_paths(per_link_paths_);
    bind();
}

void SpikeApp::bind() {
    ui_->on_pointer_down([this](float x, float y, int button) {
        sync(canvas_.press({x, y}, button == 0 ? Button::Left : button == 2 ? Button::Right : Button::Middle));
    });
    ui_->on_pointer_move([this](float x, float y) { sync(canvas_.move({x, y})); });
    ui_->on_pointer_up([this](float x, float y) { sync(canvas_.release({x, y})); });
    ui_->on_wheel([this](float x, float y, float delta) {
        sync(canvas_.zoom_at({x, y}, delta > 0 ? 1.1f : 1 / 1.1f));
    });
    ui_->on_key([this](slint::SharedString value) {
        const Key key = to_key(std::string_view(value));
        if (key == Key::Fit) { fit(); return true; }
        const unsigned changes = canvas_.key(key);
        sync(changes);
        return changes != None;
    });
    ui_->on_rename([this](slint::SharedString name) {
        if (canvas_.selection().kind == HitKind::Node && canvas_.rename(canvas_.selection().id, text(name)))
            update_node(canvas_.selection().id);
    });
    ui_->on_delete_selection([this] { sync(canvas_.key(Key::Delete)); });
    ui_->on_fit_view([this] { fit(); });
}

void SpikeApp::small_scene() {
    const int a = canvas_.add_node(NodeKind::Host, {60, 60});
    const int b = canvas_.add_node(NodeKind::Host, {460, 60});
    const int s = canvas_.add_node(NodeKind::Switch, {260, 260});
    canvas_.connect({a, 0}, {s, 0});
    canvas_.connect({b, 0}, {s, 7});
    canvas_.add_node(NodeKind::Host, {660, 300});  // left free for linking
    rebuild_models();
}

// Deterministic: hosts each join a switch, then switches are cross-linked.
void SpikeApp::stress_scene(int node_count, int link_count, int event_count) {
    std::uint32_t state = 12345;
    const auto next = [&state](int bound) {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(bound));
    };
    const int columns = 15;
    std::vector<int> switches, hosts;
    for (int i = 0; i < node_count; ++i) {
        const auto kind = i % 5 < 2 ? NodeKind::Switch : NodeKind::Host;
        const int id = canvas_.add_node(kind, {static_cast<float>(i % columns) * 220.f, static_cast<float>(i / columns) * 170.f});
        (kind == NodeKind::Switch ? switches : hosts).push_back(id);
    }
    const auto free_port = [&](int node) {
        for (int p = 0; p < port_count(canvas_.node(node)->kind); ++p)
            if (!canvas_.port_used({node, p})) return p;
        return -1;
    };
    for (int host : hosts) {
        if (static_cast<int>(canvas_.links().size()) >= link_count) break;
        for (int attempt = 0; attempt < 20; ++attempt) {
            const int sw = switches[static_cast<std::size_t>(next(static_cast<int>(switches.size())))];
            if (const int p = free_port(sw); p >= 0 && canvas_.connect({host, 0}, {sw, p})) break;
        }
    }
    for (int attempt = 0; attempt < 10000 && static_cast<int>(canvas_.links().size()) < link_count; ++attempt) {
        const int a = switches[static_cast<std::size_t>(next(static_cast<int>(switches.size())))];
        const int b = switches[static_cast<std::size_t>(next(static_cast<int>(switches.size())))];
        const int pa = free_port(a), pb = free_port(b);
        if (pa >= 0 && pb >= 0) canvas_.connect({a, pa}, {b, pb});
    }
    std::vector<EventRow> events;
    events.reserve(static_cast<std::size_t>(event_count));
    for (int i = 0; i < event_count; ++i) {
        const auto time = std::to_string(i * 10) + " ms";
        const auto summary = "Event " + std::to_string(i) + " · frame forwarded";
        EventRow row;
        row.time = time.c_str();
        row.protocol = i % 2 ? "ARP" : "ICMP";
        row.summary = summary.c_str();
        events.push_back(row);
    }
    events_->set_vector(std::move(events));
    rebuild_models();
}

void SpikeApp::fit() {
    sync(canvas_.fit({ui_->get_canvas_width(), ui_->get_canvas_height()}));
}

NodeRow SpikeApp::node_row(const Node& n) const {
    NodeRow row;
    row.id = n.id;
    row.name = slint::SharedString(n.name);
    row.switch_kind = n.kind == NodeKind::Switch;
    row.x = n.pos.x;
    row.y = n.pos.y;
    row.selected = canvas_.selection().kind == HitKind::Node && canvas_.selection().id == n.id;
    return row;
}
PortRow SpikeApp::port_row(PortRef port) const {
    const Vec2 at = canvas_.port_anchor(port);
    const Hit& hover = canvas_.hover();
    PortRow row;
    row.x = at.x;
    row.y = at.y;
    row.used = canvas_.port_used(port);
    row.target = hover.kind == HitKind::Port && hover.id == port.node && hover.port == port.port;
    return row;
}
LinkRow SpikeApp::link_row(const Link& l) const {
    const Vec2 a = canvas_.port_anchor(l.a), b = canvas_.port_anchor(l.b);
    LinkRow row;
    row.ax = a.x;
    row.ay = a.y;
    row.bx = b.x;
    row.by = b.y;
    row.selected = canvas_.selection().kind == HitKind::Link && canvas_.selection().id == l.id;
    return row;
}

void SpikeApp::rebuild_models() {
    std::vector<NodeRow> nodes;
    std::vector<PortRow> ports;
    node_row_.clear();
    port_row_.clear();
    port_refs_.clear();
    for (const auto& n : canvas_.nodes()) {
        node_row_[n.id] = nodes.size();
        nodes.push_back(node_row(n));
        port_row_[n.id] = ports.size();
        for (int p = 0; p < port_count(n.kind); ++p) {
            port_refs_.push_back({n.id, p});
            ports.push_back(port_row({n.id, p}));
        }
    }
    nodes_->set_vector(std::move(nodes));
    ports_->set_vector(std::move(ports));
    if (per_link_paths_) {
        std::vector<LinkRow> links;
        for (const auto& l : canvas_.links()) links.push_back(link_row(l));
        links_->set_vector(std::move(links));
    }
    shown_selection_ = canvas_.selection();
    shown_hover_ = canvas_.hover();
}

void SpikeApp::update_node(int id) {
    const auto* n = canvas_.node(id);
    const auto row = node_row_.find(id);
    if (!n || row == node_row_.end()) return;
    nodes_->set_row_data(row->second, node_row(*n));
    const std::size_t first = port_row_[id];
    for (int p = 0; p < port_count(n->kind); ++p) ports_->set_row_data(first + static_cast<std::size_t>(p), port_row({id, p}));
}

void SpikeApp::update_paths() {
    if (per_link_paths_) {
        const auto& all = canvas_.links();
        for (std::size_t i = 0; i < all.size(); ++i) links_->set_row_data(i, link_row(all[i]));
    } else {
        ui_->set_link_path(slint::SharedString(canvas_.link_path(false)));
    }
    ui_->set_selected_path(per_link_paths_ ? slint::SharedString() : slint::SharedString(canvas_.link_path(true)));
}

void SpikeApp::sync(unsigned changes) {
    if (changes & Topology) rebuild_models();
    if (changes & View) {
        ui_->set_zoom(canvas_.zoom());
        ui_->set_pan_x(canvas_.pan().x);
        ui_->set_pan_y(canvas_.pan().y);
    }
    if (changes & Moved) {
        update_node(canvas_.moved_node());
        if (per_link_paths_) {
            const auto& all = canvas_.links();
            for (std::size_t i = 0; i < all.size(); ++i)
                if (all[i].a.node == canvas_.moved_node() || all[i].b.node == canvas_.moved_node())
                    links_->set_row_data(i, link_row(all[i]));
        }
    }
    if ((changes & Selection) && !(changes & Topology)) {
        for (const Hit& hit : {shown_selection_, canvas_.selection()})
            if (hit.kind == HitKind::Node) update_node(hit.id);
        shown_selection_ = canvas_.selection();
    }
    if (changes & Selection) {
        const bool node = canvas_.selection().kind == HitKind::Node;
        ui_->set_name_editable(node);
        ui_->set_selected_name(node ? slint::SharedString(canvas_.node(canvas_.selection().id)->name) : slint::SharedString());
    }
    if (changes & Overlay) {
        if (!(shown_hover_ == canvas_.hover())) {
            for (const Hit& hit : {shown_hover_, canvas_.hover()})
                if (hit.kind == HitKind::Port) ports_->set_row_data(port_row_[hit.id] + static_cast<std::size_t>(hit.port), port_row({hit.id, hit.port}));
            shown_hover_ = canvas_.hover();
        }
        std::string band;
        if (const auto line = canvas_.rubber_band())
            band = "M" + std::to_string(line->first.x) + " " + std::to_string(line->first.y) +
                   "L" + std::to_string(line->second.x) + " " + std::to_string(line->second.y);
        ui_->set_band_path(slint::SharedString(band));
    }
    // With a single path in screen coordinates, any geometric change redraws it;
    // per-link paths only follow the rows updated above.
    if (changes & (View | Moved | Topology | Selection)) {
        if (!per_link_paths_ || (changes & (Topology | Selection))) update_paths();
    }
    ui_->set_status(slint::SharedString(canvas_.status() + "  ·  " + std::to_string(canvas_.nodes().size()) + " nodes, " +
                                        std::to_string(canvas_.links().size()) + " cables, zoom " +
                                        std::to_string(static_cast<int>(canvas_.zoom() * 100)) + "%"));
}

} // namespace spike

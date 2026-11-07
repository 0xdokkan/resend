#include "app_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace resend {
namespace {
std::string text(const slint::SharedString& value) { return std::string(value.data(), value.size()); }

template<class T>
void replace_rows(const std::shared_ptr<slint::VectorModel<T>>& model, const std::vector<T>& rows) {
    if (model->row_count() != rows.size()) {
        model->set_vector(rows);
        return;
    }
    for (std::size_t index = 0; index < rows.size(); ++index)
        model->set_row_data(index, rows[index]);
}

std::string timestamp(std::uint64_t ms) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << static_cast<double>(ms) / 1000.0 << " s";
    return out.str();
}
} // namespace

AppController::AppController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)),
      nodes_(std::make_shared<slint::VectorModel<NodeView>>()),
      links_(std::make_shared<slint::VectorModel<LinkView>>()),
      traces_(std::make_shared<slint::VectorModel<TraceView>>()) {
    engine_.seed_demo();
    ui_->set_nodes(nodes_);
    ui_->set_links(links_);
    ui_->set_traces(traces_);
    ui_->set_target_ip("192.168.1.20");
    bind_callbacks();
    refresh(true);
    ui_->invoke_fit_view();
    status("Local sandbox. Drag devices, connect ports and watch a ping.");
}

void AppController::status(const std::string& value) { ui_->set_status_text(value.c_str()); }

void AppController::stop() {
    timer_.stop();
    ui_->set_running(false);
}

std::string AppController::table_text() const {
    std::ostringstream out;
    const auto* device = engine_.device(selected_);
    if (!device) return "Select a device to see its state.";
    if (device->kind == netsim::DeviceKind::Host) {
        out << "ARP CACHE\n";
        const auto table = engine_.arp_table(selected_);
        if (table.empty()) out << "No mappings learned yet.\nSend a ping to watch ARP.";
        for (const auto& row : table) out << row.ip.to_string() << "\n" << row.mac.to_string() << "\n";
    } else {
        out << "MAC TABLE\n";
        const auto table = engine_.mac_table(selected_);
        if (table.empty()) out << "No addresses learned yet.\nThe switch learns from incoming frames.";
        for (const auto& row : table) out << row.mac.to_string() << "  /  port " << row.port + 1 << "\n";
    }
    return out.str();
}

// Nodes, cables and content bounds only: cheap enough to run on every drag step.
void AppController::refresh_topology() {
    const auto& history = engine_.events();
    int active_link = -1;
    int active_from = -1;
    if (selected_event_ >= 0 && selected_event_ < static_cast<int>(history.size())) {
        active_link = history[selected_event_].link_id;
        active_from = history[selected_event_].from;
    }
    const auto& geometry = ui_->global<Geometry>();
    const float card_width = geometry.get_card_width();
    const float card_height = geometry.get_card_height();

    std::vector<NodeView> node_rows;
    float left = 0, top = 0, right = 0, bottom = 0;
    for (const auto& device : engine_.devices()) {
        if (node_rows.empty()) {
            left = right = device.x;
            top = bottom = device.y;
        }
        left = std::min(left, device.x);
        top = std::min(top, device.y);
        right = std::max(right, device.x + card_width);
        bottom = std::max(bottom, device.y + card_height);
        NodeView row;
        row.id = device.id;
        row.title = device.name.c_str();
        row.kind = device.kind == netsim::DeviceKind::Host ? "host" : "switch";
        const auto address = device.kind == netsim::DeviceKind::Host
            ? device.ip.to_string() + "/" + std::to_string(device.prefix) : "8 Ethernet ports";
        row.address = address.c_str();
        row.mac = device.mac.to_string().c_str();
        row.x = device.x;
        row.y = device.y;
        row.enabled = device.enabled;
        row.selected = device.id == selected_;
        node_rows.push_back(row);
    }
    replace_rows(nodes_, node_rows);
    ui_->set_bounds_left(left);
    ui_->set_bounds_top(top);
    ui_->set_bounds_right(right);
    ui_->set_bounds_bottom(bottom);

    std::vector<LinkView> link_rows;
    for (const auto& link : engine_.links()) {
        const auto* a = engine_.device(link.a);
        const auto* b = engine_.device(link.b);
        if (!a || !b) continue;
        LinkView row;
        row.id = link.id;
        row.ax = a->x + card_width / 2;
        row.ay = a->y + card_height / 2;
        row.bx = b->x + card_width / 2;
        row.by = b->y + card_height / 2;
        row.enabled = link.enabled;
        row.active = link.id == active_link;
        if (row.active && active_from == link.b) {
            std::swap(row.ax, row.bx);
            std::swap(row.ay, row.by);
        }
        const auto label = a->name + " :" + std::to_string(link.a_port + 1)
            + " — " + b->name + " :" + std::to_string(link.b_port + 1);
        row.label = label.c_str();
        link_rows.push_back(row);
    }
    replace_rows(links_, link_rows);
}

void AppController::refresh(bool edit_fields) {
    const auto& history = engine_.events();
    if (history.empty()) selected_event_ = -1;
    else if (follow_events_ || selected_event_ >= static_cast<int>(history.size()))
        selected_event_ = static_cast<int>(history.size()) - 1;
    if (selected_event_ >= 0) {
        const auto& event = history[selected_event_];
        ui_->set_event_title(event.summary.c_str());
        ui_->set_event_detail(event.detail.c_str());
        ui_->set_event_protocol(event.protocol.c_str());
    } else {
        ui_->set_event_title("The network is ready for your first experiment.");
        ui_->set_event_detail("Select a host, choose an IPv4 address and press Send. Use Step to watch every decision; Run advances automatically. The tables show the state of the engine.");
        ui_->set_event_protocol("WAITING");
    }
    refresh_topology();

    // Events are append-only until the simulation is reset.
    if (traces_->row_count() > history.size()) traces_->set_vector({});
    for (std::size_t index = traces_->row_count(); index < history.size(); ++index) {
        const auto& event = history[index];
        TraceView row;
        row.index = static_cast<int>(index);
        row.time = timestamp(event.time_ms).c_str();
        row.protocol = event.protocol.c_str();
        row.summary = event.summary.c_str();
        row.detail = event.detail.c_str();
        row.from_id = event.from;
        row.to_id = event.to;
        row.link_id = event.link_id;
        traces_->push_back(row);
    }
    ui_->set_event_count(static_cast<int>(history.size()));
    ui_->set_selected_event(selected_event_);
    ui_->set_follow_events(follow_events_);
    if (follow_events_) ui_->invoke_reveal_event(selected_event_);

    const bool pending = engine_.pending();
    ui_->set_has_pending(pending);
    ui_->set_selected_id(selected_);
    ui_->set_link_source_id(connection_source_);
    const auto* device = engine_.device(selected_);
    ui_->set_selected_kind(device ? (device->kind == netsim::DeviceKind::Host ? "host" : "switch") : "");
    ui_->set_selected_mac(device ? device->mac.to_string().c_str() : "");
    ui_->set_selected_details(table_text().c_str());
    if (edit_fields) {
        ui_->set_edit_name(device ? device->name.c_str() : "");
        ui_->set_edit_ip(device ? device->ip.to_string().c_str() : "");
        ui_->set_edit_prefix(device ? std::to_string(device->prefix).c_str() : "24");
    }
    if (!pending) stop();
}

void AppController::mutation_result(bool success, const std::string& message) {
    if (success) {
        stop();
        selected_event_ = -1;
        follow_events_ = true;
        status(message + " Simulation state cleared.");
        refresh(true);
    } else {
        status(engine_.last_error());
    }
}

void AppController::advance() {
    if (!engine_.pending()) return;
    engine_.step();
    refresh();
    if (!engine_.pending())
        status(engine_.ping_succeeded() ? "Ping succeeded. Explore the events and the learned tables."
                                        : "Experiment finished: read the last event to understand the result.");
}

void AppController::bind_callbacks() {
    ui_->on_select_node([this](int id) {
        if (id == -1 || engine_.device(id)) { selected_ = id; refresh(true); }
    });
    ui_->on_move_node([this](int id, float x, float y) {
        if (std::isfinite(x) && std::isfinite(y) && engine_.move_device(id, x, y)) refresh_topology();
    });
    ui_->on_add_node([this](slint::SharedString kind, float x, float y) {
        // Cards added in a row fan out instead of stacking exactly.
        const auto offset = static_cast<float>(engine_.devices().size() % 5) * 22.0f;
        selected_ = engine_.add_device(text(kind) == "switch" ? netsim::DeviceKind::Switch : netsim::DeviceKind::Host,
                                       x + offset, y + offset);
        mutation_result(selected_ >= 0, "Device added. Use the connector on its card to connect it.");
    });
    ui_->on_link_node([this](int id) {
        if (!engine_.device(id)) return;
        if (connection_source_ < 0) {
            connection_source_ = id;
            status("Select the connector of another device. Click the first one again to cancel.");
        } else if (connection_source_ == id) {
            connection_source_ = -1;
            status("Connection cancelled.");
        } else {
            const auto from = connection_source_;
            connection_source_ = -1;
            mutation_result(engine_.connect(from, id), "Link created on the first free ports.");
        }
        ui_->set_link_source_id(connection_source_);
    });
    ui_->on_delete_selected([this] {
        if (selected_ < 0) return;
        const auto old = selected_;
        if (engine_.remove_device(old)) {
            selected_ = engine_.devices().empty() ? -1 : engine_.devices().front().id;
            if (connection_source_ == old) connection_source_ = -1;
            mutation_result(true, "Device and its links removed.");
        } else status(engine_.last_error());
    });
    ui_->on_apply_node([this](slint::SharedString name, slint::SharedString ip, slint::SharedString prefix) {
        const auto* device = engine_.device(selected_);
        if (!device) return;
        if (device->kind == netsim::DeviceKind::Switch) {
            if (engine_.rename_device(selected_, text(name))) {
                status("Name updated.");
                refresh(true);
            } else status(engine_.last_error());
            return;
        }
        const auto prefix_text = text(prefix);
        try {
            std::size_t consumed = 0;
            const int bits = std::stoi(prefix_text, &consumed);
            if (consumed != prefix_text.size()) throw std::invalid_argument("prefix");
            mutation_result(engine_.configure_host(selected_, text(name), text(ip), bits), "Configuration applied.");
        } catch (const std::exception&) {
            status("The prefix must be a whole number, for example 24.");
        }
    });
    ui_->on_ping([this] {
        stop();
        if (engine_.start_ping(selected_, text(ui_->get_target_ip()))) {
            follow_events_ = true;
            selected_event_ = -1;
            status("Ping ready. Press Step or Run to advance simulated time.");
            refresh();
        } else status(engine_.last_error());
    });
    ui_->on_step([this] { stop(); advance(); });
    ui_->on_run_toggle([this] {
        if (timer_.running()) { stop(); return; }
        if (!engine_.pending()) { status("Prepare a ping to start an experiment."); return; }
        ui_->set_running(true);
        timer_.start(slint::TimerMode::Repeated, 420ms, [this] { advance(); });
    });
    ui_->on_reset([this] {
        engine_.reset_simulation();
        mutation_result(true, "Caches, tables and events cleared. The topology stays as it is.");
    });
    ui_->on_load_demo([this] {
        engine_.seed_demo();
        selected_ = 1;
        connection_source_ = -1;
        ui_->set_target_ip("192.168.1.20");
        mutation_result(true, "Sample LAN loaded.");
    });
    ui_->on_clear_workspace([this] {
        engine_.clear();
        selected_ = -1;
        connection_source_ = -1;
        mutation_result(true, "Empty sandbox. Add hosts and switches from the panel on the left.");
    });
    ui_->on_toggle_link([this](int id) {
        if (const auto* link = engine_.link(id)) {
            const bool enable = !link->enabled;
            mutation_result(engine_.set_link_enabled(id, enable), enable ? "Link enabled." : "Link disabled.");
        }
    });
    ui_->on_remove_link([this](int id) { mutation_result(engine_.remove_link(id), "Link removed."); });
    ui_->on_select_event([this](int index) {
        if (index >= 0 && index < static_cast<int>(engine_.events().size())) {
            selected_event_ = index;
            follow_events_ = false;
            refresh();
        }
    });
}

} // namespace resend

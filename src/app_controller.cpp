#include "app_controller.hpp"

#include "app/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace resend {
namespace {
constexpr double speeds[] = {0.25, 0.5, 1, 2, 4};
// Longest real time a single frame may advance: after a stall the animation
// resumes where it was instead of jumping ahead.
constexpr auto max_frame_step = 100ms;
// Space above the cards for the decision chips, in canvas units.
constexpr float marker_room = 40;

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

std::string speed_text(double speed) {
    std::ostringstream out;
    out << speed << "×";
    return out.str();
}

const char* tone_name(Tone tone) {
    switch (tone) {
    case Tone::Learn: return "learn";
    case Tone::Forward: return "forward";
    case Tone::Muted: return "muted";
    case Tone::Wait: return "wait";
    case Tone::Drop: return "drop";
    case Tone::Success: return "success";
    }
    return "muted";
}
} // namespace

AppController::AppController(slint::ComponentHandle<AppWindow> ui)
    : ui_(std::move(ui)),
      nodes_(std::make_shared<slint::VectorModel<NodeView>>()),
      links_(std::make_shared<slint::VectorModel<LinkView>>()),
      packets_(std::make_shared<slint::VectorModel<PacketView>>()),
      markers_(std::make_shared<slint::VectorModel<MarkerView>>()),
      traces_(std::make_shared<slint::VectorModel<TraceView>>()) {
    const auto& geometry = ui_->global<Geometry>();
    geometry.set_card_width(card_width);
    geometry.set_card_height(card_height);
    engine_.seed_demo();
    playback_.reset();
    ui_->set_nodes(nodes_);
    ui_->set_links(links_);
    ui_->set_packets(packets_);
    ui_->set_markers(markers_);
    ui_->set_traces(traces_);
    ui_->set_target_ip("192.168.1.20");
    ui_->set_speed_label(speed_text(playback_.speed()).c_str());
    ui_->set_version(RESEND_VERSION);
    bind_callbacks();
    refresh(true);
    ui_->invoke_fit_view();
    status("Local sandbox. Drag devices, connect ports and watch a ping.");
}

void AppController::status(const std::string& value) { ui_->set_status_text(value.c_str()); }

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
    // Cables the selected frame has crossed so far, flooded copies included.
    std::set<std::uint32_t> path;
    if (selected_pdu_ >= 0)
        for (const auto& sent : engine_.trace().transmissions())
            if (sent.pdu.value == static_cast<std::uint32_t>(selected_pdu_)) path.insert(sent.link.value);
    std::map<int, unsigned> connected, down;
    for (const auto& link : engine_.links()) {
        connected[link.a] |= 1U << link.a_port;
        connected[link.b] |= 1U << link.b_port;
        if (!link.enabled) {
            down[link.a] |= 1U << link.a_port;
            down[link.b] |= 1U << link.b_port;
        }
    }
    const auto scene = build_scene(engine_, playback_);
    const std::map<int, unsigned> activity(scene.port_activity.begin(), scene.port_activity.end());

    std::vector<NodeView> node_rows;
    shown_activity_.clear();
    float left = 0, top = 0, right = 0, bottom = 0;
    for (const auto& device : engine_.devices()) {
        if (node_rows.empty()) {
            left = right = device.x;
            top = bottom = device.y;
        }
        left = std::min(left, device.x);
        top = std::min(top, device.y - marker_room);
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
        row.port_count = port_count(device);
        row.connected_ports = static_cast<int>(connected[device.id]);
        row.down_ports = static_cast<int>(down[device.id]);
        const auto active = activity.find(device.id);
        shown_activity_.push_back(active == activity.end() ? 0U : active->second);
        row.active_ports = static_cast<int>(shown_activity_.back());
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
        const auto cable = cable_between(card_box(*a), card_box(*b));
        LinkView row;
        row.id = link.id;
        row.ax = cable.a.x;
        row.ay = cable.a.y;
        row.bx = cable.b.x;
        row.by = cable.b.y;
        row.enabled = link.enabled;
        row.highlight = path.contains(static_cast<std::uint32_t>(link.id));
        row.a_port = a->kind == netsim::DeviceKind::Switch ? std::to_string(link.a_port + 1).c_str() : "";
        row.b_port = b->kind == netsim::DeviceKind::Switch ? std::to_string(link.b_port + 1).c_str() : "";
        const auto label = a->name + " :" + std::to_string(link.a_port + 1)
            + " — " + b->name + " :" + std::to_string(link.b_port + 1);
        row.label = label.c_str();
        link_rows.push_back(row);
    }
    replace_rows(links_, link_rows);
}

// Packets, decision chips, port lights and the clock: runs on every frame.
void AppController::refresh_scene() {
    const auto scene = build_scene(engine_, playback_);
    std::vector<PacketView> packet_rows;
    for (const auto& packet : scene.packets) {
        PacketView row;
        row.pdu = packet.pdu;
        row.x = packet.at.x;
        row.y = packet.at.y;
        row.tail_x = packet.tail.x;
        row.tail_y = packet.tail.y;
        row.protocol = netsim::is_arp(packet.kind) ? "ARP" : "ICMP";
        row.label = packet.label.c_str();
        row.broadcast = packet.broadcast;
        row.selected = packet.pdu == selected_pdu_;
        row.description = packet.description.c_str();
        packet_rows.push_back(row);
    }
    replace_rows(packets_, packet_rows);

    std::vector<MarkerView> marker_rows;
    for (const auto& marker : scene.markers) {
        MarkerView row;
        row.x = marker.anchor.x;
        row.y = marker.anchor.y;
        row.stack = marker.stack;
        row.text = marker.text.c_str();
        row.tone = tone_name(marker.tone);
        row.opacity = marker.opacity;
        marker_rows.push_back(row);
    }
    replace_rows(markers_, marker_rows);

    const std::map<int, unsigned> activity(scene.port_activity.begin(), scene.port_activity.end());
    const auto& devices = engine_.devices();
    if (shown_activity_.size() == devices.size() && nodes_->row_count() == devices.size()) {
        for (std::size_t row = 0; row < devices.size(); ++row) {
            const auto found = activity.find(devices[row].id);
            const unsigned mask = found == activity.end() ? 0U : found->second;
            if (mask == shown_activity_[row]) continue;
            auto data = *nodes_->row_data(row);
            data.active_ports = static_cast<int>(mask);
            nodes_->set_row_data(row, data);
            shown_activity_[row] = mask;
        }
    }
    ui_->set_sim_clock(netsim::format_ms(playback_.now()).c_str());
}

void AppController::refresh(bool edit_fields) {
    const auto& history = engine_.events();
    if (history.empty()) selected_event_ = -1;
    else if (follow_events_ || selected_event_ >= static_cast<int>(history.size()))
        selected_event_ = static_cast<int>(history.size()) - 1;
    if (follow_events_) selected_pdu_ = selected_event_ >= 0 ? history[selected_event_].pdu : -1;
    if (selected_pdu_ >= static_cast<int>(engine_.trace().pdus().size())) selected_pdu_ = -1;
    if (selected_event_ >= 0) {
        const auto& event = history[selected_event_];
        ui_->set_event_title(event.summary.c_str());
        ui_->set_event_detail(event.detail.c_str());
        ui_->set_event_protocol(event.protocol.c_str());
    } else {
        ui_->set_event_title("The network is ready for your first experiment.");
        ui_->set_event_detail("Select a host, choose an IPv4 address and press Send. Step plays one event at a time; Run plays the whole experiment. The tables show the state of the engine at the instant on the canvas.");
        ui_->set_event_protocol("WAITING");
    }
    ui_->set_frame_layers(selected_pdu_ >= 0
        ? describe_layers(engine_.trace().pdu(netsim::PduId{static_cast<std::uint32_t>(selected_pdu_)})).c_str() : "");
    refresh_topology();
    refresh_scene();

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

    ui_->set_has_pending(engine_.pending());
    ui_->set_selected_id(selected_);
    ui_->set_link_source_id(connection_source_);
    const auto* device = engine_.device(selected_);
    ui_->set_selected_kind(device ? (device->kind == netsim::DeviceKind::Host ? "host" : "switch") : "");
    ui_->set_selected_mac(device ? device->mac.to_string().c_str() : "");
    ui_->set_selected_enabled(device ? device->enabled : true);
    ui_->set_selected_details(table_text().c_str());
    if (edit_fields) {
        ui_->set_edit_name(device ? device->name.c_str() : "");
        ui_->set_edit_ip(device ? device->ip.to_string().c_str() : "");
        ui_->set_edit_prefix(device ? std::to_string(device->prefix).c_str() : "24");
    }
}

void AppController::mutation_result(bool success, const std::string& message) {
    if (success) {
        playback_.reset();
        sync_playback();
        selected_event_ = -1;
        selected_pdu_ = -1;
        follow_events_ = true;
        status(message + " Simulation state cleared.");
        refresh(true);
    } else {
        status(engine_.last_error());
    }
}

void AppController::sync_playback() {
    const bool playing = playback_.playing();
    ui_->set_running(playing);
    if (playing && !frame_timer_.running()) {
        last_frame_ = std::chrono::steady_clock::now();
        frame_timer_.start(slint::TimerMode::Repeated, 16ms, [this] {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::min<std::chrono::nanoseconds>(now - last_frame_, max_frame_step);
            last_frame_ = now;
            advance_playback(elapsed);
        });
    } else if (!playing && frame_timer_.running()) {
        frame_timer_.stop();
    }
}

template<class Command>
void AppController::playback_command(Command command) {
    const bool was_pending = engine_.pending();
    const auto events = engine_.events().size();
    const auto records = engine_.trace().records().size();
    command();
    if (engine_.events().size() != events || engine_.trace().records().size() != records) refresh();
    else refresh_scene();
    sync_playback();
    if (was_pending && !engine_.pending())
        status(engine_.ping_succeeded() ? "Ping succeeded. Explore the events and the learned tables."
                                        : "Experiment finished: read the last event to understand the result.");
}

void AppController::advance_playback(std::chrono::nanoseconds elapsed) {
    playback_command([&] { playback_.tick(elapsed); });
}

void AppController::seek(netsim::SimTime time) {
    playback_command([&] { playback_.seek(time); });
}

void AppController::select_frame(int pdu) {
    if (pdu < 0 || pdu >= static_cast<int>(engine_.trace().pdus().size())) return;
    selected_pdu_ = pdu;
    follow_events_ = false;
    const auto& history = engine_.events();
    for (auto index = history.size(); index-- > 0;) {
        if (history[index].pdu == pdu) {
            selected_event_ = static_cast<int>(index);
            break;
        }
    }
    refresh();
}

void AppController::bind_callbacks() {
    ui_->on_select_node([this](int id) {
        if (id == -1 || engine_.device(id)) { selected_ = id; refresh(true); }
    });
    ui_->on_move_node([this](int id, float x, float y) {
        if (std::isfinite(x) && std::isfinite(y) && engine_.move_device(id, x, y)) {
            refresh_topology();
            refresh_scene();
        }
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
    ui_->on_toggle_power([this] {
        const auto* device = engine_.device(selected_);
        if (!device) return;
        const bool power = !device->enabled;
        mutation_result(engine_.set_device_enabled(selected_, power), power ? "Device powered on." : "Device powered off.");
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
        playback_.pause();
        if (engine_.start_ping(selected_, text(ui_->get_target_ip()))) {
            follow_events_ = true;
            selected_event_ = -1;
            status("Ping ready. Press Step or Run to advance simulated time.");
            refresh();
        } else status(engine_.last_error());
        sync_playback();
    });
    ui_->on_step([this] { playback_command([this] { playback_.step(); }); });
    ui_->on_run_toggle([this] {
        if (playback_.playing()) {
            playback_command([this] { playback_.pause(); });
            return;
        }
        if (!engine_.pending()) { status("Prepare a ping to start an experiment."); return; }
        playback_command([this] { playback_.play(); });
    });
    ui_->on_cycle_speed([this] {
        const auto next = std::find_if(std::begin(speeds), std::end(speeds), [&](double s) { return s > playback_.speed(); });
        playback_.set_speed(next == std::end(speeds) ? speeds[0] : *next);
        ui_->set_speed_label(speed_text(playback_.speed()).c_str());
    });
    ui_->on_toggle_motion([this] {
        playback_.set_reduced_motion(!playback_.reduced_motion());
        ui_->set_reduced_motion(playback_.reduced_motion());
        status(playback_.reduced_motion() ? "Reduced motion: events appear one instant at a time, without moving packets."
                                          : "Packets move along the cables again.");
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
            selected_pdu_ = engine_.events()[index].pdu;
            follow_events_ = false;
            refresh();
        }
    });
    ui_->on_select_frame([this](int pdu) { select_frame(pdu); });
}

} // namespace resend

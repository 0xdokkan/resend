#include "app_controller.hpp"

#include "app/scene.hpp"
#include "scenario/edit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

template<class T>
int id_or_none(std::optional<T> id) { return id ? static_cast<int>(id->value) : -1; }

netsim::DeviceId device_id(int id) { return netsim::DeviceId{static_cast<std::uint32_t>(std::max(id, 0))}; }
netsim::LinkId link_id(int id) { return netsim::LinkId{static_cast<std::uint32_t>(std::max(id, 0))}; }
int canvas(float value) { return static_cast<int>(std::lround(value)); }

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

void AppController::open(netsim::Scenario scenario) {
    scenario_ = std::move(scenario);
    const auto first_host = std::find_if(scenario_.devices.begin(), scenario_.devices.end(),
                                         [](const auto& device) { return device.kind == netsim::DeviceKind::Host; });
    selected_ = first_host != scenario_.devices.end() ? static_cast<int>(first_host->id.value)
        : scenario_.devices.empty() ? -1 : static_cast<int>(scenario_.devices.front().id.value);
    connection_source_ = -1;
    edited(netsim::Problem::None, scenario_.title.empty() ? "Network loaded." : scenario_.title + " loaded.");
    ui_->invoke_fit_view();
}

void AppController::rebuild() {
    simulation_ = netsim::Simulation(scenario_);
    explainer_.clear();
    playback_.reset();
}

std::string AppController::table_text() const {
    std::ostringstream out;
    const auto* device = scenario_.device(device_id(selected_));
    if (!device) return "Select a device to see its state.";
    if (device->kind == netsim::DeviceKind::Host) {
        out << "ARP CACHE\n";
        const auto table = simulation_.arp_cache(device->id);
        if (table.empty()) out << "No mappings learned yet.\nSend a ping to watch ARP.";
        for (const auto& row : table) out << row.ip.to_string() << "\n" << row.mac.to_string() << "\n";
    } else {
        out << "MAC TABLE\n";
        const auto table = simulation_.mac_table(device->id);
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
        for (const auto& sent : simulation_.trace().transmissions())
            if (sent.pdu.value == static_cast<std::uint32_t>(selected_pdu_)) path.insert(sent.link.value);
    // A port is down when its cable is, or when it or the far end is disabled.
    std::map<netsim::DeviceId, unsigned> connected, down;
    const auto enabled = [&](netsim::PortRef port) {
        const auto* device = scenario_.device(port.device);
        return device && device->ports.at(port.port).admin_up;
    };
    for (const auto& link : scenario_.links) {
        const bool up = link.up && enabled(link.a) && enabled(link.b);
        for (const auto end : {link.a, link.b}) {
            connected[end.device] |= 1U << end.port;
            if (!up) down[end.device] |= 1U << end.port;
        }
    }
    const auto scene = build_scene(scenario_, simulation_, playback_);
    const std::map<int, unsigned> activity(scene.port_activity.begin(), scene.port_activity.end());

    std::vector<NodeView> node_rows;
    shown_activity_.clear();
    float left = 0, top = 0, right = 0, bottom = 0;
    for (const auto& device : scenario_.devices) {
        const auto card = card_box(device);
        if (node_rows.empty()) {
            left = right = card.x;
            top = bottom = card.y;
        }
        left = std::min(left, card.x);
        top = std::min(top, card.y - marker_room);
        right = std::max(right, card.x + card_width);
        bottom = std::max(bottom, card.y + card_height);
        const int id = static_cast<int>(device.id.value);
        NodeView row;
        row.id = id;
        row.title = device.name.c_str();
        row.kind = device.kind == netsim::DeviceKind::Host ? "host" : "switch";
        const auto address = device.kind == netsim::DeviceKind::Host ? device.ports.front().ipv4->to_string()
                                                                     : std::to_string(device.ports.size()) + " Ethernet ports";
        row.address = address.c_str();
        row.mac = device.ports.front().mac.to_string().c_str();
        row.x = card.x;
        row.y = card.y;
        row.enabled = device.powered;
        row.selected = id == selected_;
        row.port_count = static_cast<int>(device.ports.size());
        row.connected_ports = static_cast<int>(connected[device.id]);
        row.down_ports = static_cast<int>(down[device.id]);
        const auto active = activity.find(id);
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
    for (const auto& link : scenario_.links) {
        const auto* a = scenario_.device(link.a.device);
        const auto* b = scenario_.device(link.b.device);
        if (!a || !b) continue;
        const auto cable = cable_between(card_box(*a), card_box(*b));
        LinkView row;
        row.id = static_cast<int>(link.id.value);
        row.ax = cable.a.x;
        row.ay = cable.a.y;
        row.bx = cable.b.x;
        row.by = cable.b.y;
        row.enabled = link.up;
        row.highlight = path.contains(link.id.value);
        row.a_port = a->kind == netsim::DeviceKind::Switch ? std::to_string(link.a.port + 1).c_str() : "";
        row.b_port = b->kind == netsim::DeviceKind::Switch ? std::to_string(link.b.port + 1).c_str() : "";
        const auto label = a->name + " :" + std::to_string(link.a.port + 1)
            + " — " + b->name + " :" + std::to_string(link.b.port + 1);
        row.label = label.c_str();
        link_rows.push_back(row);
    }
    replace_rows(links_, link_rows);
}

// Packets, decision chips, port lights and the clock: runs on every frame.
void AppController::refresh_scene() {
    const auto scene = build_scene(scenario_, simulation_, playback_);
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
    const auto& devices = scenario_.devices;
    if (shown_activity_.size() == devices.size() && nodes_->row_count() == devices.size()) {
        for (std::size_t row = 0; row < devices.size(); ++row) {
            const auto found = activity.find(static_cast<int>(devices[row].id.value));
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
    explainer_.update(simulation_);
    const auto& history = explainer_.events();
    if (history.empty()) selected_event_ = -1;
    else if (follow_events_ || selected_event_ >= static_cast<int>(history.size()))
        selected_event_ = static_cast<int>(history.size()) - 1;
    if (follow_events_) selected_pdu_ = selected_event_ >= 0 ? id_or_none(history[selected_event_].pdu) : -1;
    if (selected_pdu_ >= static_cast<int>(simulation_.trace().pdus().size())) selected_pdu_ = -1;
    if (selected_event_ >= 0) {
        const auto& event = history[selected_event_];
        ui_->set_event_title(event.summary.c_str());
        ui_->set_event_detail(event.detail.c_str());
        ui_->set_event_protocol(event.protocol.c_str());
    } else {
        ui_->set_event_title("The network is ready for your first experiment.");
        ui_->set_event_detail("Select a host, choose an IPv4 address and press Send. Step plays one event at a time; Run plays the whole experiment. The tables show the state of the network at the instant on the canvas. Several pings can run at once.");
        ui_->set_event_protocol("WAITING");
    }
    ui_->set_frame_layers(selected_pdu_ >= 0
        ? describe_layers(simulation_.trace().pdu(netsim::PduId{static_cast<std::uint32_t>(selected_pdu_)})).c_str() : "");
    refresh_topology();
    refresh_scene();

    // Events are append-only until the simulation is reset.
    if (traces_->row_count() > history.size()) traces_->set_vector({});
    for (std::size_t index = traces_->row_count(); index < history.size(); ++index) {
        const auto& event = history[index];
        TraceView row;
        row.index = static_cast<int>(index);
        row.time = netsim::format_ms(event.time).c_str();
        row.protocol = event.protocol.c_str();
        row.summary = event.summary.c_str();
        row.detail = event.detail.c_str();
        row.from_id = id_or_none(event.from);
        row.to_id = id_or_none(event.to);
        row.link_id = id_or_none(event.link);
        traces_->push_back(row);
    }
    ui_->set_event_count(static_cast<int>(history.size()));
    ui_->set_selected_event(selected_event_);
    ui_->set_follow_events(follow_events_);
    if (follow_events_) ui_->invoke_reveal_event(selected_event_);

    ui_->set_has_pending(simulation_.pending());
    ui_->set_selected_id(selected_);
    ui_->set_link_source_id(connection_source_);
    const auto* device = scenario_.device(device_id(selected_));
    const auto* address = device && device->kind == netsim::DeviceKind::Host ? &*device->ports.front().ipv4 : nullptr;
    ui_->set_selected_kind(device ? (device->kind == netsim::DeviceKind::Host ? "host" : "switch") : "");
    ui_->set_selected_mac(device ? device->ports.front().mac.to_string().c_str() : "");
    ui_->set_selected_enabled(device ? device->powered : true);
    ui_->set_selected_details(table_text().c_str());
    if (edit_fields) {
        ui_->set_edit_name(device ? device->name.c_str() : "");
        ui_->set_edit_ip(address ? address->address().to_string().c_str() : "");
        ui_->set_edit_prefix(address ? std::to_string(address->length()).c_str() : "24");
    }
}

void AppController::edited(netsim::Problem problem, const std::string& message) {
    if (!netsim::ok(problem)) {
        status(netsim::explain::describe(problem));
        return;
    }
    rebuild();
    sync_playback();
    selected_event_ = -1;
    selected_pdu_ = -1;
    follow_events_ = true;
    status(message + " Simulation state cleared.");
    refresh(true);
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
    const bool was_pending = simulation_.pending();
    const auto records = simulation_.trace().records().size();
    command();
    if (simulation_.trace().records().size() != records) refresh();
    else refresh_scene();
    sync_playback();
    if (was_pending && !simulation_.pending()) {
        const auto& pings = simulation_.pings();
        const bool all = std::all_of(pings.begin(), pings.end(), [](const auto& ping) { return ping.state == netsim::PingState::Succeeded; });
        status(all ? (pings.size() > 1 ? "Every ping succeeded. Explore the events and the learned tables."
                                       : "Ping succeeded. Explore the events and the learned tables.")
                   : "Experiment finished: read the last event to understand the result.");
    }
}

void AppController::advance_playback(std::chrono::nanoseconds elapsed) {
    playback_command([&] { playback_.tick(elapsed); });
}

void AppController::seek(netsim::SimTime time) {
    playback_command([&] { playback_.seek(time); });
}

void AppController::select_frame(int pdu) {
    if (pdu < 0 || pdu >= static_cast<int>(simulation_.trace().pdus().size())) return;
    selected_pdu_ = pdu;
    follow_events_ = false;
    const auto& history = explainer_.events();
    for (auto index = history.size(); index-- > 0;) {
        if (id_or_none(history[index].pdu) == pdu) {
            selected_event_ = static_cast<int>(index);
            break;
        }
    }
    refresh();
}

void AppController::bind_callbacks() {
    namespace edit = netsim::edit;
    ui_->on_select_node([this](int id) {
        if (id == -1 || scenario_.device(device_id(id))) { selected_ = id; refresh(true); }
    });
    ui_->on_move_node([this](int id, float x, float y) {
        // Positions are layout only: the simulation carries on.
        if (std::isfinite(x) && std::isfinite(y) && netsim::ok(edit::move(scenario_, device_id(id), canvas(x), canvas(y)))) {
            refresh_topology();
            refresh_scene();
        }
    });
    ui_->on_add_node([this](slint::SharedString kind, float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        // Cards added in a row fan out instead of stacking exactly.
        const auto offset = static_cast<float>(scenario_.devices.size() % 5) * 22.0f;
        netsim::DeviceId created{};
        const auto problem = edit::add_device(scenario_, text(kind) == "switch" ? netsim::DeviceKind::Switch : netsim::DeviceKind::Host,
                                              canvas(x + offset), canvas(y + offset), &created);
        if (netsim::ok(problem)) selected_ = static_cast<int>(created.value);
        edited(problem, "Device added. Use the connector on its card to connect it.");
    });
    ui_->on_link_node([this](int id) {
        if (!scenario_.device(device_id(id))) return;
        if (connection_source_ < 0) {
            connection_source_ = id;
            status("Select the connector of another device. Click the first one again to cancel.");
        } else if (connection_source_ == id) {
            connection_source_ = -1;
            status("Connection cancelled.");
        } else {
            const auto from = connection_source_;
            connection_source_ = -1;
            edited(edit::connect(scenario_, device_id(from), device_id(id)), "Link created on the first free ports.");
        }
        ui_->set_link_source_id(connection_source_);
    });
    ui_->on_delete_selected([this] {
        if (selected_ < 0) return;
        const auto old = selected_;
        const auto problem = edit::remove_device(scenario_, device_id(old));
        if (netsim::ok(problem)) {
            selected_ = scenario_.devices.empty() ? -1 : static_cast<int>(scenario_.devices.front().id.value);
            if (connection_source_ == old) connection_source_ = -1;
        }
        edited(problem, "Device and its links removed.");
    });
    ui_->on_toggle_power([this] {
        const auto* device = scenario_.device(device_id(selected_));
        if (!device) return;
        const bool power = !device->powered;
        edited(edit::set_powered(scenario_, device->id, power), power ? "Device powered on." : "Device powered off.");
    });
    ui_->on_apply_node([this](slint::SharedString name, slint::SharedString ip, slint::SharedString prefix) {
        const auto* device = scenario_.device(device_id(selected_));
        if (!device) return;
        if (device->kind == netsim::DeviceKind::Switch) {
            edited(edit::rename(scenario_, device->id, text(name)), "Name updated.");
            return;
        }
        const auto prefix_text = text(prefix);
        try {
            std::size_t consumed = 0;
            const int bits = std::stoi(prefix_text, &consumed);
            if (consumed != prefix_text.size()) throw std::invalid_argument("prefix");
            edited(edit::configure_host(scenario_, device->id, text(name), text(ip), bits), "Configuration applied.");
        } catch (const std::exception&) {
            status("The prefix must be a whole number, for example 24.");
        }
    });
    ui_->on_ping([this] {
        // Pings may overlap: a new one starts at the instant on screen.
        const auto destination = netsim::Ipv4Address::parse(text(ui_->get_target_ip()));
        auto problem = !scenario_.device(device_id(selected_)) ? netsim::Problem::SourceNotHost
            : destination ? netsim::Problem::None : netsim::Problem::InvalidDestination;
        if (netsim::ok(problem)) simulation_.start_ping(device_id(selected_), *destination, problem);
        if (!netsim::ok(problem)) {
            status(netsim::explain::describe(problem));
            return;
        }
        follow_events_ = true;
        selected_event_ = -1;
        status(playback_.playing() ? "Ping started." : "Ping ready. Press Step or Run to advance simulated time.");
        refresh();
        sync_playback();
    });
    ui_->on_step([this] { playback_command([this] { playback_.step(); }); });
    ui_->on_run_toggle([this] {
        if (playback_.playing()) {
            playback_command([this] { playback_.pause(); });
            return;
        }
        if (!simulation_.pending()) { status("Prepare a ping to start an experiment."); return; }
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
    ui_->on_reset([this] { edited(netsim::Problem::None, "Caches, tables and events cleared. The topology stays as it is."); });
    ui_->on_load_demo([this] {
        ui_->set_target_ip("192.168.1.20");
        open(netsim::sample_lan());
    });
    ui_->on_clear_workspace([this] {
        scenario_ = {};
        selected_ = -1;
        connection_source_ = -1;
        edited(netsim::Problem::None, "Empty sandbox. Add hosts and switches from the panel on the left.");
    });
    ui_->on_toggle_link([this](int id) {
        if (const auto* link = scenario_.link(link_id(id))) {
            const bool up = !link->up;
            edited(edit::set_link_up(scenario_, link->id, up), up ? "Link enabled." : "Link disabled.");
        }
    });
    ui_->on_remove_link([this](int id) { edited(edit::remove_link(scenario_, link_id(id)), "Link removed."); });
    ui_->on_select_event([this](int index) {
        const auto& history = explainer_.events();
        if (index >= 0 && index < static_cast<int>(history.size())) {
            selected_event_ = index;
            selected_pdu_ = id_or_none(history[static_cast<std::size_t>(index)].pdu);
            follow_events_ = false;
            refresh();
        }
    });
    ui_->on_select_frame([this](int pdu) { select_frame(pdu); });
}

} // namespace resend

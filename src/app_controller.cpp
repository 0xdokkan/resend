#include "app_controller.hpp"

#include "app/scene.hpp"
#include "core/text.hpp"
#include "wire/wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iterator>
#include <map>
#include <ratio>
#include <sstream>
#include <utility>

using namespace std::chrono_literals;

namespace resend {
namespace {
using netsim::text::tr;

constexpr double speeds[] = {0.25, 0.5, 1, 2, 4};
// Longest real time a single frame may advance: after a stall the animation
// resumes where it was instead of jumping ahead.
constexpr auto max_frame_step = 100ms;
constexpr auto autosave_interval = 5s;
// A packet is under the pointer this close to its centre, in pixels.
constexpr float packet_reach = 12;

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

std::string speed_text(double speed) {
    std::ostringstream out;
    out << speed << "×";
    return out.str();
}

const char* kind_name(netsim::DeviceKind kind) {
    switch (kind) {
    case netsim::DeviceKind::Host: return "host";
    case netsim::DeviceKind::Switch: return "switch";
    case netsim::DeviceKind::Router: return "router";
    }
    return "host";
}

// The address line of a card: a host's address, a router's, or its ports.
std::string device_address(const netsim::DeviceConfig& device) {
    if (device.kind == netsim::DeviceKind::Switch) return tr("{0} Ethernet ports", {std::to_string(device.ports.size())});
    std::string addresses;
    for (const auto& port : device.ports)
        if (port.ipv4) addresses += (addresses.empty() ? "" : ", ") + port.ipv4->to_string();
    if (addresses.empty()) return tr("no IPv4 address");
    return addresses;
}

// The same in less room: addresses without prefix lengths, the first one
// and how many more.
std::string device_brief(const netsim::DeviceConfig& device) {
    if (device.kind == netsim::DeviceKind::Switch) return tr("{0} ports", {std::to_string(device.ports.size())});
    std::vector<std::string> addresses;
    for (const auto& port : device.ports)
        if (port.ipv4) addresses.push_back(port.ipv4->address().to_string());
    if (addresses.empty()) return tr("no IPv4 address");
    return addresses.size() == 1 ? addresses.front() : addresses.front() + " +" + std::to_string(addresses.size() - 1);
}

const char* tone_name(Tone tone) {
    switch (tone) {
    case Tone::Learn: return "learn";
    case Tone::Forward: return "forward";
    case Tone::Muted: return "muted";
    case Tone::Wait: return "wait";
    case Tone::Drop: return "drop";
    case Tone::Success: return "success";
    case Tone::Edit: return "edit";
    case Tone::Guard: return "guard";
    }
    return "muted";
}

float milliseconds(netsim::SimTime time) { return static_cast<float>(std::chrono::duration<double, std::milli>(time).count()); }
netsim::SimTime from_milliseconds(float value) {
    return netsim::SimTime(std::llround(std::max(0.0, static_cast<double>(value)) * 1e6));
}

// What a device's sprite shows: off above all, then what it does just now,
// then whether it is selected.
int device_state(const netsim::DeviceConfig& device, const std::map<int, look::DeviceState>& doing, bool selected) {
    auto state = look::DeviceState::On;
    if (!device.powered) state = look::DeviceState::Off;
    else if (const auto found = doing.find(static_cast<int>(device.id.value)); found != doing.end()) state = found->second;
    else if (selected) state = look::DeviceState::Selected;
    return static_cast<int>(state);
}

std::string segment_path(const Segment& segment) {
    char buffer[96];
    const int length = std::snprintf(buffer, sizeof buffer, "M%.1f %.1fL%.1f %.1f", segment.a.x, segment.a.y, segment.b.x, segment.b.y);
    return std::string(buffer, static_cast<std::size_t>(std::max(length, 0)));
}

std::string hex_byte(std::uint8_t value) {
    static constexpr char numerals[] = "0123456789abcdef";
    return {numerals[value >> 4], numerals[value & 0xfU]};
}

std::optional<CanvasController::Key> key_named(std::string_view name) {
    using Key = CanvasController::Key;
    static const std::map<std::string_view, Key> keys{
        {"left", Key::Left}, {"right", Key::Right}, {"up", Key::Up}, {"down", Key::Down},
        {"delete", Key::Delete}, {"escape", Key::Escape}, {"enter", Key::Connect},
        {"zoom-in", Key::ZoomIn}, {"zoom-out", Key::ZoomOut}, {"fit", Key::Fit},
    };
    const auto found = keys.find(name);
    return found == keys.end() ? std::nullopt : std::optional(found->second);
}
} // namespace

AppController::AppController(slint::ComponentHandle<AppWindow> ui, Options options)
    : ui_(std::move(ui)),
      state_directory_(std::move(options.state_directory)),
      settings_(load_settings(state_directory_)),
      session_theme_(std::move(options.theme)),
      nodes_(std::make_shared<slint::VectorModel<NodeView>>()),
      links_(std::make_shared<slint::VectorModel<LinkView>>()),
      packets_(std::make_shared<slint::VectorModel<PacketView>>()),
      markers_(std::make_shared<slint::VectorModel<MarkerView>>()),
      shapes_(std::make_shared<slint::VectorModel<ShapeView>>()),
      events_(std::make_shared<slint::VectorModel<EventView>>()),
      fields_(std::make_shared<slint::VectorModel<FieldRow>>()),
      hex_(std::make_shared<slint::VectorModel<HexRow>>()),
      ports_(std::make_shared<slint::VectorModel<PortView>>()),
      routes_(std::make_shared<slint::VectorModel<RouteView>>()),
      table_(std::make_shared<slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>>()),
      routing_(std::make_shared<slint::VectorModel<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>>()),
      commands_(std::make_shared<slint::VectorModel<CommandView>>()),
      table_columns_(std::make_shared<slint::VectorModel<TableColumn>>()),
      routing_columns_(std::make_shared<slint::VectorModel<TableColumn>>()),
      terminal_menus_(std::make_shared<slint::VectorModel<TerminalMenuView>>()),
      terminal_blocks_(std::make_shared<slint::VectorModel<TerminalBlockView>>()),
      goals_(std::make_shared<slint::VectorModel<GoalView>>()),
      recovery_(state_directory_) {
    const auto& geometry = ui_->global<Geometry>();
    geometry.set_card_width(card_width);
    geometry.set_card_height(card_height);
    const auto& canvas = ui_->global<UiCanvas>();
    canvas.set_nodes(nodes_);
    canvas.set_links(links_);
    canvas.set_packets(packets_);
    canvas.set_markers(markers_);
    canvas.set_shapes(shapes_);
    ui_->global<UiTimeline>().set_events(events_);
    ui_->global<UiFrame>().set_rows(fields_);
    ui_->global<UiFrame>().set_hex(hex_);
    ui_->global<UiInspector>().set_ports(ports_);
    ui_->global<UiInspector>().set_routes(routes_);
    ui_->global<UiInspector>().set_table_rows(table_);
    ui_->global<UiInspector>().set_routing_rows(routing_);
    ui_->global<UiInspector>().set_table_columns(table_columns_);
    ui_->global<UiInspector>().set_routing_columns(routing_columns_);
    ui_->global<UiApp>().set_commands(commands_);
    ui_->global<UiApp>().set_version(RESEND_VERSION);
    intensity_ = intensity_named(settings_.effects).value_or(Intensity::Full);
    playback_.set_reduced_motion(intensity_ != Intensity::Full);
    output_ = options.audio ? audio::open_output(synth_) : audio::null_output("none in this session");

    apply_theme();
    bind_callbacks();
    bind_inspector();
    bind_documents();
    bind_commands();
    bind_lab();
    bind_sound();
    bind_terminal();
    ui_->global<UiTerminal>().set_menus(terminal_menus_);
    ui_->global<UiTerminal>().set_blocks(terminal_blocks_);
    apply_settings();
    open(netsim::sample_lan());
    status(tr("Local sandbox. Drag devices, connect ports and watch a ping."));
    refresh_units();
    // The scale factor is known once the window is on a screen, and changes
    // when it moves to another one: checked a few times a second, which
    // redraws nothing unless it changed.
    // The canvas's size is asked for in the same way: a window that opens
    // at the size it asked for never reports a change of it.
    units_timer_.start(slint::TimerMode::Repeated, std::chrono::milliseconds(250), [this] {
        refresh_units();
        ui_->invoke_report_geometry();
    });
    geometry_timer_.start(slint::TimerMode::SingleShot, std::chrono::milliseconds(0), [this] { ui_->invoke_report_geometry(); });
    if (options.welcome && !settings_.welcomed) ui_->global<UiApp>().set_dialog("welcome");

    autosave_timer_.start(slint::TimerMode::Repeated, autosave_interval, [this] { autosave(); });
    ui_->window().on_close_requested([this] {
        return close_requested() ? slint::CloseRequestResponse::HideWindow : slint::CloseRequestResponse::KeepWindowShown;
    });
    ready_ = true;
}

AppController::~AppController() {
    // Closing normally: nothing is left to recover.
    recovery_.clear();
}

void AppController::status(const std::string& value) {
    note(value);
    if (ready_) toast(value);
}

void AppController::note(const std::string& value) { ui_->global<UiApp>().set_status(value.c_str()); }

void AppController::toast(const std::string& text) {
    const auto& app = ui_->global<UiApp>();
    app.set_toast(text.c_str());
    app.set_toast_serial(++toast_serial_);
}

std::string AppController::past_text() const {
    return tr("The instant shown is before later changes: go to the latest instant to change the network.");
}

void AppController::sync() {
    // A new simulation: its events and its playback start over.
    if (workspace_.generation() != generation_) {
        generation_ = workspace_.generation();
        explainer_.clear();
        playback_.reset();
        events_->set_vector({});
        shown_events_.clear();
        listed_ = 0;
    }
    explainer_.update(workspace_.simulation());
    if (selected_pdu_ >= static_cast<int>(simulation().trace().pdus().size())) selected_pdu_ = -1;
}

void AppController::refresh_all() {
    sync();
    refresh_view();
    refresh_topology();
    refresh_scene();
    refresh_events();
    refresh_inspector(true);
    refresh_run();
    refresh_document();
}

void AppController::network_changed() {
    canvas_.validate();
    sync();
    refresh_topology();
    refresh_scene();
    refresh_events();
    refresh_inspector(true);
    refresh_run();
    refresh_document();
}

void AppController::refresh_view() {
    const auto& canvas = ui_->global<UiCanvas>();
    canvas.set_zoom(canvas_.zoom());
    canvas.set_pan_x(canvas_.pan().x);
    canvas.set_pan_y(canvas_.pan().y);
    canvas.set_compact(canvas_.compact());
    refresh_units();
    refresh_overlay();
}

// What is drawn over the cards' positions: the cables, which depend on the
// view, and what the pointer is doing.
void AppController::refresh_overlay() {
    const auto& canvas = ui_->global<UiCanvas>();
    // Cables the selected frame has crossed so far, flooded copies included.
    std::vector<std::uint32_t> path;
    if (selected_pdu_ >= 0)
        for (const auto& sent : simulation().trace().transmissions())
            if (sent.pdu.value == static_cast<std::uint32_t>(selected_pdu_)) path.push_back(sent.link.value);
    const auto paths = canvas_.cable_paths(path);
    canvas.set_cables_up(paths.up.c_str());
    canvas.set_cables_down(paths.down.c_str());
    canvas.set_cables_highlight(paths.highlight.c_str());
    canvas.set_cable_selected(paths.selected.c_str());
    const auto band = canvas_.rubber_band();
    canvas.set_rubber_band(band ? segment_path(*band).c_str() : "");
    refresh_dashes();

    const auto& hover = canvas_.hover();
    const bool on_device = hover.kind == CanvasHit::Kind::Device || hover.kind == CanvasHit::Kind::Port || hover.kind == CanvasHit::Kind::Connector;
    canvas.set_hover_device(on_device ? static_cast<int>(hover.id) : -1);
    canvas.set_hover_port(hover.kind == CanvasHit::Kind::Port ? hover.port : hover.kind == CanvasHit::Kind::Connector ? -1 : -2);
    const auto source = canvas_.cable_source();
    canvas.set_source_device(source && canvas_.drawing_cable() ? static_cast<int>(source->device.value) : -1);
    canvas.set_panning(canvas_.panning());
    canvas.set_hint(canvas_.drawing_cable() ? tr("Plugging a cable: end it on a free port or on another device. Escape cancels.").c_str()
                                            : tr("Drag a card to move it · drag from a port or + to plug a cable · wheel to zoom").c_str());
}

// Cards and cables: cheap enough to run on every step of a drag.
void AppController::refresh_topology() {
    const auto& network = scenario();
    // A port is down when its cable is, or when it or the far end is disabled.
    std::map<netsim::DeviceId, unsigned> connected, down;
    const auto enabled = [&](netsim::PortRef port) {
        const auto* device = network.device(port.device);
        return device && device->ports.at(port.port).admin_up;
    };
    for (const auto& link : network.links) {
        const bool up = link.up && enabled(link.a) && enabled(link.b);
        for (const auto end : {link.a, link.b}) {
            connected[end.device] |= 1U << end.port;
            if (!up) down[end.device] |= 1U << end.port;
        }
    }
    const auto scene = build_scene(network, simulation(), playback_, intensity_);
    const std::map<int, unsigned> activity(scene.port_activity.begin(), scene.port_activity.end());
    const std::map<int, look::DeviceState> doing(scene.device_activity.begin(), scene.device_activity.end());
    const std::map<int, Point> nudges(scene.nudges.begin(), scene.nudges.end());
    const auto& selection = canvas_.selection();

    std::vector<NodeView> node_rows;
    shown_activity_.clear();
    for (const auto& device : network.devices) {
        const int id = static_cast<int>(device.id.value);
        NodeView row{};
        row.id = id;
        row.title = device.name.c_str();
        row.kind = kind_name(device.kind);
        row.address = device_address(device).c_str();
        row.brief = device_brief(device).c_str();
        row.x = static_cast<float>(device.x);
        row.y = static_cast<float>(device.y);
        row.powered = device.powered;
        row.selected = selection.kind == Selection::Kind::Device && selection.id == device.id.value;
        row.port_count = static_cast<int>(device.ports.size());
        row.connected_ports = static_cast<int>(connected[device.id]);
        row.down_ports = static_cast<int>(down[device.id]);
        const auto active = activity.find(id);
        shown_activity_.push_back(active == activity.end() ? 0U : active->second);
        row.active_ports = static_cast<int>(shown_activity_.back());
        row.state = device_state(device, doing, row.selected);
        if (const auto nudge = nudges.find(id); nudge != nudges.end()) {
            row.nudge_x = nudge->second.x;
            row.nudge_y = nudge->second.y;
        }
        node_rows.push_back(row);
    }
    replace_rows(nodes_, node_rows);

    std::vector<LinkView> link_rows;
    for (const auto& link : network.links) {
        const auto* a = network.device(link.a.device);
        const auto* b = network.device(link.b.device);
        const auto segment = canvas_.cable_segment(link.id);
        if (!a || !b || !segment) continue;
        LinkView row{};
        row.id = static_cast<int>(link.id.value);
        const auto label = netsim::explain::port_text(network, link.a) + " — " + netsim::explain::port_text(network, link.b);
        row.label = label.c_str();
        row.ax = segment->a.x;
        row.ay = segment->a.y;
        row.bx = segment->b.x;
        row.by = segment->b.y;
        row.up = link.up;
        // Port numbers where there is more than one port.
        row.a_port = a->kind != netsim::DeviceKind::Host ? std::to_string(link.a.port + 1).c_str() : "";
        row.b_port = b->kind != netsim::DeviceKind::Host ? std::to_string(link.b.port + 1).c_str() : "";
        link_rows.push_back(row);
    }
    replace_rows(links_, link_rows);
    ui_->global<UiInspector>().set_summary(
        ((network.title.empty() ? tr("Your own network") : network.title) + "\n" +
         (network.devices.size() == 1 ? tr("1 device") : tr("{0} devices", {std::to_string(network.devices.size())})) + " · " +
         (network.links.size() == 1 ? tr("1 cable") : tr("{0} cables", {std::to_string(network.links.size())})))
            .c_str());
    refresh_overlay();
}

// Dashes move along the cables frames are on, with simulated time; they
// follow the view too, so the overlay redraws them.
void AppController::refresh_dashes() {
    std::vector<Segment> busy;
    for (const auto& packet : shown_packets_) {
        const auto same = [&](const Segment& cable) {
            return std::abs(cable.a.x - packet.cable.a.x) < 0.01f && std::abs(cable.a.y - packet.cable.a.y) < 0.01f &&
                   std::abs(cable.b.x - packet.cable.b.x) < 0.01f && std::abs(cable.b.y - packet.cable.b.y) < 0.01f;
        };
        if (std::none_of(busy.begin(), busy.end(), same)) busy.push_back(packet.cable);
    }
    std::string busy_path;
    for (const auto& dash : dashes(busy, dash_phase(playback_.now())))
        busy_path += segment_path({canvas_.to_screen(dash.a), canvas_.to_screen(dash.b)});
    ui_->global<UiCanvas>().set_cables_busy(busy_path.c_str());
}

// Packets, decision chips, port lights, the clock and the tables: runs on every frame.
void AppController::refresh_scene() {
    const auto scene = build_scene(scenario(), simulation(), playback_, intensity_);
    std::vector<PacketView> packet_rows;
    for (const auto& packet : scene.packets) {
        PacketView row{};
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
        row.kind = static_cast<int>(packet.kind);
        packet_rows.push_back(row);
    }
    replace_rows(packets_, packet_rows);
    shown_packets_ = scene.packets;
    refresh_dashes();

    std::vector<MarkerView> marker_rows;
    for (const auto& marker : scene.markers) {
        MarkerView row{};
        row.x = marker.anchor.x;
        row.y = marker.anchor.y;
        row.stack = marker.stack;
        row.text = marker.text.c_str();
        row.tone = tone_name(marker.tone);
        row.opacity = marker.opacity;
        row.icon = marker.icon.c_str();
        marker_rows.push_back(row);
    }
    replace_rows(markers_, marker_rows);

    std::vector<ShapeView> shape_rows;
    for (const auto& shape : scene.shapes) {
        ShapeView row{};
        row.form = shape.form == Shape::Form::Ring ? "ring" : shape.form == Shape::Form::Dot ? "dot" : "flash";
        row.x = shape.at.x;
        row.y = shape.at.y;
        row.size = shape.size;
        row.tone = shape.tone.c_str();
        row.opacity = shape.opacity;
        shape_rows.push_back(row);
    }
    replace_rows(shapes_, shape_rows);

    const std::map<int, unsigned> activity(scene.port_activity.begin(), scene.port_activity.end());
    const std::map<int, look::DeviceState> doing(scene.device_activity.begin(), scene.device_activity.end());
    const std::map<int, Point> nudges(scene.nudges.begin(), scene.nudges.end());
    const auto& devices = scenario().devices;
    if (shown_activity_.size() == devices.size() && nodes_->row_count() == devices.size()) {
        for (std::size_t row = 0; row < devices.size(); ++row) {
            const auto found = activity.find(static_cast<int>(devices[row].id.value));
            const unsigned mask = found == activity.end() ? 0U : found->second;
            const auto moved = nudges.find(static_cast<int>(devices[row].id.value));
            const Point nudge = moved == nudges.end() ? Point{} : moved->second;
            auto data = *nodes_->row_data(row);
            const int state = device_state(devices[row], doing, data.selected);
            if (mask == shown_activity_[row] && state == data.state && nudge.x == data.nudge_x && nudge.y == data.nudge_y) continue;
            data.active_ports = static_cast<int>(mask);
            data.state = state;
            data.nudge_x = nudge.x;
            data.nudge_y = nudge.y;
            nodes_->set_row_data(row, data);
            shown_activity_[row] = mask;
        }
    }
    const auto& canvas = ui_->global<UiCanvas>();
    canvas.set_clock(netsim::format_ms(playback_.now()).c_str());
    canvas.set_running(playback_.playing());
    canvas.set_speed_label(speed_text(playback_.speed()).c_str());
    const auto& timeline = ui_->global<UiTimeline>();
    timeline.set_end_ms(milliseconds(std::max(workspace_.end_time(), playback_.now())));
    timeline.set_now_ms(milliseconds(playback_.now()));
    timeline.set_locked(workspace_.in_past());
    refresh_tables();
}

void AppController::refresh_events() {
    const auto& events = explainer_.events();
    const auto& timeline = ui_->global<UiTimeline>();
    // Events only grow until a new simulation; the filter may skip some.
    for (; listed_ < events.size(); ++listed_) {
        const auto& event = events[listed_];
        if (!filter_.accepts(event)) continue;
        EventView row{};
        row.index = static_cast<int>(listed_);
        row.time = netsim::format_ms(event.time).c_str();
        row.protocol = event.protocol.c_str();
        row.summary = event.summary.c_str();
        row.problem = !event.success;
        events_->push_back(row);
        shown_events_.push_back(listed_);
    }
    const int previous_pdu = selected_pdu_;
    if (events.empty()) selected_event_ = -1;
    else if (follow_events_ || selected_event_ >= static_cast<int>(events.size()))
        selected_event_ = shown_events_.empty() ? -1 : static_cast<int>(shown_events_.back());
    if (follow_events_) selected_pdu_ = selected_event_ >= 0 ? id_or_none(events[static_cast<std::size_t>(selected_event_)].pdu) : -1;

    timeline.set_total(static_cast<int>(events.size()));
    timeline.set_selected(selected_event_);
    timeline.set_follow(follow_events_);
    timeline.set_arp(filter_.arp);
    timeline.set_icmp(filter_.icmp);
    timeline.set_ipv4(filter_.ipv4);
    timeline.set_edits(filter_.edits);
    timeline.set_simulator(filter_.simulator);
    timeline.set_problems(filter_.problems_only);
    timeline.set_device_only(filter_.device.has_value());
    const auto* device = filter_.device ? scenario().device(*filter_.device) : nullptr;
    timeline.set_device_name(device ? device->name.c_str() : "");
    timeline.set_filtered(filter_.active());
    if (follow_events_ && selected_event_ >= 0) {
        const auto row = std::lower_bound(shown_events_.begin(), shown_events_.end(), static_cast<std::size_t>(selected_event_));
        timeline.set_reveal_row(static_cast<int>(row - shown_events_.begin()));
        timeline.set_reveal_serial(timeline.get_reveal_serial() + 1);
    }
    refresh_explanation();
    if (selected_pdu_ != previous_pdu || fields_->row_count() == 0) {
        refresh_frame();
        refresh_overlay();
    }
}

void AppController::refresh_explanation() {
    const auto& events = explainer_.events();
    const auto& timeline = ui_->global<UiTimeline>();
    if (selected_event_ >= 0 && selected_event_ < static_cast<int>(events.size())) {
        const auto& event = events[static_cast<std::size_t>(selected_event_)];
        timeline.set_title(event.summary.c_str());
        timeline.set_detail(event.detail.c_str());
        timeline.set_protocol(event.protocol.c_str());
    } else {
        timeline.set_title(tr("The network is ready for your first experiment.").c_str());
        timeline.set_detail(tr("Select a host, choose an IPv4 address and press Send. Step plays one event at a time; Run "
                               "plays the whole experiment. The tables show the state of the network at the instant on the "
                               "canvas. Several pings can run at once.").c_str());
        timeline.set_protocol("");
    }
}

// The selected frame, field by field, and its bytes.
void AppController::refresh_frame() {
    const auto& frame = ui_->global<UiFrame>();
    frame.set_selected_row(-1);
    frame.set_selected_offset(-1);
    frame.set_selected_length(0);
    frame.set_selection("");
    if (selected_pdu_ < 0) {
        fields_->set_vector({});
        hex_->set_vector({});
        frame.set_title("");
        return;
    }
    const auto& pdu = simulation().trace().pdu(netsim::PduId{static_cast<std::uint32_t>(selected_pdu_)});
    const auto bytes = netsim::wire::encode(pdu);
    std::vector<FieldRow> rows;
    for (const auto& layer : netsim::wire::dissect(bytes)) {
        rows.push_back({true, layer.label.c_str(), "", "", static_cast<int>(layer.offset), static_cast<int>(layer.length)});
        for (const auto& field : layer.fields)
            rows.push_back({false, field.label.c_str(), field.value.c_str(), field.meaning.c_str(), static_cast<int>(field.offset),
                            static_cast<int>(field.length)});
    }
    fields_->set_vector(rows);
    std::vector<HexRow> lines;
    for (std::size_t start = 0; start < bytes.size(); start += 16) {
        std::vector<HexByte> line;
        for (std::size_t at = start; at < std::min(start + 16, bytes.size()); ++at)
            line.push_back({static_cast<int>(at), hex_byte(bytes[at]).c_str()});
        char offset[20];
        std::snprintf(offset, sizeof offset, "%04zx", start);
        lines.push_back({offset, std::make_shared<slint::VectorModel<HexByte>>(line)});
    }
    hex_->set_vector(lines);
    frame.set_title(tr("{0} · {1} bytes", {frame_name(netsim::kind_of(pdu)), std::to_string(bytes.size())}).c_str());
}

void AppController::select_field(int row) {
    const auto data = row >= 0 ? fields_->row_data(static_cast<std::size_t>(row)) : std::nullopt;
    if (!data || data->header) return;
    const auto& frame = ui_->global<UiFrame>();
    frame.set_selected_row(row);
    frame.set_selected_offset(data->offset);
    frame.set_selected_length(data->length);
    const auto value = data->meaning.empty() ? std::string(data->value) : tr("{0} ({1})", {std::string(data->value), std::string(data->meaning)});
    const auto bytes = data->length == 1 ? tr("byte {0}", {std::to_string(data->offset)})
                                         : tr("bytes {0}–{1}", {std::to_string(data->offset), std::to_string(data->offset + data->length - 1)});
    frame.set_selection(tr("{0}: {1} · {2}", {std::string(data->label), value, bytes}).c_str());
}

void AppController::refresh_run() {
    const auto& run = ui_->global<UiRun>();
    const auto& selection = canvas_.selection();
    const auto* device = selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
    const bool host = device && device->kind == netsim::DeviceKind::Host;
    run.set_can_ping(host);
    run.set_source(host ? device->name.c_str() : "");
    run.set_running(playback_.playing());
    run.set_pending(workspace_.next_event_time().has_value());
    run.set_speed_label(speed_text(playback_.speed()).c_str());
    run.set_speed(static_cast<float>(playback_.speed()));
}

void AppController::canvas_changed(unsigned change) {
    if (change & CanvasController::Network) {
        network_changed();
        if (change & CanvasController::View) refresh_view();
    } else {
        if (change & CanvasController::View) refresh_view();
        if (change & CanvasController::Selection) {
            refresh_topology();
            refresh_inspector(true);
            refresh_run();
        } else if (change & CanvasController::Overlay) {
            refresh_overlay();
        }
    }
    // While a cable is being drawn the hint over the canvas says what to do;
    // how it ended is an answer.
    if (auto message = canvas_.take_status(); !message.empty()) {
        if (canvas_.drawing_cable()) note(message);
        else status(message);
    }
}

void AppController::sync_playback() {
    const bool playing = playback_.playing();
    ui_->global<UiRun>().set_running(playing);
    ui_->global<UiCanvas>().set_running(playing);
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
void AppController::playback_command(Command command, std::chrono::nanoseconds real) {
    const bool was_pending = workspace_.next_event_time().has_value();
    // Only what the playback itself shows is heard: whatever a jump or a
    // change added since the last command is passed over in silence.
    cues_.skip(simulation());
    const auto records = simulation().trace().records().size();
    const auto edits = simulation().edits().size();
    const bool was_past = workspace_.in_past();
    command();
    cue_clock_ += real;
    play(mixer_.mix(cues_.take(simulation()), cue_clock_));
    const auto& trace = simulation().trace().records();
    // The storm guard pauses the animation: what follows is the simulator
    // stepping in, not the network, and deserves a look.
    const bool guarded = std::any_of(trace.begin() + static_cast<std::ptrdiff_t>(records), trace.end(),
                                     [](const auto& record) { return record.kind == netsim::TraceKind::SimulatorGuard; });
    if (guarded) playback_.pause();
    if (trace.size() != records) {
        sync();
        refresh_events();
        refresh_terminal(false);
    }
    // Changes recorded for later instants are made again as their time comes.
    if (simulation().edits().size() != edits || workspace_.in_past() != was_past) {
        refresh_topology();
        refresh_inspector(true);
        refresh_document();
    }
    refresh_scene();
    refresh_run();
    sync_playback();
    if (guarded) {
        status(tr("Storm protection: a frame went round an Ethernet loop and the simulator removed its copies. Playback paused."));
        return;
    }
    if (was_pending && !workspace_.next_event_time()) {
        const auto& pings = simulation().pings();
        const bool all = std::all_of(pings.begin(), pings.end(), [](const auto& ping) { return ping.state == netsim::PingState::Succeeded; });
        status(all ? (pings.size() > 1 ? tr("Every ping succeeded. Explore the events and the learned tables.")
                                       : tr("Ping succeeded. Explore the events and the learned tables."))
                   : tr("Experiment finished: read the last event to understand the result."));
    }
}

void AppController::advance_playback(std::chrono::nanoseconds elapsed) {
    playback_command([&] { playback_.tick(elapsed); }, elapsed);
}

void AppController::seek(netsim::SimTime time) { travel(time); }

void AppController::travel(netsim::SimTime time) {
    playback_.pause();
    time = std::max(time, netsim::SimTime{});
    if (time < playback_.now()) {
        // Back: the run is replayed up to that instant.
        workspace_.travel(time);
        sync();
    } else if (time > playback_.now()) {
        playback_.seek(time);
    }
    follow_events_ = true;
    sync();
    refresh_topology();
    refresh_scene();
    refresh_events();
    refresh_inspector(true);
    refresh_run();
    refresh_document();
    sync_playback();
}

void AppController::select_event(int index) {
    const auto& events = explainer_.events();
    if (index < 0 || index >= static_cast<int>(events.size())) return;
    selected_event_ = index;
    selected_pdu_ = id_or_none(events[static_cast<std::size_t>(index)].pdu);
    follow_events_ = false;
    const auto& timeline = ui_->global<UiTimeline>();
    timeline.set_selected(selected_event_);
    timeline.set_follow(false);
    refresh_explanation();
    refresh_frame();
    refresh_overlay();
    refresh_scene();
}

void AppController::select_frame(int pdu) {
    if (pdu < 0 || pdu >= static_cast<int>(simulation().trace().pdus().size())) return;
    selected_pdu_ = pdu;
    follow_events_ = false;
    // The latest event about this frame explains it.
    const auto& events = explainer_.events();
    for (auto index = events.size(); index-- > 0;) {
        if (id_or_none(events[index].pdu) == pdu) {
            selected_event_ = static_cast<int>(index);
            break;
        }
    }
    const auto& timeline = ui_->global<UiTimeline>();
    timeline.set_selected(selected_event_);
    timeline.set_follow(false);
    timeline.set_tab(1);
    refresh_explanation();
    refresh_frame();
    refresh_overlay();
    refresh_scene();
}

bool AppController::report(const Outcome& outcome, const std::string& label, std::size_t commands_before) {
    if (outcome.in_past) {
        status(past_text());
        return false;
    }
    if (!outcome.ok()) {
        status(why_refused(outcome));
        return false;
    }
    // The canvas and the inspector show the change itself.
    if (workspace_.commands().size() > commands_before)
        note(tr("{0}: applied at t = {1}.", {label, netsim::format_ms(simulation().now())}));
    else
        note(label + ".");
    return true;
}

void AppController::start_ping() {
    const auto& run = ui_->global<UiRun>();
    const auto destination = netsim::Ipv4Address::parse(std::string(run.get_target()));
    netsim::PingOptions options;
    // A traceroute's count is the hops it tries.
    options.traceroute = run.get_traceroute();
    const std::string count_text(options.traceroute ? run.get_hops() : run.get_count());
    int count = options.traceroute ? netsim::default_max_hops : 1;
    if (!count_text.empty()) {
        std::size_t consumed = 0;
        try {
            count = std::stoi(count_text, &consumed);
        } catch (const std::exception&) {
            consumed = 0;
        }
        if (consumed != count_text.size()) count = 0;
    }
    if (options.traceroute && (count < 1 || count > netsim::max_hops)) {
        status(tr("A traceroute tries 1 to 255 hops."));
        return;
    }
    if (!options.traceroute && (count < 1 || count > netsim::max_ping_count)) {
        status(tr("Send 1 to 1000 Echo Requests per ping."));
        return;
    }
    options.count = static_cast<std::uint16_t>(count);
    const auto& selection = canvas_.selection();
    const auto* device = selection.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection.id}) : nullptr;
    if (!device || device->kind != netsim::DeviceKind::Host) {
        status(netsim::explain::describe(netsim::Problem::SourceNotHost));
        return;
    }
    if (!destination) {
        status(netsim::explain::describe(netsim::Problem::InvalidDestination));
        return;
    }
    // Pings may overlap: a new one starts at the instant on screen.
    const auto outcome = workspace_.ping(device->id, *destination, options);
    if (outcome.in_past) {
        status(past_text());
        return;
    }
    if (!outcome.ok()) {
        status(netsim::explain::describe(outcome.problem));
        return;
    }
    follow_events_ = true;
    if (options.traceroute)
        status(playback_.playing() ? tr("Traceroute started.") : tr("Traceroute ready. Press Step or Run to advance simulated time."));
    else
        status(playback_.playing() ? tr("Ping started.") : tr("Ping ready. Press Step or Run to advance simulated time."));
    sync();
    refresh_events();
    refresh_scene();
    refresh_run();
    refresh_document();
    sync_playback();
}

void AppController::undo() {
    const auto* step = workspace_.history().undoable();
    const auto label = step ? step->label : std::string();
    const auto before = workspace_.commands().size();
    const auto outcome = workspace_.undo();
    if (!step && !outcome.in_past) {
        status(tr("Nothing to undo."));
        return;
    }
    if (report(outcome, tr("Undo {0}", {label}), before)) {
        network_changed();
        toast(std::string(ui_->global<UiApp>().get_status()));
    }
}

void AppController::redo() {
    const auto* step = workspace_.history().redoable();
    const auto label = step ? step->label : std::string();
    const auto before = workspace_.commands().size();
    const auto outcome = workspace_.redo();
    if (!step && !outcome.in_past) {
        status(tr("Nothing to redo."));
        return;
    }
    if (report(outcome, tr("Redo {0}", {label}), before)) {
        network_changed();
        toast(std::string(ui_->global<UiApp>().get_status()));
    }
}

bool AppController::select_device(std::string_view name) {
    const auto& devices = scenario().devices;
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return device.name == name; });
    if (found == devices.end()) return false;
    canvas_changed(canvas_.select({Selection::Kind::Device, found->id.value}));
    return true;
}

void AppController::add_device(netsim::DeviceKind kind) {
    const auto place = canvas_.new_card_place();
    const netsim::AddDevice edit{kind, static_cast<int>(std::lround(place.x)), static_cast<int>(std::lround(place.y))};
    const auto label = kind == netsim::DeviceKind::Host ? tr("Add a host") : kind == netsim::DeviceKind::Switch ? tr("Add a switch") : tr("Add a router");
    const auto before = workspace_.commands().size();
    if (!report(workspace_.change(edit, label), label, before)) return;
    canvas_.select({Selection::Kind::Device, scenario().devices.back().id.value});
    network_changed();
}

void AppController::bind_callbacks() {
    const auto& canvas = ui_->global<UiCanvas>();
    canvas.on_resized([this](float width, float height) { canvas_changed(canvas_.resize(width, height)); });
    canvas.on_free_area([this](float x, float y, float width, float height) {
        canvas_changed(canvas_.set_free_area({x, y, width, height}));
    });
    canvas.on_press([this](float x, float y, int button) {
        const Point point{x, y};
        // Packets lie over everything: a click on one shows its frame.
        if (button == 0) {
            for (const auto& packet : shown_packets_) {
                const auto at = canvas_.to_screen(packet.at);
                if (std::hypot(at.x - x, at.y - y) <= packet_reach) {
                    select_frame(packet.pdu);
                    return;
                }
            }
        }
        const auto which = button == 2 ? CanvasController::Button::Right : button == 1 ? CanvasController::Button::Middle
                                                                                         : CanvasController::Button::Left;
        canvas_changed(canvas_.press(point, which));
    });
    canvas.on_moved([this](float x, float y) {
        canvas_changed(canvas_.move({x, y}));
    });
    canvas.on_release([this](float x, float y) { canvas_changed(canvas_.release({x, y})); });
    canvas.on_wheel([this](float x, float y, float delta) {
        if (delta != 0) canvas_changed(canvas_.zoom_at({x, y}, delta > 0 ? 1.1f : 1 / 1.1f));
    });
    canvas.on_key([this](slint::SharedString name, bool fine) {
        const auto key = key_named(std::string_view(name));
        if (!key) return false;
        // A dialog is over the network: its keys are not the canvas's.
        if (!ui_->global<UiApp>().get_dialog().empty()) return false;
        const auto change = canvas_.key(*key, fine);
        canvas_changed(change);
        return change != CanvasController::None || *key == CanvasController::Key::Delete;
    });
    canvas.on_select_device([this](int id) {
        if (id >= 0) canvas_changed(canvas_.select({Selection::Kind::Device, static_cast<std::uint32_t>(id)}));
    });
    canvas.on_select_cable([this](int id) {
        if (id >= 0) canvas_changed(canvas_.select({Selection::Kind::Cable, static_cast<std::uint32_t>(id)}));
    });
    canvas.on_connector([this](int id) {
        if (id >= 0) canvas_changed(canvas_.connector(static_cast<std::uint32_t>(id)));
    });
    canvas.on_select_frame([this](int pdu) { select_frame(pdu); });
    canvas.on_zoom_by([this](float factor) { canvas_changed(canvas_.zoom_by(factor)); });
    canvas.on_reset_zoom([this] { canvas_changed(canvas_.reset_zoom()); });
    canvas.on_fit([this] { canvas_changed(canvas_.fit()); });

    const auto& timeline = ui_->global<UiTimeline>();
    timeline.on_select([this](int index) { select_event(index); });
    timeline.on_toggle_filter([this](slint::SharedString which) {
        const std::string_view name(which);
        if (name == "arp") filter_.arp = !filter_.arp;
        else if (name == "icmp") filter_.icmp = !filter_.icmp;
        else if (name == "ipv4") filter_.ipv4 = !filter_.ipv4;
        else if (name == "edits") filter_.edits = !filter_.edits;
        else if (name == "simulator") filter_.simulator = !filter_.simulator;
        else if (name == "problems") filter_.problems_only = !filter_.problems_only;
        else if (name == "device") {
            const auto& selection = canvas_.selection();
            if (filter_.device) {
                filter_.device.reset();
            } else if (selection.kind == Selection::Kind::Device) {
                filter_.device = netsim::DeviceId{selection.id};
            } else {
                status(tr("Select a device to see only its events."));
                return;
            }
        }
        events_->set_vector({});
        shown_events_.clear();
        listed_ = 0;
        refresh_events();
    });
    timeline.on_clear_filters([this] {
        filter_ = {};
        events_->set_vector({});
        shown_events_.clear();
        listed_ = 0;
        refresh_events();
    });
    timeline.on_scrub([this](float value) {
        const auto time = from_milliseconds(value);
        if (time != playback_.now()) travel(time);
    });
    timeline.on_latest([this] { travel(workspace_.end_time()); });

    const auto& frame = ui_->global<UiFrame>();
    frame.on_select_row([this](int row) { select_field(row); });
    frame.on_select_byte([this](int index) {
        // The smallest field holding the byte: a flag rather than its whole byte.
        int best = -1, length = 0;
        for (std::size_t row = 0; row < fields_->row_count(); ++row) {
            const auto data = *fields_->row_data(row);
            if (data.header || index < data.offset || index >= data.offset + data.length) continue;
            if (best < 0 || data.length < length) {
                best = static_cast<int>(row);
                length = data.length;
            }
        }
        select_field(best);
    });

    const auto& run = ui_->global<UiRun>();
    run.on_ping([this] { start_ping(); });
    run.on_step([this] { playback_command([this] { playback_.step(); }); });
    run.on_run_toggle([this] {
        if (playback_.playing()) {
            playback_command([this] { playback_.pause(); });
            return;
        }
        if (!workspace_.next_event_time()) {
            status(tr("Prepare a ping to start an experiment."));
            return;
        }
        playback_command([this] { playback_.play(); });
    });
    run.on_cycle_speed([this] {
        const auto next = std::find_if(std::begin(speeds), std::end(speeds), [&](double s) { return s > playback_.speed(); });
        playback_.set_speed(next == std::end(speeds) ? speeds[0] : *next);
        refresh_run();
        refresh_scene();
    });
    run.on_set_speed([this](float speed) {
        playback_.set_speed(speed);
        refresh_run();
        refresh_scene();
    });
    run.on_reset([this] {
        playback_.pause();
        workspace_.reset();
        follow_events_ = true;
        selected_event_ = selected_pdu_ = -1;
        refresh_all();
        sync_playback();
        status(tr("Caches, tables and events cleared. The topology stays as it is."));
    });
    run.on_latest([this] { travel(workspace_.end_time()); });

    const auto& app = ui_->global<UiApp>();
    app.on_undo([this] { undo(); });
    app.on_redo([this] { redo(); });
    app.on_add_device([this](slint::SharedString kind) {
        const std::string_view name(kind);
        add_device(name == "switch" ? netsim::DeviceKind::Switch : name == "router" ? netsim::DeviceKind::Router : netsim::DeviceKind::Host);
    });
    app.on_delete_selection([this] { canvas_changed(canvas_.key(CanvasController::Key::Delete)); });
    app.on_set_effects([this](slint::SharedString level) {
        const std::string_view text(level);
        // "next": full, reduced, off, and around again.
        const auto chosen = text == "next" ? (intensity_ == Intensity::Full ? Intensity::Reduced
                                              : intensity_ == Intensity::Reduced ? Intensity::Off : Intensity::Full)
                                           : intensity_named(text);
        if (chosen) set_effects(*chosen);
    });
}

void AppController::set_effects(Intensity intensity) {
    intensity_ = intensity;
    settings_.effects = std::string(name(intensity));
    save_settings();
    playback_.set_reduced_motion(intensity != Intensity::Full);
    ui_->global<Motion>().set_effects(settings_.effects.c_str());
    refresh_scene();
    refresh_commands();
    status(intensity == Intensity::Full ? tr("Effects full: packets travel along the cables, and every event has its reaction.")
           : intensity == Intensity::Reduced ? tr("Effects reduced: events appear one instant at a time, without particles or shaking.")
                                             : tr("Effects off: events appear one instant at a time, with their markers and texts only."));
}

} // namespace resend

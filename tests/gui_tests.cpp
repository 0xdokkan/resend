// GUI behaviour tests on Slint's testing backend: no display is needed.
// Elements are found by accessible label and driven through accessibility
// actions or real pointer and keyboard events, as a user or a screen reader
// would. Menu items, which the testing backend does not expose, are driven
// through the callbacks they invoke.
#include "app_controller.hpp"
#include "look/look.hpp"
#include "check.hpp"
#include "scenario/scenario_json.hpp"

#include <slint-platform.h>
#include <slint-testing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using slint::language::AccessibleRole;
using slint::testing::ElementHandle;
using Ui = slint::ComponentHandle<AppWindow>;
using netsim::DeviceId;
using netsim::LinkId;

bool succeeded(const resend::AppController& app) {
    const auto& pings = app.simulation().pings();
    return !pings.empty() && std::all_of(pings.begin(), pings.end(), [](const auto& ping) { return ping.state == netsim::PingState::Succeeded; });
}

// Text elements are labelled by their own text, so a button and its caption
// share a label: every lookup ignores plain text.
std::vector<ElementHandle> controls(const Ui& ui, std::string_view label) {
    std::vector<ElementHandle> result;
    for (const auto& item : ElementHandle::find_by_accessible_label(ui, label))
        if (item.accessible_role() != AccessibleRole::Text) result.push_back(item);
    return result;
}
ElementHandle element(const Ui& ui, std::string_view label) {
    const auto found = controls(ui, label);
    if (found.size() != 1) {
        std::cerr << "Expected one control labelled \"" << label << "\", found " << found.size() << ". Controls with a label:\n";
        ElementHandle::visit_elements(ui, [](const ElementHandle& item) {
            if (const auto text = item.accessible_label(); text && !text->empty() && item.accessible_role() != AccessibleRole::Text)
                std::cerr << "  " << std::string_view(*text) << '\n';
        });
    }
    CHECK(found.size() == 1);
    return found[0];
}
bool exists(const Ui& ui, std::string_view label) { return !controls(ui, label).empty(); }
// Plain text on screen, such as the decision chips on the canvas.
bool shown(const Ui& ui, std::string_view text) { return !ElementHandle::find_by_accessible_label(ui, text).empty(); }
void press(const Ui& ui, std::string_view label) { element(ui, label).invoke_accessible_default_action(); }
void type(const Ui& ui, std::string_view label, std::string_view value) { element(ui, label).set_accessible_value(slint::SharedString(value)); }
// A filter of the timeline, by its text.
ElementHandle chip(const Ui& ui, std::string_view label) {
    std::vector<ElementHandle> found;
    for (const auto& item : ElementHandle::find_by_accessible_label(ui, label))
        if (item.accessible_role() == AccessibleRole::Checkbox) found.push_back(item);
    if (found.size() != 1) std::cerr << "Expected one filter labelled \"" << label << "\", found " << found.size() << '\n';
    CHECK(found.size() == 1);
    return found[0];
}

// Runs what Slint leaves to the event loop: `changed` handlers, then the
// layout of what they changed, such as the rows of a list they scrolled.
// Not while playing: the frame timer would add real time.
void settle(const Ui& ui) {
    slint::platform::update_timers_and_animations();
    for (const auto& list : ElementHandle::find_by_accessible_label(ui, "Event list")) (void)list.size();
    slint::platform::update_timers_and_animations();
}
// The newest event, in view at the bottom of the list as it follows them.
bool newest(const Ui& ui, std::string_view label) {
    settle(ui);
    const auto events = ui->global<UiTimeline>().get_events();
    const auto count = events->row_count();
    if (count == 0) return false;
    const auto last = *events->row_data(count - 1);
    return std::string(last.protocol) + ": " + std::string(last.summary) == label && exists(ui, label);
}
// An event in the list, in view or scrolled out of it.
bool listed(const Ui& ui, std::string_view label) {
    const auto events = ui->global<UiTimeline>().get_events();
    for (std::size_t row = 0; row < events->row_count(); ++row) {
        const auto event = *events->row_data(row);
        if (std::string(event.protocol) + ": " + std::string(event.summary) == label) return true;
    }
    return false;
}
std::string status(const Ui& ui) { return std::string(ui->global<UiApp>().get_status()); }
std::string clock(const Ui& ui) { return std::string(ui->global<UiCanvas>().get_clock()); }
bool running(const Ui& ui) { return ui->global<UiRun>().get_running(); }
std::string dialog(const Ui& ui) { return std::string(ui->global<UiApp>().get_dialog()); }

// Steps through the experiment, letting real time pass after every press so
// each step's animation completes.
void run_to_end(const Ui& ui, resend::AppController& app) {
    for (int step = 0; step < 500 && app.simulation().pending(); ++step) {
        press(ui, "Step");
        app.advance_playback(2s);
    }
    CHECK(!app.simulation().pending());
}
void play_to_end(const Ui& ui, resend::AppController& app) {
    press(ui, "Run");
    // To the last event, then through the afterglow, when it stops.
    for (int frame = 0; frame < 2000 && (app.simulation().pending() || running(ui)); ++frame) app.advance_playback(100ms);
    CHECK(!app.simulation().pending() && !running(ui));
}
slint::LogicalPosition center(const ElementHandle& item) {
    const auto at = item.absolute_position();
    const auto size = item.size();
    return slint::LogicalPosition({at.x + size.width / 2, at.y + size.height / 2});
}
bool inside(const ElementHandle& item, const ElementHandle& area) {
    const auto a = item.absolute_position(), b = area.absolute_position();
    const auto as = item.size(), bs = area.size();
    return a.x >= b.x && a.y >= b.y && a.x + as.width <= b.x + bs.width && a.y + as.height <= b.y + bs.height;
}
void drag(const Ui& ui, slint::LogicalPosition from, slint::LogicalPosition to) {
    auto& window = ui->window();
    window.dispatch_pointer_move_event(from);
    window.dispatch_pointer_press_event(from, slint::PointerEventButton::Left);
    for (int step = 1; step <= 5; ++step)
        window.dispatch_pointer_move_event(slint::LogicalPosition({from.x + (to.x - from.x) * static_cast<float>(step) / 5,
                                                                   from.y + (to.y - from.y) * static_cast<float>(step) / 5}));
    window.dispatch_pointer_release_event(to, slint::PointerEventButton::Left);
}
void key(const Ui& ui, const slint::SharedString& text) {
    settle(ui);
    ui->window().dispatch_key_press_event(text);
    ui->window().dispatch_key_release_event(text);
}
// Ctrl plus a key, and Shift too if asked.
void shortcut(const Ui& ui, const slint::SharedString& text, bool shift = false) {
    settle(ui);  // e.g. the focus coming back to the canvas after a dialog
    auto& window = ui->window();
    window.dispatch_key_press_event(slint::platform::key_codes::Control);
    if (shift) window.dispatch_key_press_event(slint::platform::key_codes::Shift);
    key(ui, text);
    if (shift) window.dispatch_key_release_event(slint::platform::key_codes::Shift);
    window.dispatch_key_release_event(slint::platform::key_codes::Control);
}
// The sample network again, as if just opened, without asking.
void fresh(resend::AppController& app) { app.open(netsim::sample_lan()); }
std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string cell(const std::shared_ptr<slint::Model<std::shared_ptr<slint::Model<slint::StandardListViewItem>>>>& rows,
                 std::size_t row, std::size_t column) {
    if (row >= rows->row_count()) return {};
    const auto cells = *rows->row_data(row);
    if (column >= cells->row_count()) return {};
    return std::string(cells->row_data(column)->text);
}
std::string cell(const Ui& ui, std::size_t row, std::size_t column) { return cell(ui->global<UiInspector>().get_table_rows(), row, column); }
// A row of the selected router's routing table, its cells joined by " | ".
std::string route_row(const Ui& ui, std::size_t row) {
    const auto rows = ui->global<UiInspector>().get_routing_rows();
    std::string text;
    for (std::size_t column = 0; column < 4; ++column) text += (column ? " | " : "") + cell(rows, row, column);
    return text;
}

void initial_view_shows_whole_network(const Ui& ui) {
    const auto canvas = element(ui, "Network topology");
    CHECK(inside(element(ui, "PC-A, 192.168.1.10/24"), canvas));
    CHECK(inside(element(ui, "PC-B, 192.168.1.20/24"), canvas));
    CHECK(inside(element(ui, "Switch-01, 8 Ethernet ports"), canvas));
    CHECK(shown(ui, "Sample LAN"));
    // The first host is selected, ready to ping the other one.
    CHECK(ui->global<UiInspector>().get_kind() == slint::SharedString("host"));
    CHECK(element(ui, "Destination IPv4").accessible_value() == slint::SharedString("192.168.1.20"));
    CHECK(!ui->global<UiApp>().get_can_undo() && !ui->global<UiApp>().get_modified());
}

void drag_keyboard_and_undo(const Ui& ui, const resend::AppController& app) {
    const auto before = *app.scenario().device(DeviceId{1});
    auto from = center(element(ui, "PC-A, 192.168.1.10/24"));
    from.y -= 20;  // away from the port light and the connector at the bottom
    drag(ui, from, slint::LogicalPosition({from.x + 50, from.y + 30}));
    const auto after = *app.scenario().device(DeviceId{1});
    const int dx = after.x - before.x, dy = after.y - before.y;
    CHECK(dx > 0 && dy > 0);
    // Same direction as the pointer, whatever the zoom; positions are whole
    // canvas units, so each may be off by half a unit.
    CHECK(std::abs(dx * 30 - dy * 50) <= 40);
    // The drag is one step, and the network is modified.
    CHECK(ui->global<UiApp>().get_undo_text() == slint::SharedString("Undo Move PC-A"));
    CHECK(ui->global<UiApp>().get_modified());
    CHECK(shown(ui, "Sample LAN, not saved"));

    // The press gave the canvas the keyboard: arrows move the selected card,
    // by 10 units, or by 1 with Shift.
    key(ui, slint::platform::key_codes::RightArrow);
    CHECK(app.scenario().device(DeviceId{1})->x == after.x + 10);
    ui->window().dispatch_key_press_event(slint::platform::key_codes::Shift);
    key(ui, slint::platform::key_codes::DownArrow);
    ui->window().dispatch_key_release_event(slint::platform::key_codes::Shift);
    CHECK(app.scenario().device(DeviceId{1})->y == after.y + 1);

    // Ctrl+Z three times: back where it started, and saved again.
    for (int i = 0; i < 3; ++i) shortcut(ui, "z");
    CHECK(app.scenario().device(DeviceId{1})->x == before.x && app.scenario().device(DeviceId{1})->y == before.y);
    CHECK(!ui->global<UiApp>().get_can_undo() && !ui->global<UiApp>().get_modified());
    // Ctrl+Shift+Z and Ctrl+Y redo; the redo button names what it redoes.
    shortcut(ui, "z", true);
    shortcut(ui, "y");
    CHECK(app.scenario().device(DeviceId{1})->x == after.x + 10);
    CHECK(ui->global<UiApp>().get_redo_text() == slint::SharedString("Redo Move PC-A"));
    press(ui, "Redo Move PC-A");
    CHECK(app.scenario().device(DeviceId{1})->y == after.y + 1);
    CHECK(status(ui) == "Redo Move PC-A.");
    press(ui, "Undo Move PC-A");
    CHECK(app.scenario().device(DeviceId{1})->y == after.y);
}

void plug_cables(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "Add Host");
    CHECK(app.scenario().devices.size() == 4);
    // A new card appears in view, selected: clipped controls are not found.
    CHECK(inside(element(ui, "PC-4, 192.168.1.13/24"), element(ui, "Network topology")));
    CHECK(ui->global<UiInspector>().get_heading() == slint::SharedString("PC-4"));
    CHECK(ui->global<UiApp>().get_undo_text() == slint::SharedString("Undo Add a host"));

    // The + button starts a cable; on another device it ends it on the first free port.
    press(ui, "Connect PC-4");
    CHECK(ui->global<UiCanvas>().get_source_device() == 4);
    press(ui, "Connect Switch-01");
    CHECK(app.scenario().links.size() == 3);
    CHECK(exists(ui, "Cable PC-4 port 1 — Switch-01 port 3"));
    CHECK(ui->global<UiCanvas>().get_source_device() == -1);
    // On the same device it cancels.
    press(ui, "Connect Switch-01");
    press(ui, "Connect Switch-01");
    CHECK(app.scenario().links.size() == 3 && status(ui) == "Connection cancelled.");

    // With the pointer: from a card's + to another card.
    press(ui, "Add Host");
    const auto connector = center(element(ui, "Connect PC-5"));
    drag(ui, connector, center(element(ui, "Switch-01, 8 Ethernet ports")));
    CHECK(app.scenario().links.size() == 4);
    CHECK(exists(ui, "Cable PC-5 port 1 — Switch-01 port 4"));
    // A host has one port: a second cable is refused, and says why.
    press(ui, "Connect PC-5");
    press(ui, "Connect PC-A");
    CHECK(app.scenario().links.size() == 4);
    CHECK(status(ui) == "No free port: every port of the device has a cable.");
    key(ui, slint::platform::key_codes::Escape);

    // Delete removes the device and its cable, as one step.
    press(ui, "PC-5, 192.168.1.14/24");
    press(ui, "Delete device");
    CHECK(app.scenario().devices.size() == 4 && app.scenario().links.size() == 3);
    press(ui, "Undo Remove PC-5");
    CHECK(app.scenario().devices.size() == 5 && app.scenario().links.size() == 4);
}

void cable_inspector(const Ui& ui, resend::AppController& app) {
    fresh(app);
    const auto& inspector = ui->global<UiInspector>();
    press(ui, "Cable PC-B port 1 — Switch-01 port 2");
    CHECK(inspector.get_kind() == slint::SharedString("cable"));
    CHECK(inspector.get_cable_a() == slint::SharedString("PC-B port 1"));
    CHECK(inspector.get_delay() == slint::SharedString("10ms"));
    type(ui, "Delay", "-3ms");
    press(ui, "Apply the delay");
    CHECK(app.scenario().link(LinkId{2})->delay == 10ms);
    CHECK(status(ui) == "Write the delay as a duration, for example 10ms or 250us.");
    type(ui, "Delay", "25ms");
    press(ui, "Apply the delay");
    CHECK(app.scenario().link(LinkId{2})->delay == 25ms);
    CHECK(status(ui) == "Set the delay of cable PC-B port 1 — Switch-01 port 2.");

    press(ui, "Disable cable");
    CHECK(!app.scenario().link(LinkId{2})->up);
    press(ui, "Enable cable");
    CHECK(app.scenario().link(LinkId{2})->up);
    press(ui, "Unplug cable");
    CHECK(!app.scenario().link(LinkId{2}) && inspector.get_kind() == slint::SharedString(""));
    CHECK(!exists(ui, "Cable PC-B port 1 — Switch-01 port 2"));
    shortcut(ui, "z");
    CHECK(app.scenario().link(LinkId{2}) && exists(ui, "Cable PC-B port 1 — Switch-01 port 2"));

    // A port's cable is one click away from the device.
    press(ui, "PC-B, 192.168.1.20/24");
    press(ui, "Select the cable of Port 1");
    CHECK(inspector.get_kind() == slint::SharedString("cable") && inspector.get_cable_a() == slint::SharedString("PC-B port 1"));
}

void device_inspector(const Ui& ui, resend::AppController& app) {
    fresh(app);
    const auto& inspector = ui->global<UiInspector>();
    press(ui, "PC-A, 192.168.1.10/24");
    type(ui, "IPv4 address", "invalid");
    press(ui, "Apply the configuration");
    CHECK(app.scenario().device(DeviceId{1})->ports[0].ipv4->to_string() == "192.168.1.10/24");
    CHECK(status(ui).starts_with("Invalid IPv4 host address"));
    type(ui, "IPv4 address", "192.168.1.10");
    type(ui, "Prefix length", "33");
    press(ui, "Apply the configuration");
    CHECK(status(ui) == "IPv4 prefixes go from /1 to /32.");
    // A name and an address in one step.
    type(ui, "Prefix length", "24");
    type(ui, "Name", "Laptop");
    type(ui, "IPv4 address", "192.168.1.30");
    press(ui, "Apply the configuration");
    CHECK(app.scenario().device(DeviceId{1})->name == "Laptop");
    CHECK(exists(ui, "Laptop, 192.168.1.30/24"));
    CHECK(status(ui) == "Configure PC-A.");
    press(ui, "Undo Configure PC-A");
    CHECK(exists(ui, "PC-A, 192.168.1.10/24") && inspector.get_name() == slint::SharedString("PC-A"));

    // Ports, power and the switch's ageing time.
    press(ui, "Port 1");
    CHECK(!app.scenario().device(DeviceId{1})->ports[0].admin_up);
    press(ui, "Port 1");
    CHECK(app.scenario().device(DeviceId{1})->ports[0].admin_up);
    press(ui, "Power off");
    CHECK(!app.scenario().device(DeviceId{1})->powered && exists(ui, "Power on"));
    press(ui, "Power on");
    press(ui, "Switch-01, 8 Ethernet ports");
    CHECK(inspector.get_kind() == slint::SharedString("switch") && inspector.get_ageing() == slint::SharedString("300s"));
    CHECK(inspector.get_ports()->row_count() == 8);
    type(ui, "MAC ageing time", "soon");
    press(ui, "Apply the configuration");
    CHECK(status(ui) == "Write the ageing time as a duration, for example 300s.");
    type(ui, "MAC ageing time", "45s");
    press(ui, "Apply the configuration");
    CHECK(app.scenario().device(DeviceId{3})->mac_ageing == 45s);
    // Apply with nothing new says so.
    press(ui, "Apply the configuration");
    CHECK(status(ui) == "Nothing to change.");
}

void ping_and_tables(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    CHECK(app.simulation().pending());
    run_to_end(ui, app);
    CHECK(succeeded(app));
    CHECK(ui->global<UiTimeline>().get_total() == 9);
    // The list follows the latest event: its row is entirely in view.
    CHECK(newest(ui, "ICMP: Ping succeeded · 192.168.1.20 → PC-A"));
    CHECK(inside(element(ui, "ICMP: Ping succeeded · 192.168.1.20 → PC-A"), element(ui, "Event list")));
    CHECK(ui->global<UiTimeline>().get_title() == slint::SharedString("Ping succeeded · 192.168.1.20 → PC-A"));
    // The tables at the instant shown: PC-A knows PC-B, the switch both.
    CHECK(cell(ui, 0, 0) == "192.168.1.20" && cell(ui, 0, 1) == "02:00:00:00:00:02" && cell(ui, 0, 2) == "reachable");
    press(ui, "Switch-01, 8 Ethernet ports");
    CHECK(ui->global<UiInspector>().get_table_rows()->row_count() == 2);
    CHECK(cell(ui, 0, 0) == "02:00:00:00:00:01" && cell(ui, 0, 1) == "1");
}

void frame_fields_and_bytes(const Ui& ui, resend::AppController& app) {
    // After ping_and_tables: the first event is PC-A's ARP request, scrolled
    // out of the list, which only makes its visible rows.
    CHECK(!exists(ui, "ARP: PC-A → Switch-01 · ARP Request"));
    ui->global<UiTimeline>().invoke_select(0);
    CHECK(!ui->global<UiTimeline>().get_follow());
    ui->global<UiTimeline>().set_tab(1);
    const auto& frame = ui->global<UiFrame>();
    // 42 bytes of headers, padded to Ethernet's minimum of 60.
    CHECK(frame.get_title() == slint::SharedString("ARP Request · 60 bytes"));
    CHECK(frame.get_hex()->row_count() == 4);
    // Layers as headers, then their fields with value and meaning.
    const auto rows = frame.get_rows();
    CHECK(rows->row_data(0)->header && rows->row_data(0)->label == slint::SharedString("Ethernet II"));
    press(ui, "Destination: ff:ff:ff:ff:ff:ff");
    CHECK(frame.get_selected_offset() == 0 && frame.get_selected_length() == 6);
    CHECK(std::string_view(frame.get_selection()).starts_with("Destination: ff:ff:ff:ff:ff:ff"));
    // A byte selects the smallest field that holds it.
    frame.invoke_select_byte(12);
    CHECK(frame.get_selected_offset() == 12 && frame.get_selected_length() == 2);
    CHECK(std::string_view(frame.get_selection()).ends_with("bytes 12–13"));
    // Every frame of the run decodes to rows.
    for (int index = 0; index < ui->global<UiTimeline>().get_total(); ++index) {
        ui->global<UiTimeline>().invoke_select(index);
        if (app.explainer().events()[static_cast<std::size_t>(index)].pdu) CHECK(frame.get_rows()->row_count() > 4);
    }
    ui->global<UiTimeline>().set_tab(0);
}

void timeline_filters(const Ui& ui, resend::AppController& app) {
    // After ping_and_tables: 9 events, 5 of them ARP.
    const auto& timeline = ui->global<UiTimeline>();
    const auto listed = [&] { return timeline.get_events()->row_count(); };
    const auto all = [&](std::string_view protocol) {
        const auto events = timeline.get_events();
        for (std::size_t row = 0; row < events->row_count(); ++row)
            if (events->row_data(row)->protocol != slint::SharedString(protocol)) return false;
        return true;
    };
    CHECK(listed() == 9);
    chip(ui, "ARP").invoke_accessible_default_action();
    CHECK(!timeline.get_arp() && timeline.get_filtered() && all("ICMP") && listed() == 4);
    chip(ui, "ARP").invoke_accessible_default_action();
    chip(ui, "ICMP").invoke_accessible_default_action();
    CHECK(all("ARP") && listed() == 5);
    chip(ui, "ICMP").invoke_accessible_default_action();
    CHECK(!timeline.get_filtered() && listed() == 9);
    // Only problems: none in a ping that succeeded.
    chip(ui, "Problems").invoke_accessible_default_action();
    CHECK(listed() == 0);
    chip(ui, "Problems").invoke_accessible_default_action();
    // The selected device: PC-B takes part in fewer events than the whole run.
    press(ui, "PC-B, 192.168.1.20/24");
    chip(ui, "Selected device").invoke_accessible_default_action();
    CHECK(timeline.get_device_only() && timeline.get_device_name() == slint::SharedString("PC-B"));
    CHECK(listed() > 0 && listed() < 9);
    chip(ui, "PC-B").invoke_accessible_default_action();
    CHECK(listed() == 9);
    // Without a selection the filter says what it needs.
    app.open(netsim::sample_lan());
    press(ui, "Fit the network in view");
    ui->global<UiCanvas>().invoke_press(4, 4, 0);  // empty canvas: nothing selected
    ui->global<UiCanvas>().invoke_release(4, 4);
    chip(ui, "Selected device").invoke_accessible_default_action();
    CHECK(!timeline.get_device_only() && status(ui) == "Select a device to see only its events.");
}

void scrubber_travels_and_locks(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    play_to_end(ui, app);
    const auto& timeline = ui->global<UiTimeline>();
    // The ping ends at 80 ms; the afterglow shows 12 ms more.
    CHECK(timeline.get_end_ms() == 92.0f && timeline.get_now_ms() == 92.0f);
    // A change after the run is recorded at its instant.
    press(ui, "PC-B, 192.168.1.20/24");
    press(ui, "Power off");
    CHECK(status(ui) == "Power off PC-B: applied at t = 92.000 ms.");
    CHECK(newest(ui, "EDIT: PC-B powered off"));

    // Back to 40 ms: the run is replayed up to there.
    timeline.invoke_scrub(40.0f);
    CHECK(clock(ui) == "40.000 ms" && timeline.get_now_ms() == 40.0f);
    CHECK(timeline.get_total() == 5 && timeline.get_locked());  // the ARP exchange
    CHECK(!listed(ui, "EDIT: PC-B powered off"));
    // The power-off lies ahead: PC-B is on, and changes are refused.
    CHECK(app.scenario().device(DeviceId{2})->powered);
    // The ping and the power-off are commands; the second is not made yet.
    CHECK(app.simulation().edits().empty() && app.workspace().commands().size() == 2);
    press(ui, "Power off");
    CHECK(app.simulation().edits().empty() && app.workspace().commands().size() == 2);
    CHECK(status(ui) == "The instant shown is before later changes: go to the latest instant to change the network.");
    press(ui, "Go to the latest instant");
    CHECK(!timeline.get_locked() && clock(ui) == "92.000 ms" && !app.scenario().device(DeviceId{2})->powered);
    CHECK(newest(ui, "EDIT: PC-B powered off"));
    // The slider moves the instant too.
    element(ui, "Instant shown").set_accessible_value("20");
    CHECK(clock(ui) == "20.000 ms");
    press(ui, "Go to the latest instant");
}

void drops_and_playback_controls(const Ui& ui, resend::AppController& app, const fs::path& state) {
    fresh(app);
    press(ui, "PC-B, 192.168.1.20/24");
    press(ui, "Power off");
    CHECK(!app.scenario().device(DeviceId{2})->powered);
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    app.seek(20ms);
    CHECK(shown(ui, "× device off"));

    const auto& run = ui->global<UiRun>();
    press(ui, "Playback speed");
    CHECK(run.get_speed_label() == slint::SharedString("2×"));
    for (int i = 0; i < 4; ++i) press(ui, "Playback speed");
    CHECK(run.get_speed_label() == slint::SharedString("1×"));
    // Reduced effects, from the HUD's button: a step jumps straight to the
    // next event. The choice is kept in the settings.
    CHECK(ui->global<Motion>().get_effects() == slint::SharedString("full"));
    press(ui, "Effects: full");
    CHECK(ui->global<Motion>().get_effects() == slint::SharedString("reduced") && resend::load_settings(state).effects == "reduced");
    CHECK(exists(ui, "Effects: reduced"));
    press(ui, "Step");
    CHECK(!running(ui) && clock(ui) == "1000.000 ms");
    CHECK(shown(ui, "ARP timeout"));
    // Off, then full again: the button goes round.
    press(ui, "Effects: reduced");
    CHECK(ui->global<Motion>().get_effects() == slint::SharedString("off") && resend::load_settings(state).effects == "off");
    CHECK(status(ui) == "Effects off: events appear one instant at a time, with their markers and texts only.");
    press(ui, "Effects: off");
    CHECK(ui->global<Motion>().get_effects() == slint::SharedString("full") && resend::load_settings(state).effects == "full");
    press(ui, "Reset the simulation");
    CHECK(clock(ui) == "0.000 ms" && !app.simulation().pending());
    CHECK(status(ui) == "Caches, tables and events cleared. The topology stays as it is.");
}

// G5: the HUD's button and M turn the sound on and off, the menu sets its
// level, all kept in the settings; a ping is heard to its resolve, and
// what is heard never changes what happens.
void sound_on_and_off(const Ui& ui, resend::AppController& app, const fs::path& state) {
    fresh(app);
    const auto& sound = ui->global<UiSound>();
    CHECK(sound.get_on() && app.settings().sound && !app.sound_output().audible());
    CHECK(sound.get_device() == slint::SharedString("none in this session"));
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    const auto before = app.played_count();
    play_to_end(ui, app);
    CHECK(succeeded(app));
    const auto& heard = app.played();
    const auto count = static_cast<std::size_t>(app.played_count() - before);
    CHECK(count > 8 && count <= heard.size());
    std::vector<resend::CueKind> kinds;
    for (auto at = heard.end() - static_cast<std::ptrdiff_t>(count); at != heard.end(); ++at)
        if (at->kind != resend::CueKind::Click) kinds.push_back(at->kind);
    // The request, the hops, the reply, and the resolve last.
    CHECK(kinds.front() == resend::CueKind::Ask && kinds.back() == resend::CueKind::Resolve);
    CHECK(std::count(kinds.begin(), kinds.end(), resend::CueKind::Ask) == 1);
    CHECK(std::count(kinds.begin(), kinds.end(), resend::CueKind::Answer) == 1);
    const auto records = app.simulation().trace().records();

    // Off from the HUD: nothing more is heard, the same ping runs the same.
    press(ui, "Sound: on");
    CHECK(!sound.get_on() && !resend::load_settings(state).sound && exists(ui, "Sound: off"));
    CHECK(status(ui) == "Sound off.");
    const auto quiet = app.played_count();
    press(ui, "Reset the simulation");
    press(ui, "Send the ping");
    play_to_end(ui, app);
    CHECK(app.played_count() == quiet);
    CHECK(app.simulation().trace().records() == records);
    // Scrubbing back and replaying in silence, then on with M: a jump is
    // never heard, only what plays.
    key(ui, "m");
    CHECK(sound.get_on() && resend::load_settings(state).sound);
    const auto on = app.played_count();
    app.seek(20ms);
    ui->global<UiRun>().invoke_latest();
    CHECK(app.played_count() == on && clock(ui) == "92.000 ms");
    // The level, from the menu or the palette.
    ui->global<UiSound>().invoke_set_sound("quiet");
    CHECK(sound.get_level() == slint::SharedString("quiet") && resend::load_settings(state).volume == "quiet");
    ui->global<UiApp>().invoke_run_command("sound:loud");
    CHECK(sound.get_level() == slint::SharedString("loud") && resend::load_settings(state).volume == "loud");
    ui->global<UiApp>().invoke_run_command("sound:off");
    CHECK(!sound.get_on());
    ui->global<UiApp>().invoke_run_command("sound:medium");  // a level turns it back on
    CHECK(sound.get_on() && sound.get_level() == slint::SharedString("medium"));
}

void packets_move_on_the_canvas(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    CHECK(!exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    press(ui, "Step");  // the request is due at once: it appears at the port
    CHECK(exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    CHECK(clock(ui) == "0.000 ms");
    press(ui, "Step");
    CHECK(running(ui));
    app.advance_playback(300ms);  // halfway along the cable
    CHECK(clock(ui) == "5.000 ms");
    CHECK(app.explainer().events().size() == 1);  // nothing arrived yet
    app.advance_playback(300ms);
    CHECK(!running(ui));
    // The switch's decisions appear on the switch, and the flooded copy leaves.
    CHECK(shown(ui, "learn PC-A → p1"));
    CHECK(shown(ui, "flood ×1"));
    CHECK(!exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    CHECK(exists(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-B"));
    // Selecting a packet shows its frame, field by field.
    press(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-B");
    CHECK(ui->global<UiTimeline>().get_tab() == 1 && !ui->global<UiTimeline>().get_follow());
    CHECK(ui->global<UiFrame>().get_title() == slint::SharedString("ARP Request · 60 bytes"));
    CHECK(exists(ui, "Destination: ff:ff:ff:ff:ff:ff"));
    ui->global<UiTimeline>().set_tab(0);
}

void flood_copies_travel_together(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "Add Host");
    press(ui, "Connect PC-4");
    press(ui, "Connect Switch-01");
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    app.seek(15ms);
    CHECK(exists(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-B"));
    CHECK(exists(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-4"));
    CHECK(shown(ui, "flood ×2"));
    play_to_end(ui, app);
    CHECK(succeeded(app));
    CHECK(shown(ui, "✓ ping reply"));
}

void pings_overlap_and_count(const Ui& ui, resend::AppController& app) {
    // A second ping can start while the first is on its way.
    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    app.seek(25ms);
    press(ui, "PC-B, 192.168.1.20/24");
    type(ui, "Destination IPv4", "192.168.1.10");
    press(ui, "Send the ping");
    CHECK(app.simulation().pings().size() == 2 && app.simulation().pings()[1].started == 25ms);
    play_to_end(ui, app);
    CHECK(succeeded(app));
    CHECK(status(ui).starts_with("Every ping succeeded"));
    // A refused ping says why.
    type(ui, "Destination IPv4", "192.168.1.255");
    press(ui, "Send the ping");
    CHECK(status(ui) == "Pinging network or broadcast addresses is not supported.");

    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    for (const auto* refused : {"0", "1001", "3x"}) {
        type(ui, "Echo Requests per ping", refused);
        press(ui, "Send the ping");
        CHECK(app.simulation().pings().empty());
        CHECK(status(ui) == "Send 1 to 1000 Echo Requests per ping.");
    }
    type(ui, "Echo Requests per ping", "3");
    press(ui, "Send the ping");
    play_to_end(ui, app);
    CHECK(app.simulation().pings().size() == 1 && app.simulation().pings()[0].replies() == 3);
    CHECK(listed(ui, "ICMP: Reply 2/3 · 192.168.1.20 → PC-A"));
    CHECK(shown(ui, "✓ ping 3/3"));
    type(ui, "Echo Requests per ping", "1");
}

void live_edits(const Ui& ui, resend::AppController& app) {
    // Before anything happens, an edit simply gives another network.
    fresh(app);
    press(ui, "Cable PC-B port 1 — Switch-01 port 2");
    press(ui, "Disable cable");
    CHECK(!app.scenario().link(LinkId{2})->up && app.simulation().edits().empty() && ui->global<UiTimeline>().get_total() == 0);
    CHECK(status(ui) == "Disable cable PC-B port 1 — Switch-01 port 2.");
    press(ui, "Enable cable");

    // While a ping runs, the edit happens at the instant on screen.
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send the ping");
    app.seek(15ms);
    press(ui, "PC-B, 192.168.1.20/24");
    press(ui, "Power off");
    CHECK(app.simulation().edits().size() == 1 && app.simulation().edits()[0].time == 15ms);
    CHECK(status(ui) == "Power off PC-B: applied at t = 15.000 ms.");
    CHECK(newest(ui, "EDIT: PC-B powered off"));
    CHECK(shown(ui, "powered off"));
    app.seek(20ms);
    CHECK(shown(ui, "× device off"));
    play_to_end(ui, app);
    CHECK(!succeeded(app));
    // Undo while running applies the inverse edit now; PC-B's tables went with the power.
    press(ui, "Undo Power off PC-B");
    CHECK(app.scenario().device(DeviceId{2})->powered && app.simulation().edits().size() == 2);
    CHECK(app.simulation().arp_cache(DeviceId{2}).empty());
}

void storm_pauses_playback(const Ui& ui, resend::AppController& app) {
    // The network was changed: opening an example asks first.
    ui->global<UiApp>().invoke_open_example(5);
    CHECK(dialog(ui) == "confirm");
    press(ui, "Don't save");
    CHECK(shown(ui, "Switch loop") && !ui->global<UiApp>().get_modified());
    type(ui, "Destination IPv4", "192.168.1.20");
    press(ui, "Send the ping");
    press(ui, "Run");
    for (int frame = 0; frame < 2000 && running(ui); ++frame) app.advance_playback(100ms);
    // Paused at the guard, with the network still running.
    CHECK(!running(ui) && app.simulation().pending());
    CHECK(status(ui).starts_with("Storm protection:"));
    CHECK(newest(ui, "SIMULATOR: Storm protection · frame stopped at Switch-2"));
    CHECK(shown(ui, "storm stopped"));
    // Run again: it pauses at each of the three guards.
    int pauses = 1;
    while (app.simulation().pending() && pauses < 10) {
        press(ui, "Run");
        for (int frame = 0; frame < 2000 && running(ui); ++frame) app.advance_playback(100ms);
        if (app.simulation().pending()) ++pauses;
    }
    CHECK(!app.simulation().pending() && pauses == 3);
}

void examples_from_the_menu(const Ui& ui, resend::AppController& app) {
    const auto examples = ui->global<UiApp>().get_examples();
    CHECK(examples->row_count() == 11);
    CHECK(examples->row_data(6)->title == slint::SharedString("Two LANs and a router"));
    CHECK(examples->row_data(0)->title == slint::SharedString("Sample LAN"));
    CHECK(examples->row_data(2)->title == slint::SharedString("Office floor"));
    ui->global<UiApp>().invoke_open_example(2);
    CHECK(status(ui) == "Office floor opened." && shown(ui, "Office floor"));
    CHECK(ui->global<UiApp>().get_notes() == slint::SharedString(app.scenario().notes.c_str()));
    // The view fits the new network, and the first host is selected.
    const auto canvas = element(ui, "Network topology");
    CHECK(inside(element(ui, "Desk-1, 10.0.0.11/24"), canvas));
    CHECK(inside(element(ui, "Printer-Room, 10.0.0.23/24"), canvas));
    CHECK(ui->global<UiInspector>().get_heading() == slint::SharedString("Desk-1"));
    type(ui, "Destination IPv4", "10.0.0.22");
    press(ui, "Send the ping");
    play_to_end(ui, app);
    CHECK(succeeded(app));
    // Running a network does not change it: no question when leaving.
    CHECK(!ui->global<UiApp>().get_modified());
    ui->global<UiApp>().invoke_open_example(0);
    CHECK(dialog(ui).empty() && shown(ui, "Sample LAN"));
}

void routers(const Ui& ui, resend::AppController& app) {
    // The first router example: PC-A selected, PC-B on the other LAN as the destination.
    ui->global<UiApp>().invoke_open_example(6);
    CHECK(status(ui) == "Two LANs and a router opened.");
    const auto canvas = element(ui, "Network topology");
    CHECK(inside(element(ui, "Router, 10.1.0.1/24, 10.2.0.1/24"), canvas));
    CHECK(element(ui, "Destination IPv4").accessible_value() == slint::SharedString("10.2.0.20"));
    press(ui, "Send the ping");
    play_to_end(ui, app);
    CHECK(succeeded(app) && listed(ui, "ICMP: Router → Switch-2 · Echo Request"));

    // A traceroute from the run bar: the router, then PC-B.
    chip(ui, "Traceroute").invoke_accessible_default_action();
    CHECK(ui->global<UiRun>().get_traceroute() && exists(ui, "Hops to try") && !exists(ui, "Echo Requests per ping"));
    type(ui, "Hops to try", "0");
    press(ui, "Start the traceroute");
    CHECK(status(ui) == "A traceroute tries 1 to 255 hops.");
    type(ui, "Hops to try", "5");
    press(ui, "Start the traceroute");
    CHECK(status(ui) == "Traceroute ready. Press Step or Run to advance simulated time.");
    play_to_end(ui, app);
    const auto& trace = app.simulation().pings().back();
    CHECK(trace.options.traceroute && trace.options.count == 5 && trace.state == netsim::PingState::Succeeded && trace.echoes.size() == 2);
    CHECK(listed(ui, "ICMP: Hop 1 · 10.1.0.1") && listed(ui, "ICMP: Hop 2 · 10.2.0.20 reached"));
    chip(ui, "Traceroute").invoke_accessible_default_action();
    CHECK(!ui->global<UiRun>().get_traceroute() && exists(ui, "Echo Requests per ping"));

    // The router's inspector: its ports, routes and routing table.
    // It is long: only what is in view can be found, so the window grows.
    press(ui, "Router, 10.1.0.1/24, 10.2.0.1/24");
    ui->window().set_size(slint::LogicalSize({1100, 1800}));
    settle(ui);
    const auto& inspector = ui->global<UiInspector>();
    CHECK(inspector.get_kind() == slint::SharedString("router") && inspector.get_port_count() == 2);
    CHECK(inspector.get_routing_rows()->row_count() == 2);
    CHECK(route_row(ui, 0) == "10.1.0.0/24 | connected | 1 | active" && route_row(ui, 1) == "10.2.0.0/24 | connected | 2 | active");
    CHECK(cell(ui, 0, 0) == "10.1.0.10" && cell(ui, 0, 2) == "1" && cell(ui, 1, 0) == "10.2.0.20" && cell(ui, 1, 2) == "2");
    // A route through a next hop on no connected network stays unused.
    type(ui, "Network", "10.9.0.0/16");
    type(ui, "Next hop", "10.3.0.2");
    press(ui, "Add the static route");
    CHECK(app.scenario().device(DeviceId{3})->routes.size() == 1);
    CHECK(route_row(ui, 2) == "10.9.0.0/16 | 10.3.0.2 | — | not used");
    CHECK(inspector.get_routes()->row_data(0)->note == slint::SharedString("Not used: the next hop is on none of the router's networks."));
    CHECK(inspector.get_route_destination().empty() && inspector.get_route_via().empty());
    type(ui, "Network", "10.9.0.1/16");
    type(ui, "Next hop", "10.2.0.30");
    press(ui, "Add the static route");
    CHECK(status(ui) == "A route goes to a network, with no host bits set, through a unicast next hop; only routers have routes.");
    type(ui, "Network", "nowhere");
    press(ui, "Add the static route");
    CHECK(status(ui).starts_with("Write the network with its prefix length"));
    // A third port with an address makes the route usable.
    press(ui, "Add a port");
    CHECK(inspector.get_port_count() == 3 && app.scenario().device(DeviceId{3})->ports.size() == 3);
    type(ui, "IPv4 address of Port 3", "10.3.0.1/24");
    press(ui, "Apply the configuration");
    CHECK(app.scenario().device(DeviceId{3})->ports[2].ipv4 == netsim::Ipv4Prefix::parse("10.3.0.1/24"));
    CHECK(route_row(ui, 2) == "10.3.0.0/24 | connected | 3 | active" && route_row(ui, 3) == "10.9.0.0/16 | 10.3.0.2 | 3 | active");
    type(ui, "IPv4 address of Port 3", "10.3.0.1");
    press(ui, "Apply the configuration");
    CHECK(status(ui) == "Write the address of port 3 with its prefix length, for example 10.1.0.1/24.");
    press(ui, "Remove the route to 10.9.0.0/16");
    CHECK(app.scenario().device(DeviceId{3})->routes.empty() && inspector.get_routing_rows()->row_count() == 3);
    press(ui, "Remove the last port");
    CHECK(inspector.get_port_count() == 2);
    // Two is the minimum, so "−" is off; and port 2 has a cable.
    inspector.invoke_set_port_count(1);
    CHECK(inspector.get_port_count() == 2 && status(ui) == "Port 2 has a cable: unplug it first.");

    // Without a gateway PC-A reaches only its own subnet.
    press(ui, "PC-A, 10.1.0.10/24");
    CHECK(inspector.get_gateway() == slint::SharedString("10.1.0.1"));
    type(ui, "Default gateway", "10.1.0");
    press(ui, "Apply the configuration");
    CHECK(status(ui).starts_with("Write the default gateway as an IPv4 address"));
    type(ui, "Default gateway", "");
    press(ui, "Apply the configuration");
    CHECK(!app.scenario().device(DeviceId{1})->gateway);
    press(ui, "Send the ping");
    play_to_end(ui, app);
    CHECK(app.simulation().pings().back().state == netsim::PingState::Failed && listed(ui, "IPv4: Ping failed · no route"));
    shortcut(ui, "z");
    CHECK(app.scenario().device(DeviceId{1})->gateway == netsim::Ipv4Address::parse("10.1.0.1"));

    // A router from the palette: four ports, no addresses yet.
    press(ui, "Add Router");
    const auto& added = app.scenario().devices.back();
    CHECK(added.kind == netsim::DeviceKind::Router && added.ports.size() == 4 && inspector.get_kind() == slint::SharedString("router"));
    CHECK(exists(ui, added.name + ", no IPv4 address"));
    ui->window().set_size(slint::LogicalSize({1100, 760}));
    fresh(app);
}

void save_and_reopen(const Ui& ui, resend::AppController& app, const fs::path& home) {
    fresh(app);
    press(ui, "Add Switch");
    CHECK(ui->global<UiApp>().get_modified());
    // Ctrl+S on a network without a file asks where, in the home folder.
    shortcut(ui, "s");
    CHECK(dialog(ui) == "save" && exists(ui, "Save the network"));
    CHECK(ui->global<UiApp>().get_file_name() == slint::SharedString("sample-lan.json"));
    CHECK(fs::equivalent(resend::path_from_text(std::string(ui->global<UiApp>().get_folder())), home));
    press(ui, "Save");
    const auto saved = home / "sample-lan.json";
    CHECK(dialog(ui).empty() && fs::exists(saved));
    CHECK(!ui->global<UiApp>().get_modified() && app.document() == saved);
    CHECK(status(ui) == "Saved sample-lan.json." && shown(ui, "sample-lan.json"));

    // Reopened and saved again, the file is the same, byte for byte.
    const auto first = read_text(saved);
    CHECK(app.open_file(saved));
    CHECK(app.scenario().devices.size() == 4);
    press(ui, "PC-A, 192.168.1.10/24");
    key(ui, slint::platform::key_codes::RightArrow);
    key(ui, slint::platform::key_codes::LeftArrow);
    // Moved there and back: changed, but the same network.
    CHECK(ui->global<UiApp>().get_modified() && app.scenario().device(DeviceId{1})->x == 100);
    fs::remove(saved);
    shortcut(ui, "s");
    CHECK(dialog(ui).empty() && read_text(saved) == first);

    // Save as an existing name asks first; Cancel goes back to the dialog.
    shortcut(ui, "s", true);
    CHECK(dialog(ui) == "save");
    press(ui, "Save");
    CHECK(dialog(ui) == "confirm" && exists(ui, "Replace sample-lan.json?"));
    press(ui, "Cancel");
    CHECK(dialog(ui) == "save");
    type(ui, "File name", "copy.json");
    press(ui, "Save");
    CHECK(dialog(ui).empty() && fs::exists(home / "copy.json") && app.document() == home / "copy.json");
    // A name with a folder in it is refused.
    ui->global<UiApp>().invoke_save_as();
    type(ui, "File name", "sub/x.json");
    press(ui, "Save");
    CHECK(dialog(ui) == "save" && ui->global<UiApp>().get_browser_error() == slint::SharedString("Type a name for the file, without folders."));
    press(ui, "Cancel");
    CHECK(dialog(ui).empty());

    // Open: the dialog lists networks; a broken one says why and stays open.
    {
        std::ofstream broken(home / "broken.json");
        broken << "{ not json";
    }
    ui->global<UiApp>().invoke_open_document();
    CHECK(dialog(ui) == "open" && exists(ui, "copy.json") && exists(ui, "sample-lan.json") && exists(ui, "broken.json"));
    press(ui, "broken.json");
    CHECK(dialog(ui) == "open" && std::string_view(ui->global<UiApp>().get_browser_error()).starts_with("broken.json cannot be opened"));
    press(ui, "sample-lan.json");
    CHECK(dialog(ui).empty() && app.document() == saved && status(ui) == "sample-lan.json opened.");
    fs::remove(home / "broken.json");
}

void unsaved_changes_are_asked_about(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "Add Host");
    // New: Cancel keeps everything, Don't save drops the changes.
    ui->global<UiApp>().invoke_new_document();
    CHECK(dialog(ui) == "confirm" && exists(ui, "Save the changes to Sample LAN?"));
    press(ui, "Cancel");
    CHECK(dialog(ui).empty() && app.scenario().devices.size() == 4);
    ui->global<UiApp>().invoke_new_document();
    press(ui, "Don't save");
    CHECK(app.scenario().devices.empty() && shown(ui, "Untitled"));
    CHECK(shown(ui, "A network starts\nwith a connection."));
    // Closing the window asks too, and stays open on Cancel.
    press(ui, "Add Host");
    CHECK(!app.close_requested() && dialog(ui) == "confirm");
    press(ui, "Cancel");
    // Save from the question saves first, then goes on.
    ui->global<UiApp>().invoke_open_example(0);
    press(ui, "Save");
    CHECK(dialog(ui) == "save");
    type(ui, "File name", "first.json");
    press(ui, "Save");
    CHECK(dialog(ui).empty() && shown(ui, "Sample LAN") && !ui->global<UiApp>().get_modified());
}

void autosave_and_recovery(const Ui& ui, resend::AppController& app, const fs::path& state) {
    fresh(app);
    const auto files = [&] {
        std::vector<fs::path> found;
        for (const auto& item : fs::directory_iterator(state))
            if (item.path().filename().string().starts_with("autosave-")) found.push_back(item.path());
        return found;
    };
    app.autosave();
    CHECK(files().empty());  // nothing to save
    press(ui, "Add Host");
    app.autosave();
    CHECK(files().size() == 1);
    // A crash: the file stays, under the name of a process that is gone.
    const auto left = state / "autosave-999999999999.json";
    fs::copy_file(files().front(), left);
    shortcut(ui, "z");
    app.autosave();
    CHECK(files().size() == 1 && files().front() == left);

    CHECK(app.offer_recovery());
    CHECK(dialog(ui) == "confirm" && exists(ui, "Recover the unsaved network?"));
    press(ui, "Recover");
    CHECK(app.scenario().devices.size() == 4 && ui->global<UiApp>().get_modified());
    CHECK(shown(ui, "Recovered network, not saved") && status(ui) == "Network recovered. Save it to keep it.");
    CHECK(files().empty() && !app.offer_recovery());
    // Discarded, it is gone too.
    app.autosave();
    fs::copy_file(files().front(), left);
    fresh(app);
    app.autosave();
    CHECK(app.offer_recovery());
    press(ui, "Discard");
    CHECK(files().empty() && app.scenario().devices.size() == 3);
}

// The HUD's parts over the canvas, which lies under all of them; they do
// not overlap, so their areas add up.
double covered(const Ui& ui) {
    double area = 0;
    for (const auto* label : {"Document", "Clock", "Build", "Zoom", "Inspector", "Actions", "Events", "Timeline"})
        for (const auto& item : ElementHandle::find_by_accessible_label(ui, label))
            if (item.accessible_role() == AccessibleRole::Groupbox) area += double(item.size().width) * item.size().height;
    return area;
}

void panels_fold(const Ui& ui) {
    const auto full = covered(ui);
    // The area of the HUD's part with this name.
    const auto part = [&](std::string_view label) {
        double area = 0;
        for (const auto& item : ElementHandle::find_by_accessible_label(ui, label))
            if (item.accessible_role() == AccessibleRole::Groupbox) area += double(item.size().width) * item.size().height;
        return area;
    };
    // The build bar folds to its tab. In a window this low the events,
    // which start beside it, take the room it leaves.
    const auto bar = part("Build");
    press(ui, "Hide the palette");
    CHECK(!ui->global<UiApp>().get_palette_open() && !exists(ui, "Add Host") && part("Build") < bar / 10);
    press(ui, "Hide the inspector");
    CHECK(!ui->global<UiApp>().get_inspector_open() && !exists(ui, "Apply the configuration"));
    CHECK(covered(ui) < full - 50000);
    press(ui, "Hide the timeline");
    CHECK(!exists(ui, "Event list"));
    press(ui, "Show the palette");
    press(ui, "Show the inspector");
    press(ui, "Show the timeline");
    CHECK(exists(ui, "Add Host") && exists(ui, "Event list") && covered(ui) == full);

    // A 1920 × 1080 screen at 2× scale: 960 × 540 logical pixels. With the
    // side panels folded, everything fits and the canvas keeps its room.
    ui->window().set_size(slint::LogicalSize({960, 540}));
    press(ui, "Hide the palette");
    press(ui, "Hide the inspector");
    const auto canvas = element(ui, "Network topology");
    for (const auto* label : {"Send the ping", "Reset the simulation", "Show the inspector", "Go to the latest instant", "Hide the timeline",
                              "Zoom in", "Fit the network in view", "Undo Connect PC-5 port 1 — Switch-01 port 4", "Command palette"}) {
        if (!exists(ui, label)) continue;
        const auto item = element(ui, label);
        const auto at = item.absolute_position();
        CHECK(at.x >= 0 && at.y >= 0 && at.x + item.size().width <= 960 && at.y + item.size().height <= 540);
    }
    const double window = double(canvas.size().width) * canvas.size().height;
    CHECK(canvas.size().width >= 950 && window - covered(ui) >= 0.35 * window);
    press(ui, "Hide the timeline");
    CHECK(window - covered(ui) >= 0.6 * window);
    // H hides the HUD: only the canvas is left.
    ui->global<UiApp>().set_hud_hidden(true);
    CHECK(covered(ui) == 0 && !exists(ui, "Send the ping"));
    ui->global<UiApp>().set_hud_hidden(false);
    press(ui, "Show the timeline");
    press(ui, "Show the palette");
    press(ui, "Show the inspector");
    ui->window().set_size(slint::LogicalSize({1100, 760}));
}

// G2's exit check: at 1360 × 900 with a device selected, at least 55% of
// the window is canvas that no panel covers.
void canvas_is_the_stage(const Ui& ui, resend::AppController& app) {
    fresh(app);
    ui->window().set_size(slint::LogicalSize({1360, 900}));
    ui->global<UiApp>().set_timeline_open(false);
    press(ui, "PC-A, 192.168.1.10/24");
    CHECK(exists(ui, "Apply the configuration"));
    const auto canvas = element(ui, "Network topology");
    const double window = double(canvas.size().width) * canvas.size().height;
    const double free = window - covered(ui);
    std::cout << "  canvas uncovered at 1360 × 900: " << int(100 * free / window) << "%\n";
    CHECK(free >= 0.55 * window);
    ui->global<UiApp>().set_timeline_open(true);
    ui->window().set_size(slint::LogicalSize({1100, 760}));
}

// G1's exit check: a theme changes colours and images, never a size or a
// position.
void themes_move_nothing(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    const auto layout = [&] {
        std::vector<float> numbers;
        ElementHandle::visit_elements(ui, [&](const ElementHandle& item) {
            if (!item.accessible_label() || item.accessible_label()->empty()) return;
            const auto at = item.absolute_position();
            const auto size = item.size();
            numbers.insert(numbers.end(), {at.x, at.y, size.width, size.height});
        });
        return numbers;
    };
    const auto before = layout();
    CHECK(before.size() > 200);
    for (const auto* theme : {"picnic", "bubblegum", "blueberry"}) {
        app.set_theme(theme);
        settle(ui);
        CHECK(ui->global<UiApp>().get_theme() == slint::SharedString(theme));
        CHECK(layout() == before);
    }
    CHECK(app.settings().theme == "blueberry");
    // Every sprite has its image, painted.
    for (const auto name : resend::AppController::sprite_properties()) CHECK(resend::look::find_sprite(name) != nullptr);
    CHECK(resend::AppController::painted_sprites().size() == resend::look::sprites().size());
    CHECK(ui->global<UiSprites>().get_devices()->row_count() == 18 && ui->global<UiSprites>().get_ports()->row_count() == 4);
    CHECK(ui->global<UiSprites>().get_packets()->row_count() == resend::pdu_kinds.size());
    CHECK(ui->global<UiSprites>().get_host().size().width == 32 && ui->global<UiSprites>().get_play().size().width == 16);
}

// G2's exit check: every action of the menus is in the command palette,
// under the same name; and the palette runs them.
void palette_has_every_action(const Ui& ui, resend::AppController& app) {
    fresh(app);
    const auto& run = ui->global<UiApp>();
    run.invoke_show_commands();
    settle(ui);  // the change handlers see the dialog open, as between two frames
    CHECK(run.get_dialog() == slint::SharedString("commands"));
    const auto commands = run.get_commands();
    std::set<std::string> titles;
    for (std::size_t i = 0; i < commands->row_count(); ++i) titles.insert(std::string(commands->row_data(i)->title));
    // The menu items written in ui/app.slint, read from the source.
    const auto source = read_text(fs::path(RESEND_SOURCE_DIR) / "ui" / "app.slint");
    const auto menus = source.substr(source.find("MenuBar {"), source.find("shortcuts := FocusScope") - source.find("MenuBar {"));
    int items = 0;
    for (auto at = menus.find("MenuItem { title: "); at != std::string::npos; at = menus.find("MenuItem { title: ", at + 1)) {
        const auto start = at + 18;
        std::string title;
        if (menus.compare(start, 5, "@tr(\"") == 0) title = menus.substr(start + 5, menus.find('"', start + 5) - start - 5);
        else if (menus[start] == '"') title = menus.substr(start + 1, menus.find('"', start + 1) - start - 1);
        else continue;  // a title computed from the state, checked below
        ++items;
        if (!titles.contains(title)) std::cerr << "  not in the palette: " << title << '\n';
        CHECK(titles.contains(title));
    }
    CHECK(items >= 30);
    for (const auto& computed : {std::string(run.get_undo_text()), std::string(run.get_redo_text()), std::string("Run"),
                                 std::string("Sample LAN"), std::string("Blueberry"), std::string("Picnic")})
        CHECK(titles.contains(computed));
    // Typing filters; Enter runs the first one, here Zoom in.
    type(ui, "Search the actions", "zoom in");
    CHECK(run.get_commands()->row_count() == 1);
    const auto zoom = app.canvas().zoom();
    // The row of the list, not the button of the zoom plate under the dialog.
    const auto list = element(ui, "Matching actions");
    int rows = 0;
    for (const auto& item : controls(ui, "Zoom in"))
        if (inside(item, list)) {
            item.invoke_accessible_default_action();
            ++rows;
        }
    CHECK(rows == 1);
    CHECK(run.get_dialog() == slint::SharedString("") && app.canvas().zoom() > zoom);
    // Ctrl+K opens it again; the key map is on ?.
    shortcut(ui, "k");
    CHECK(run.get_dialog() == slint::SharedString("commands"));
    press(ui, "Close");
    key(ui, "?");
    CHECK(run.get_dialog() == slint::SharedString("keys") && exists(ui, "Keyboard shortcuts"));
    press(ui, "Close");
}

// G2's exit check: no cell of a table is cut, with the longest values the
// model allows.
void tables_cut_nothing(const Ui& ui, resend::AppController& app) {
    fresh(app);
    press(ui, "PC-A, 192.168.1.10/24");
    const auto& inspector = ui->global<UiInspector>();
    inspector.set_tab(1);
    CHECK(exists(ui, "ARP cache"));
    const auto columns = inspector.get_table_columns();
    CHECK(columns->row_count() == 4);
    const std::size_t widest[] = {15, 17};  // 255.255.255.255, ff:ff:ff:ff:ff:ff
    for (std::size_t i = 0; i < 2; ++i) CHECK(static_cast<std::size_t>(columns->row_data(i)->chars) >= widest[i]);
    app.open(netsim::sample_lan());
    ui->global<UiApp>().invoke_open_example(6);  // two LANs and a router
    CHECK(app.select_device("Router"));
    const auto routing = inspector.get_routing_columns();
    CHECK(routing->row_count() == 4 && routing->row_data(0)->chars >= 18);  // 255.255.255.255/32
    // Each cell fits its column, in characters of the monospace.
    const auto rows = inspector.get_routing_rows();
    for (std::size_t r = 0; r < rows->row_count(); ++r)
        for (std::size_t c = 0; c < 4; ++c) {
            const auto text = cell(rows, r, c);
            const auto chars = std::count_if(text.begin(), text.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; });
            CHECK(chars <= routing->row_data(c)->chars);
        }
    inspector.set_tab(0);
}

// G2.4: a right click opens the menu of what is under the pointer, and the
// destination of a ping can be picked on the canvas.
void context_menu(const Ui& ui, resend::AppController& app) {
    fresh(app);
    ui->window().set_size(slint::LogicalSize({1360, 900}));
    settle(ui);
    auto& window = ui->window();
    const auto right_click = [&](slint::LogicalPosition at) {
        window.dispatch_pointer_move_event(at);
        window.dispatch_pointer_press_event(at, slint::PointerEventButton::Right);
        window.dispatch_pointer_release_event(at, slint::PointerEventButton::Right);
        settle(ui);
    };
    CHECK(app.select_device("PC-A"));
    ui->global<UiRun>().set_target("");
    right_click(center(element(ui, "PC-B, 192.168.1.20/24")));
    CHECK(ui->global<UiInspector>().get_heading() == slint::SharedString("PC-B"));
    CHECK(exists(ui, "Ping from here") && exists(ui, "Ping this device") && exists(ui, "Power off") && !exists(ui, "Unplug cable"));
    press(ui, "Ping this device");
    CHECK(ui->global<UiRun>().get_target() == slint::SharedString("192.168.1.20"));
    CHECK(ui->global<UiInspector>().get_heading() == slint::SharedString("PC-A"));
    CHECK(status(ui) == "PC-A will ping PC-B (192.168.1.20): press Send." && !exists(ui, "Context menu"));
    // Escape closes it, as a click anywhere else does.
    right_click(center(element(ui, "Switch-01, 8 Ethernet ports")));
    CHECK(exists(ui, "Context menu") && !exists(ui, "Ping this device"));
    key(ui, slint::platform::key_codes::Escape);
    settle(ui);
    CHECK(!exists(ui, "Context menu"));
    // On the background: what can be added there.
    right_click(slint::LogicalPosition({700, 820}));
    CHECK(exists(ui, "Add router") && !exists(ui, "Ping this device"));
    press(ui, "Add router");
    CHECK(app.scenario().devices.size() == 4);
    shortcut(ui, "z");
    CHECK(app.scenario().devices.size() == 3);
}

void about_dialog(const Ui& ui) {
    CHECK(!exists(ui, "About resend"));
    ui->global<UiApp>().invoke_about();
    CHECK(exists(ui, "About resend"));
    // License notice and Slint attribution, required by Slint's Royalty-free license.
    CHECK(shown(ui, "© 2026 Zayd Soulmani · MIT License"));
    CHECK(ElementHandle::find_by_element_type_name(ui, "AboutSlint").size() == 1);
    press(ui, "Close");
    CHECK(!exists(ui, "About resend"));
}

void labs(const Ui& ui, resend::AppController& app, const fs::path& state) {
    fresh(app);
    const auto& lab = ui->global<UiLab>();
    CHECK(!lab.get_active() && !exists(ui, "Check the goals"));
    // G opens the library while no lab is open; a lab opens from it.
    key(ui, "g");
    CHECK(dialog(ui) == "labs" && exists(ui, "Your first network") && exists(ui, "The missing gateway"));
    press(ui, "The missing gateway");
    CHECK(dialog(ui).empty() && lab.get_active() && app.lab() && app.lab()->id == "missing-gateway");
    CHECK(lab.get_title() == slint::SharedString("The missing gateway") && exists(ui, "Check the goals"));
    CHECK(app.scenario() == app.lab()->network && !app.workspace().modified());

    // As given, both goals fail, and say why.
    press(ui, "Check the goals");
    CHECK(app.verdicts().size() == 2 && !app.verdicts()[0].met);
    CHECK(shown(ui, "PC-A does not reach 192.168.2.20: the request arrives, but PC-B cannot send its reply back: PC-B has no default gateway."));
    CHECK(lab.get_progress() == slint::SharedString("0 of 2 met"));

    // PC-A is locked: the inspector says so, and a change is refused.
    press(ui, "PC-A, 192.168.1.10/24");
    CHECK(ui->global<UiInspector>().get_lock_note() == slint::SharedString("The lab keeps PC-A as it is: it cannot be changed."));
    type(ui, "Default gateway", "192.168.1.254");
    press(ui, "Apply the configuration");
    CHECK(status(ui) == "The lab keeps PC-A as it is: its default gateway cannot be changed.");
    CHECK(app.scenario().device(DeviceId{1})->gateway->to_string() == "192.168.1.1");

    // PC-B is the one to repair; then the goals are met, and remembered.
    press(ui, "PC-B, 192.168.2.20/24");
    CHECK(ui->global<UiInspector>().get_lock_note().empty());
    type(ui, "Default gateway", "192.168.2.1");
    press(ui, "Apply the configuration");
    CHECK(lab.get_progress() == slint::SharedString("The network changed since the last check: press Check again."));
    press(ui, "Check the goals");
    CHECK(app.verdicts().size() == 2 && netsim::lab::all_met(app.verdicts()) && lab.get_solved());
    CHECK(status(ui) == "Every goal is met: The missing gateway is solved.");
    CHECK(resend::load_settings(state).solved_labs == std::vector<std::string>{"missing-gateway"});
    // The moment: the goals just met glow, "Lab solved!" plays over the
    // canvas, and the panel offers to play again what the check saw.
    settle(ui);
    CHECK(lab.get_celebration_serial() == 1);
    CHECK(lab.get_celebration() >= 0);
    CHECK(shown(ui, "Lab solved: The missing gateway"));
    CHECK(!app.played().empty() && app.played().back().kind == resend::CueKind::Win);
    CHECK(lab.get_goals()->row_data(0)->fresh && lab.get_goals()->row_data(1)->fresh);
    press(ui, "Check the goals");  // again: nothing new is met, nothing glows, no second moment
    CHECK(!lab.get_goals()->row_data(0)->fresh && lab.get_celebration_serial() == 1);
    press(ui, "Play again what the check saw");
    CHECK(status(ui) == "PC-A pings 192.168.2.20 again, as the check did.");
    CHECK(app.simulation().pings().size() == 1 && running(ui));
    for (int frame = 0; frame < 2000 && running(ui); ++frame) app.advance_playback(100ms);
    CHECK(app.simulation().pings()[0].state == netsim::PingState::Succeeded);

    // The solution takes the network's place, and gives it back.
    const auto mine = app.workspace().design();
    press(ui, "Show the solution");
    CHECK(dialog(ui) == "confirm");
    ui->global<UiApp>().invoke_answer("show");
    CHECK(lab.get_showing_solution() && app.scenario() == app.lab()->solution.network);
    press(ui, "Back to my network");
    CHECK(!lab.get_showing_solution() && app.workspace().design() == mine && app.workspace().modified());
    CHECK(lab.get_solved());

    // Hints, one at a time.
    CHECK(lab.get_hints()->row_count() == 0 && lab.get_hint_count() == 2);
    press(ui, "Show a hint");
    CHECK(lab.get_hints()->row_count() == 1 && shown(ui, "Ping PC-B from PC-A and follow the frames in the timeline: does the request arrive?"));

    // The first lab: devices come out of their boxes, in one undo step.
    lab.invoke_open_lab(0);
    CHECK(dialog(ui) == "confirm");  // the repaired network is not saved
    ui->global<UiApp>().invoke_answer("discard");
    CHECK(app.lab() && app.lab()->id == "first-network" && app.scenario().devices.empty());
    ui->global<UiApp>().invoke_add_device("host");
    ui->global<UiApp>().invoke_add_device("router");
    const auto& devices = app.scenario().devices;
    CHECK(devices.size() == 2 && !devices[0].ports[0].ipv4);
    CHECK(std::none_of(devices[1].ports.begin(), devices[1].ports.end(), [](const auto& port) { return port.admin_up; }));
    press(ui, "Undo Add a router");
    CHECK(app.scenario().devices.size() == 1);
    press(ui, "Check the goals");
    // The new host is PC-1 until it is named. (The goals are below the
    // steps, out of the panel's view: read from its model.)
    const auto goal = *lab.get_goals()->row_data(0);
    CHECK(goal.state == slint::SharedString("unmet") && goal.detail == slint::SharedString("There is no device named PC-A yet."));

    // The library ticks what is solved; leaving keeps the network.
    lab.invoke_show_library();
    CHECK(lab.get_library()->row_count() == app.library().size());
    CHECK(lab.get_library()->row_data(2)->solved && !lab.get_library()->row_data(0)->solved);
    press(ui, "Leave the lab, keeping the network");
    CHECK(!lab.get_active() && !app.lab() && app.scenario().devices.size() == 1);
    CHECK(!exists(ui, "Check the goals"));
}

// The exit check of M7: first-network from an empty canvas, as a learner
// does it, through the build bar, the cards and the inspector.
void first_lab_through_the_interface(const Ui& ui, resend::AppController& app) {
    fresh(app);
    // The router's inspector is long: only what is in view can be found.
    ui->window().set_size(slint::LogicalSize({1600, 1800}));
    settle(ui);
    const auto& lab = ui->global<UiLab>();
    lab.invoke_open_lab(0);
    CHECK(app.lab() && app.lab()->id == "first-network" && app.scenario().devices.empty());
    for (const auto* kind : {"Host", "Host", "Switch", "Switch", "Router"}) press(ui, std::string("Add ") + kind);
    CHECK(app.scenario().devices.size() == 5);

    const auto configure = [&](const std::string& card, const std::string& name, const std::string& address) {
        press(ui, card);
        type(ui, "Name", name);
        if (!address.empty()) {
            type(ui, "IPv4 address", address);
            type(ui, "Prefix length", "24");
        }
        press(ui, "Apply the configuration");
    };
    configure("PC-1, no IPv4 address", "PC-A", "192.168.1.10");
    configure("PC-2, no IPv4 address", "PC-B", "192.168.2.10");
    configure("Switch-3, 8 Ethernet ports", "Switch-1", "");
    configure("Switch-4, 8 Ethernet ports", "Switch-2", "");
    configure("Router-5, no IPv4 address", "Router", "");
    const auto cable = [&](const std::string& a, const std::string& b) {
        press(ui, "Connect " + a);
        press(ui, "Connect " + b);
    };
    cable("PC-A", "Switch-1");
    cable("Switch-1", "Router");
    cable("Router", "Switch-2");
    cable("Switch-2", "PC-B");
    CHECK(app.scenario().links.size() == 4);
    press(ui, "Router, no IPv4 address");
    type(ui, "IPv4 address of Port 1", "192.168.1.1/24");
    type(ui, "IPv4 address of Port 2", "192.168.2.1/24");
    press(ui, "Apply the configuration");
    press(ui, "Port 1");
    press(ui, "Port 2");
    const auto* router = app.scenario().device_named("Router");
    CHECK(router && router->ports[0].admin_up && router->ports[1].admin_up && !router->ports[2].admin_up);

    // Addresses and ports right, no gateways: the ping finds no route.
    press(ui, "Check the goals");
    const auto& verdicts = app.verdicts();
    CHECK(verdicts.size() == 5);
    for (std::size_t i = 0; i < 4; ++i) CHECK(verdicts[i].met);
    CHECK(!verdicts[4].met && verdicts[4].finding == netsim::lab::Finding::NoReply && verdicts[4].reason == netsim::Reason::NoRoute);
    CHECK(lab.get_goals()->row_data(4)->detail ==
          slint::SharedString("PC-A does not reach 192.168.2.10: PC-A's request has no route: PC-A has no default gateway."));

    // With the gateways it is met.
    for (const auto& [card, gateway] : {std::pair{"PC-A, 192.168.1.10/24", "192.168.1.1"}, std::pair{"PC-B, 192.168.2.10/24", "192.168.2.1"}}) {
        press(ui, card);
        type(ui, "Default gateway", gateway);
        press(ui, "Apply the configuration");
    }
    press(ui, "Check the goals");
    CHECK(netsim::lab::all_met(app.verdicts()) && lab.get_solved());
    ui->window().set_size(slint::LogicalSize({1100, 760}));
    fresh(app);
}

// The HUD says who sends, answers every action where it can be seen, names
// its buttons with their keys, and in a narrow window shows one drawer at a
// time.
void hud_is_clear(const Ui& ui, resend::AppController& app) {
    fresh(app);
    settle(ui);
    const auto& run = ui->global<UiRun>();
    const auto& hud = ui->global<UiApp>();
    const auto& lab = ui->global<UiLab>();
    // The selected host sends; with a switch selected nothing does.
    CHECK(run.get_source() == slint::SharedString("PC-A") && run.get_can_ping());
    press(ui, "Switch-01, 8 Ethernet ports");
    CHECK(run.get_source().empty() && !run.get_can_ping());
    press(ui, "PC-A, 192.168.1.10/24");

    // A ping that cannot start says why in a toast; a change the canvas
    // shows by itself raises none.
    auto toasts = hud.get_toast_serial();
    run.set_target("not an address");
    run.invoke_ping();
    CHECK(hud.get_toast_serial() == toasts + 1 && !status(ui).empty() && hud.get_toast() == hud.get_status());
    run.set_target("192.168.1.20");
    toasts = hud.get_toast_serial();
    app.set_theme("picnic");
    app.set_theme("blueberry");
    CHECK(hud.get_toast_serial() == toasts);

    // A button that shows only an icon says what it does, and its key, once
    // the pointer is on it.
    const auto& tip = ui->global<UiTip>();
    const auto reset = element(ui, "Reset the simulation");
    const auto at = reset.absolute_position();
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({at.x + reset.size().width / 2, at.y + reset.size().height / 2}));
    settle(ui);
    CHECK(tip.get_wanted() && tip.get_text() == slint::SharedString("Reset the simulation") && tip.get_keys() == slint::SharedString("R"));
    ui->window().dispatch_pointer_exit_event();
    settle(ui);
    CHECK(!tip.get_wanted());

    // Too narrow for the lab and the inspector with a network between them:
    // the lab that opens folds the inspector, each one opened folds the
    // other, and a wide window has both again.
    ui->window().set_size(slint::LogicalSize({948, 1020}));
    settle(ui);
    CHECK(hud.get_inspector_open());
    lab.invoke_open_lab(0);
    settle(ui);
    CHECK(lab.get_active() && lab.get_open() && !hud.get_inspector_open());
    // No goal glows as just met before any check.
    for (std::size_t row = 0; row < lab.get_goals()->row_count(); ++row) {
        const auto goal = *lab.get_goals()->row_data(row);
        CHECK(!goal.fresh && goal.state == slint::SharedString("open"));
    }
    press(ui, "Show the inspector");
    settle(ui);
    CHECK(hud.get_inspector_open() && !lab.get_open());
    press(ui, "Show the lab");
    settle(ui);
    CHECK(lab.get_open() && !hud.get_inspector_open());
    ui->window().set_size(slint::LogicalSize({1360, 900}));
    settle(ui);
    CHECK(lab.get_open() && hud.get_inspector_open());
    lab.invoke_leave();
    ui->window().set_size(slint::LogicalSize({1100, 760}));
    settle(ui);
    fresh(app);
}

// The Terminal tab (M8) as the mock-up draws it: the commands in a row of
// buttons no taller than a button, and one screen that holds what was
// printed and, at its foot, the line typed on.
void terminal_is_one_screen(const Ui& ui, resend::AppController& app) {
    fresh(app);
    settle(ui);
    const auto& terminal = ui->global<UiTerminal>();
    terminal.invoke_open();
    settle(ui);
    CHECK(ui->global<UiInspector>().get_tab() == 2 && terminal.get_available());
    const auto screen = element(ui, "Terminal screen");
    const auto menu = element(ui, "ip commands");
    const auto clear = element(ui, "Clear the terminal");
    CHECK(menu.size().height <= 34 && clear.size().height <= 34 && menu.size().width < 100);
    CHECK(menu.absolute_position().y + menu.size().height <= screen.absolute_position().y);
    CHECK(clear.absolute_position().x > menu.absolute_position().x + menu.size().width);
    CHECK(inside(element(ui, "Command line"), screen));
    CHECK(center(element(ui, "Command line")).y > center(screen).y);

    // A command stands after its prompt, over what it printed; one resend
    // does not know prints why, and what to type instead.
    const auto blocks = terminal.get_blocks();
    CHECK(blocks->row_count() == 0);
    terminal.invoke_run("ip route");
    terminal.invoke_run("frobnicate");
    settle(ui);
    CHECK(blocks->row_count() == 5);
    if (blocks->row_count() == 5) {
        const auto typed = *blocks->row_data(0);
        CHECK(typed.kind == slint::SharedString("command") && typed.prompt == slint::SharedString("root@PC-A:~#") &&
              typed.text == slint::SharedString("ip route"));
        CHECK(blocks->row_data(1)->kind == slint::SharedString("output"));
        CHECK(blocks->row_data(3)->kind == slint::SharedString("error"));
        CHECK(blocks->row_data(4)->kind == slint::SharedString("note"));
    }
    const std::string printed(terminal.get_screen());
    CHECK(printed.starts_with("root@PC-A:~# ip route\n") && printed.find("frobnicate: command not found") != std::string::npos);

    // A family's menu is no wider than a menu; a command with nothing to
    // fill in runs when chosen.
    press(ui, "ip commands");
    settle(ui);
    CHECK(element(ui, "Commands of ip").size().width <= 330);
    press(ui, "ip link");
    settle(ui);
    CHECK(blocks->row_count() == 7 && blocks->row_data(5)->text == slint::SharedString("ip link") && !exists(ui, "Commands of ip"));

    // More than the screen holds: the line stays on it. Clear empties the
    // screen and keeps what is being typed.
    for (int times = 0; times < 12; ++times) terminal.invoke_run("ip address");
    settle(ui);
    CHECK(inside(element(ui, "Command line"), element(ui, "Terminal screen")));
    terminal.set_line("ip a");
    press(ui, "Clear the terminal");
    settle(ui);
    CHECK(blocks->row_count() == 0 && terminal.get_screen().empty() && terminal.get_line() == slint::SharedString("ip a"));
    terminal.set_line("");
    ui->global<UiInspector>().set_tab(0);
    fresh(app);
}

// A panel is as tall inside as what it holds, so that its last line can be
// scrolled to: a router's settings in a lab, whose notes wrap, and a lab
// with every hint shown.
void panels_scroll_to_their_end(const Ui& ui, resend::AppController& app) {
    fresh(app);
    const auto& hud = ui->global<UiApp>();
    const auto& lab = ui->global<UiLab>();
    const bool events = hud.get_timeline_open();
    ui->window().set_size(slint::LogicalSize({1360, 900}));
    hud.set_timeline_open(true);
    CHECK(app.open_file(fs::path(RESEND_SOURCE_DIR) / "labs" / "static-routes.json"));
    settle(ui);
    CHECK(app.select_device("R1"));
    hud.set_inspector_open(true);
    ui->global<UiInspector>().set_tab(0);
    while (lab.get_hints()->row_count() < static_cast<std::size_t>(lab.get_hint_count())) lab.invoke_next_hint();
    settle(ui);
    const auto bottom = [](const ElementHandle& item) { return item.absolute_position().y + item.size().height; };
    // Wheels each panel to its end: what is past a panel's edge is not in
    // the tree the tests see.
    const auto views = ElementHandle::find_by_element_id(ui, "Scroll::flick");
    for (const auto& view : views) {
        // The phase of a mouse wheel: a touchpad's gesture has a start and an end.
        ui->window().dispatch_pointer_scroll_event(center(view), 0, -100000, slint::cbindgen_private::types::TouchPhase::Cancelled);
        settle(ui);
    }
    ui->window().dispatch_pointer_exit_event();
    // `last` shows whole in the view it is in.
    const auto shown_whole = [&](const ElementHandle& last, float margin) {
        for (const auto& view : views) {
            const auto x = last.absolute_position().x;
            if (x >= view.absolute_position().x && x < view.absolute_position().x + view.size().width)
                return last.absolute_position().y >= view.absolute_position().y && bottom(last) + margin <= bottom(view) + 0.5f;
        }
        return false;
    };
    const auto notes = ElementHandle::find_by_accessible_label(ui, "0.0.0.0/0 is the default route, for every address no other route matches.");
    CHECK(notes.size() == 1);
    if (notes.size() == 1) {
        CHECK(notes[0].size().height > 20);  // on two lines
        CHECK(shown_whole(notes[0], 14));
    }
    const auto hints = ElementHandle::find_by_element_type_name(ui, "HintRow");
    CHECK(hints.size() == static_cast<std::size_t>(lab.get_hint_count()) && hints.size() > 0);
    if (hints.size() > 0) CHECK(shown_whole(hints[hints.size() - 1], 12));

    lab.invoke_leave();
    hud.set_timeline_open(events);
    ui->window().set_size(slint::LogicalSize({1100, 760}));
    settle(ui);
    fresh(app);
}

// Every dialog keeps its buttons a button high, has the keyboard while it
// is open and closes with Escape.
void dialogs_close_with_escape(const Ui& ui, resend::AppController& app) {
    fresh(app);
    settle(ui);
    const auto& state = ui->global<UiApp>();
    const auto escape = slint::SharedString(slint::platform::key_codes::Escape);
    // No button takes the spare height of its dialog, or of a panel.
    const auto buttons_fit = [&] {
        int seen = 0;
        bool fit = true;
        for (const auto& button : ElementHandle::find_by_element_type_name(ui, "Action")) {
            ++seen;
            if (button.size().height > 36) {
                fit = false;
                const auto label = button.accessible_label();
                std::cerr << "A button " << button.size().height << " px high: " << (label ? std::string_view(*label) : std::string_view("?")) << '\n';
            }
        }
        return fit && seen > 0;
    };
    const auto closes = [&](std::string_view name) {
        if (dialog(ui) != name) return false;
        if (!buttons_fit()) return false;
        key(ui, escape);
        settle(ui);
        return dialog(ui).empty();
    };

    // The labs: the list has the room, Close is a button.
    ui->global<UiLab>().invoke_show_library();
    CHECK(exists(ui, "Your first network"));
    const auto close = element(ui, "Close");
    CHECK(close.size().height <= 34);
    for (const auto& item : controls(ui, "Labs"))
        if (item.accessible_role() == AccessibleRole::List) CHECK(item.size().height > 300 && close.absolute_position().y > item.absolute_position().y + item.size().height);
    CHECK(closes("labs"));
    // The focus is the canvas's again: G opens the library once more.
    key(ui, "g");
    CHECK(closes("labs"));

    // No key reaches the network under a dialog.
    CHECK(app.select_device("PC-B"));
    const auto devices = app.scenario().devices.size();
    state.invoke_show_keys();
    CHECK(exists(ui, "Keyboard shortcuts"));
    key(ui, slint::platform::key_codes::Delete);
    key(ui, "1");
    CHECK(app.scenario().devices.size() == devices && dialog(ui) == "keys");
    CHECK(closes("keys"));

    state.invoke_about();
    CHECK(exists(ui, "About resend"));
    CHECK(closes("about"));
    // The dialog has the keyboard: Tab goes to its own button, not to a
    // card of the network behind it.
    state.invoke_about();
    CHECK(exists(ui, "About resend"));
    key(ui, slint::platform::key_codes::Tab);
    key(ui, " ");
    settle(ui);
    CHECK(dialog(ui).empty());

    // A dialog with a field: what is typed goes to it, and Escape still closes.
    state.invoke_show_commands();
    CHECK(exists(ui, "Search the actions"));
    key(ui, "z");
    CHECK(state.get_command_filter() == slint::SharedString("z"));
    CHECK(closes("commands"));
    state.invoke_open_document();
    CHECK(exists(ui, "Files"));
    CHECK(closes("open"));
    state.invoke_save_as();
    CHECK(exists(ui, "File name"));
    const auto name = state.get_file_name();
    key(ui, "z");
    CHECK(state.get_file_name() != name);
    CHECK(closes("save"));

    // A question: Escape is its Cancel, and nothing is lost.
    press(ui, "Add Host");
    CHECK(app.workspace().modified());
    state.invoke_new_document();
    CHECK(dialog(ui) == "confirm" && exists(ui, "Cancel"));
    CHECK(closes("confirm"));
    CHECK(app.workspace().modified() && app.scenario().devices.size() == devices + 1);
    fresh(app);

    // The first run's choices.
    state.set_dialog("welcome");
    CHECK(exists(ui, "Welcome to resend"));
    CHECK(closes("welcome"));
}

void set_environment(const char* name, const fs::path& value) {
#ifdef _WIN32
    _putenv_s(name, value.string().c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
} // namespace

int main() {
    // A home and a state folder of the tests' own: the file dialog starts at
    // home, settings and autosaves go to the state folder.
    const auto root = fs::temp_directory_path() / "resend-gui-tests";
    fs::remove_all(root);
    const auto home = root / "home", state = root / "state";
    fs::create_directories(home);
    fs::create_directories(state);
    set_environment("HOME", home);
    set_environment("USERPROFILE", home);  // the home on Windows

    slint::testing::init();
    auto ui = AppWindow::create();
    ui->window().set_size(slint::LogicalSize({1100, 760}));
    {
        resend::AppController app(ui, {state});
        // The timeline opens into its events, which most tests read.
        ui->global<UiApp>().set_timeline_open(true);
        ui->show();

        initial_view_shows_whole_network(ui);
        drag_keyboard_and_undo(ui, app);
        plug_cables(ui, app);
        cable_inspector(ui, app);
        device_inspector(ui, app);
        ping_and_tables(ui, app);
        frame_fields_and_bytes(ui, app);
        timeline_filters(ui, app);
        scrubber_travels_and_locks(ui, app);
        drops_and_playback_controls(ui, app, state);
        sound_on_and_off(ui, app, state);
        packets_move_on_the_canvas(ui, app);
        flood_copies_travel_together(ui, app);
        pings_overlap_and_count(ui, app);
        live_edits(ui, app);
        storm_pauses_playback(ui, app);
        examples_from_the_menu(ui, app);
        routers(ui, app);
        save_and_reopen(ui, app, home);
        unsaved_changes_are_asked_about(ui, app);
        autosave_and_recovery(ui, app, state);
        panels_fold(ui);
        canvas_is_the_stage(ui, app);
        themes_move_nothing(ui, app);
        palette_has_every_action(ui, app);
        tables_cut_nothing(ui, app);
        context_menu(ui, app);
        labs(ui, app, state);
        first_lab_through_the_interface(ui, app);
        hud_is_clear(ui, app);
        terminal_is_one_screen(ui, app);
        panels_scroll_to_their_end(ui, app);
        dialogs_close_with_escape(ui, app);
        about_dialog(ui);
    }
    fs::remove_all(root);
    std::cout << "GUI tests passed: " << test::checks << " checks.\n";
}

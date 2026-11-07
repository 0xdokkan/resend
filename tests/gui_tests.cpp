// GUI behaviour tests on Slint's testing backend: no display is needed.
// Elements are found by accessible label and driven through accessibility
// actions or real pointer/keyboard events, as a user or screen reader would.
#include "app_controller.hpp"
#include "check.hpp"
#include "scenario/scenario_json.hpp"

#include <slint-testing.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace std::chrono_literals;
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
    if (found.size() != 1)
        std::cerr << "Expected one control labelled \"" << label << "\", found " << found.size() << '\n';
    CHECK(found.size() == 1);
    return found[0];
}
bool exists(const Ui& ui, std::string_view label) { return !controls(ui, label).empty(); }
// Plain text on screen, such as the decision chips on the canvas.
bool shown(const Ui& ui, std::string_view text) { return !ElementHandle::find_by_accessible_label(ui, text).empty(); }
void press(const Ui& ui, std::string_view label) { element(ui, label).invoke_accessible_default_action(); }
void type(const Ui& ui, std::string_view label, std::string_view value) {
    element(ui, label).set_accessible_value(slint::SharedString(value));
}
// Steps through the experiment, letting real time pass after every press so
// each step's animation completes.
void run_to_end(const Ui& ui, resend::AppController& app) {
    for (int step = 0; step < 500 && app.simulation().pending(); ++step) {
        press(ui, "Step →");
        app.advance_playback(2s);
    }
    CHECK(!app.simulation().pending());
}
void play_to_end(const Ui& ui, resend::AppController& app) {
    press(ui, "▷ Run");
    for (int frame = 0; frame < 2000 && app.simulation().pending(); ++frame) app.advance_playback(100ms);
    CHECK(!app.simulation().pending() && !ui->get_running());
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

void initial_view_shows_whole_network(const Ui& ui) {
    // The 1100 px minimum width used to cut PC-B off at startup.
    const auto canvas = element(ui, "Network topology");
    CHECK(inside(element(ui, "PC-A, 192.168.1.10/24"), canvas));
    CHECK(inside(element(ui, "PC-B, 192.168.1.20/24"), canvas));
    CHECK(inside(element(ui, "Switch-01, 8 Ethernet ports"), canvas));
}

void drag_and_keyboard_move(const Ui& ui, const resend::AppController& app) {
    const auto before = *app.scenario().device(DeviceId{1});
    auto& window = ui->window();
    auto from = center(element(ui, "PC-A, 192.168.1.10/24"));
    from.y -= 20;  // away from the connector button in the lower right corner
    window.dispatch_pointer_press_event(from, slint::PointerEventButton::Left);
    for (int step = 1; step <= 5; ++step)
        window.dispatch_pointer_move_event(slint::LogicalPosition({from.x + 10.f * step, from.y + 6.f * step}));
    window.dispatch_pointer_release_event(slint::LogicalPosition({from.x + 50, from.y + 30}), slint::PointerEventButton::Left);
    const auto after = *app.scenario().device(DeviceId{1});
    const int dx = after.x - before.x, dy = after.y - before.y;
    CHECK(dx > 0 && dy > 0);
    // Same direction as the pointer, whatever the zoom; positions are whole
    // canvas units, so each may be off by half a unit.
    CHECK(std::abs(dx * 30 - dy * 50) <= 40);

    // Pressing a card focuses it; the arrow keys then move it by 10 units.
    window.dispatch_key_press_event(slint::platform::key_codes::RightArrow);
    window.dispatch_key_release_event(slint::platform::key_codes::RightArrow);
    CHECK(app.scenario().device(DeviceId{1})->x == after.x + 10);
}

void ping_through_buttons(const Ui& ui, resend::AppController& app) {
    press(ui, "PC-A, 192.168.1.10/24");
    CHECK(element(ui, "Destination IPv4").accessible_value() == slint::SharedString("192.168.1.20"));
    press(ui, "Send");
    CHECK(app.simulation().pending());
    run_to_end(ui, app);
    CHECK(succeeded(app));
    CHECK(ui->get_event_count() == 9);
    // The list follows the latest event: its row is entirely in view.
    CHECK(inside(element(ui, "ICMP: Ping succeeded · 192.168.1.20 → PC-A"), element(ui, "Event list")));
    press(ui, "Switch-01, 8 Ethernet ports");
    CHECK(app.simulation().mac_table(DeviceId{3}).size() == 2);
}

void invalid_input_keeps_configuration(const Ui& ui, const resend::AppController& app) {
    press(ui, "PC-A, 192.168.1.10/24");
    type(ui, "IPv4 address", "invalid");
    press(ui, "Apply configuration");
    CHECK(app.scenario().device(DeviceId{1})->ports[0].ipv4->to_string() == "192.168.1.10/24");
    CHECK(std::string_view(ui->get_status_text()).find("Invalid IPv4 host address") != std::string_view::npos);
}

void disabled_cable_breaks_ping(const Ui& ui, resend::AppController& app) {
    // The network keeps running: the cable goes down at the current time,
    // and PC-A still has PC-B's MAC from the last ping.
    const auto now = app.simulation().now();
    press(ui, "Disable PC-B :1 — Switch-01 :2");
    CHECK(!app.scenario().link(LinkId{2})->up);
    CHECK(app.simulation().edits().size() == 1 && app.simulation().edits()[0].time == now);
    CHECK(std::string_view(ui->get_status_text()).ends_with("Applied at t = " + netsim::format_ms(now) + "."));
    CHECK(exists(ui, "EDIT: Cable disabled · PC-B port 1 — Switch-01 port 2"));
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    run_to_end(ui, app);
    CHECK(!succeeded(app));
    CHECK(exists(ui, "ICMP: Ping failed · reply timed out"));
}

void build_and_delete_device(const Ui& ui, const resend::AppController& app) {
    press(ui, "Sample network");
    press(ui, "Add Host");
    CHECK(app.scenario().devices.size() == 4);
    // New cards appear in the visible area: clipped controls are not found.
    CHECK(inside(element(ui, "PC-4, 192.168.1.13/24"), element(ui, "Network topology")));
    press(ui, "Connect PC-4");
    press(ui, "Connect Switch-01");
    CHECK(app.scenario().links.size() == 3);
    press(ui, "PC-4, 192.168.1.13/24");
    press(ui, "Delete device");
    CHECK(app.scenario().devices.size() == 3 && app.scenario().links.size() == 2);
}

void packets_move_on_the_canvas(const Ui& ui, resend::AppController& app) {
    press(ui, "Sample network");
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    CHECK(!exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    press(ui, "Step →");  // the request is due at once: it appears at the port
    CHECK(exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    CHECK(ui->get_sim_clock() == slint::SharedString("0.000 ms"));
    press(ui, "Step →");
    CHECK(ui->get_running());
    app.advance_playback(300ms);  // halfway along the cable
    CHECK(ui->get_sim_clock() == slint::SharedString("5.000 ms"));
    CHECK(exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    CHECK(app.explainer().events().size() == 1);  // nothing arrived yet
    app.advance_playback(300ms);
    CHECK(!ui->get_running());
    // The switch's decisions appear on the switch, and the flooded copy leaves.
    CHECK(shown(ui, "learn PC-A → p1"));
    CHECK(shown(ui, "flood ×1"));
    CHECK(!exists(ui, "Packet: ARP Request, broadcast, PC-A → Switch-01"));
    CHECK(exists(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-B"));
    // Selecting a packet shows its frame layer by layer.
    press(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-B");
    CHECK(std::string_view(ui->get_frame_layers()).starts_with("Ethernet II · dst ff:ff:ff:ff:ff:ff"));
    CHECK(!ui->get_follow_events());
}

void flood_copies_travel_together(const Ui& ui, resend::AppController& app) {
    press(ui, "Sample network");
    press(ui, "Add Host");
    press(ui, "Connect PC-4");
    press(ui, "Connect Switch-01");
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    app.seek(15ms);
    CHECK(exists(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-B"));
    CHECK(exists(ui, "Packet: ARP Request, broadcast, Switch-01 → PC-4"));
    CHECK(shown(ui, "flood ×2"));
    play_to_end(ui, app);
    CHECK(succeeded(app));
    CHECK(shown(ui, "✓ ping reply"));
}

void drops_and_playback_controls(const Ui& ui, resend::AppController& app) {
    press(ui, "Sample network");
    press(ui, "PC-B, 192.168.1.20/24");
    press(ui, "Power off");
    CHECK(!app.scenario().device(DeviceId{2})->powered);
    CHECK(exists(ui, "Power on"));
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    app.seek(20ms);
    CHECK(shown(ui, "× device off"));

    press(ui, "Playback speed");
    CHECK(ui->get_speed_label() == slint::SharedString("2×"));
    for (int i = 0; i < 4; ++i) press(ui, "Playback speed");
    CHECK(ui->get_speed_label() == slint::SharedString("1×"));
    // Reduced motion: a step jumps straight to the next event.
    press(ui, "Reduce motion");
    CHECK(ui->get_reduced_motion());
    press(ui, "Step →");
    CHECK(!ui->get_running());
    CHECK(ui->get_sim_clock() == slint::SharedString("1000.000 ms"));
    CHECK(shown(ui, "ARP timeout"));
    press(ui, "Reduce motion");
    CHECK(!ui->get_reduced_motion());
    press(ui, "Reset");
    CHECK(ui->get_sim_clock() == slint::SharedString("0.000 ms"));
    CHECK(!app.simulation().pending());
}

void pings_overlap(const Ui& ui, resend::AppController& app) {
    // A second ping can start while the first is on its way.
    press(ui, "Sample network");
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    app.seek(25ms);
    press(ui, "PC-B, 192.168.1.20/24");
    type(ui, "Destination IPv4", "192.168.1.10");
    press(ui, "Send");
    CHECK(app.simulation().pings().size() == 2);
    CHECK(app.simulation().pings()[1].started == 25ms);
    play_to_end(ui, app);
    CHECK(succeeded(app));
    CHECK(std::string_view(ui->get_status_text()).starts_with("Every ping succeeded"));
    // A refused ping says why.
    type(ui, "Destination IPv4", "192.168.1.255");
    press(ui, "Send");
    CHECK(std::string_view(ui->get_status_text()) == "Pinging network or broadcast addresses is not supported.");
    type(ui, "Destination IPv4", "192.168.1.20");
}

void open_scenario_file(const Ui& ui, resend::AppController& app) {
    const auto path = std::filesystem::path(RESEND_EXAMPLES_DIR) / "office-floor.json";
    std::ifstream file(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string error;
    auto scenario = netsim::scenario_from_json(text, error);
    CHECK(scenario.has_value());
    app.open(std::move(*scenario));
    CHECK(std::string_view(ui->get_status_text()).starts_with("Office floor loaded."));
    // The view fits the new network, and the first host is selected.
    const auto canvas = element(ui, "Network topology");
    CHECK(inside(element(ui, "Desk-1, 10.0.0.11/24"), canvas));
    CHECK(inside(element(ui, "Printer-Room, 10.0.0.23/24"), canvas));
    CHECK(ui->get_selected_id() == static_cast<int>(app.scenario().device_named("Desk-1")->id.value));
    type(ui, "Destination IPv4", "10.0.0.22");
    press(ui, "Send");
    play_to_end(ui, app);
    CHECK(succeeded(app));
}

void live_edits(const Ui& ui, resend::AppController& app) {
    // Before anything happens, an edit simply gives another network.
    press(ui, "Sample network");
    press(ui, "Disable PC-B :1 — Switch-01 :2");
    CHECK(!app.scenario().link(LinkId{2})->up && app.simulation().edits().empty() && ui->get_event_count() == 0);
    press(ui, "Enable PC-B :1 — Switch-01 :2");
    CHECK(app.scenario().link(LinkId{2})->up);

    // While a ping runs, the edit happens at the instant on screen.
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    app.seek(15ms);
    press(ui, "PC-B, 192.168.1.20/24");
    press(ui, "Power off");
    CHECK(app.simulation().edits().size() == 1 && app.simulation().edits()[0].time == 15ms);
    CHECK(std::string_view(ui->get_status_text()).ends_with("Applied at t = 15.000 ms."));
    CHECK(exists(ui, "EDIT: PC-B powered off"));
    CHECK(shown(ui, "⏻ powered off"));
    app.seek(20ms);
    CHECK(shown(ui, "× device off"));
    play_to_end(ui, app);
    CHECK(!succeeded(app));
    // Its tables went with the power.
    press(ui, "Power on");
    CHECK(app.simulation().arp_cache(DeviceId{2}).empty());
}

void ping_count(const Ui& ui, resend::AppController& app) {
    press(ui, "Sample network");
    press(ui, "PC-A, 192.168.1.10/24");
    type(ui, "Destination IPv4", "192.168.1.20");
    for (const auto* refused : {"0", "1001", "3x"}) {
        type(ui, "Echo Requests per ping", refused);
        press(ui, "Send");
        CHECK(app.simulation().pings().empty());
        CHECK(std::string_view(ui->get_status_text()) == "Send 1 to 1000 Echo Requests per ping.");
    }
    type(ui, "Echo Requests per ping", "3");
    press(ui, "Send");
    play_to_end(ui, app);
    CHECK(app.simulation().pings().size() == 1 && app.simulation().pings()[0].replies() == 3);
    CHECK(exists(ui, "ICMP: Reply 2/3 · 192.168.1.20 → PC-A"));
    CHECK(shown(ui, "✓ ping 3/3"));
    type(ui, "Echo Requests per ping", "1");
}

void storm_pauses_playback(const Ui& ui, resend::AppController& app) {
    const auto path = std::filesystem::path(RESEND_EXAMPLES_DIR) / "switch-loop.json";
    std::ifstream file(path, std::ios::binary);
    std::string error;
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto scenario = netsim::scenario_from_json(text, error);
    CHECK(scenario.has_value());
    app.open(std::move(*scenario));
    type(ui, "Destination IPv4", "192.168.1.20");
    press(ui, "Send");
    press(ui, "▷ Run");
    for (int frame = 0; frame < 2000 && ui->get_running(); ++frame) app.advance_playback(100ms);
    // Paused at the guard, with the network still running.
    CHECK(!ui->get_running() && app.simulation().pending());
    CHECK(std::string_view(ui->get_status_text()).starts_with("Storm protection:"));
    CHECK(exists(ui, "SIMULATOR: Storm protection · frame stopped at Switch-2"));
    CHECK(shown(ui, "⚠ storm stopped"));
    // Run again: it pauses at each of the three guards.
    int pauses = 1;
    while (app.simulation().pending() && pauses < 10) {
        press(ui, "▷ Run");
        for (int frame = 0; frame < 2000 && ui->get_running(); ++frame) app.advance_playback(100ms);
        if (app.simulation().pending()) ++pauses;
    }
    CHECK(!app.simulation().pending() && pauses == 3);
}

void about_dialog(const Ui& ui) {
    CHECK(!exists(ui, "About resend"));
    press(ui, "About");
    CHECK(exists(ui, "About resend"));
    // License notice and Slint attribution, required by Slint's Royalty-free license.
    CHECK(!ElementHandle::find_by_accessible_label(ui, "© 2026 Zayd Soulmani · MIT License").empty());
    CHECK(ElementHandle::find_by_element_type_name(ui, "AboutSlint").size() == 1);
    press(ui, "Close");
    CHECK(!exists(ui, "About resend"));
}
} // namespace

int main() {
    slint::testing::init();
    auto ui = AppWindow::create();
    ui->window().set_size(slint::LogicalSize({1100, 760}));
    resend::AppController app(ui);
    ui->show();

    initial_view_shows_whole_network(ui);
    drag_and_keyboard_move(ui, app);
    ping_through_buttons(ui, app);
    invalid_input_keeps_configuration(ui, app);
    disabled_cable_breaks_ping(ui, app);
    build_and_delete_device(ui, app);
    packets_move_on_the_canvas(ui, app);
    flood_copies_travel_together(ui, app);
    drops_and_playback_controls(ui, app);
    pings_overlap(ui, app);
    open_scenario_file(ui, app);
    live_edits(ui, app);
    ping_count(ui, app);
    storm_pauses_playback(ui, app);
    about_dialog(ui);
    std::cout << "GUI tests passed: " << test::checks << " checks.\n";
}

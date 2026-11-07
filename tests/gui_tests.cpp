// GUI behaviour tests on Slint's testing backend: no display is needed.
// Elements are found by accessible label and driven through accessibility
// actions or real pointer/keyboard events, as a user or screen reader would.
#include "app_controller.hpp"
#include "check.hpp"

#include <slint-testing.h>

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using slint::language::AccessibleRole;
using slint::testing::ElementHandle;
using Ui = slint::ComponentHandle<AppWindow>;

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
void press(const Ui& ui, std::string_view label) { element(ui, label).invoke_accessible_default_action(); }
void type(const Ui& ui, std::string_view label, std::string_view value) {
    element(ui, label).set_accessible_value(slint::SharedString(value));
}
void run_to_end(const Ui& ui, const resend::AppController& app) {
    for (int step = 0; step < 500 && app.engine().pending(); ++step) press(ui, "Step →");
    CHECK(!app.engine().pending());
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
    const auto before = *app.engine().device(1);
    auto& window = ui->window();
    auto from = center(element(ui, "PC-A, 192.168.1.10/24"));
    from.y -= 20;  // away from the connector button in the lower right corner
    window.dispatch_pointer_press_event(from, slint::PointerEventButton::Left);
    for (int step = 1; step <= 5; ++step)
        window.dispatch_pointer_move_event(slint::LogicalPosition({from.x + 10.f * step, from.y + 6.f * step}));
    window.dispatch_pointer_release_event(slint::LogicalPosition({from.x + 50, from.y + 30}), slint::PointerEventButton::Left);
    const auto after = *app.engine().device(1);
    const float dx = after.x - before.x, dy = after.y - before.y;
    CHECK(dx > 0 && dy > 0);
    CHECK(std::abs(dx * 30 - dy * 50) < 1.0f);  // same direction as the pointer, whatever the zoom

    // Pressing a card focuses it; the arrow keys then move it by 10 units.
    window.dispatch_key_press_event(slint::platform::key_codes::RightArrow);
    window.dispatch_key_release_event(slint::platform::key_codes::RightArrow);
    CHECK(app.engine().device(1)->x == after.x + 10);
}

void ping_through_buttons(const Ui& ui, const resend::AppController& app) {
    press(ui, "PC-A, 192.168.1.10/24");
    CHECK(element(ui, "Destination IPv4").accessible_value() == slint::SharedString("192.168.1.20"));
    press(ui, "Send");
    CHECK(app.engine().pending());
    run_to_end(ui, app);
    CHECK(app.engine().ping_succeeded());
    CHECK(ui->get_event_count() == 9);
    // The list follows the latest event: its row is entirely in view.
    CHECK(inside(element(ui, "ICMP: Ping succeeded · 192.168.1.20 → PC-A"), element(ui, "Event list")));
    press(ui, "Switch-01, 8 Ethernet ports");
    CHECK(app.engine().mac_table(3).size() == 2);
}

void invalid_input_keeps_configuration(const Ui& ui, const resend::AppController& app) {
    press(ui, "PC-A, 192.168.1.10/24");
    type(ui, "IPv4 address", "invalid");
    press(ui, "Apply configuration");
    CHECK(app.engine().device(1)->ip.to_string() == "192.168.1.10");
    CHECK(std::string_view(ui->get_status_text()).find("Invalid IPv4 host address") != std::string_view::npos);
}

void disabled_cable_breaks_ping(const Ui& ui, const resend::AppController& app) {
    press(ui, "Disable PC-B :1 — Switch-01 :2");
    CHECK(!app.engine().link(2)->enabled);
    press(ui, "PC-A, 192.168.1.10/24");
    press(ui, "Send");
    run_to_end(ui, app);
    CHECK(!app.engine().ping_succeeded());
    CHECK(exists(ui, "ARP: Ping failed · ARP resolution timed out"));
}

void build_and_delete_device(const Ui& ui, const resend::AppController& app) {
    press(ui, "Sample network");
    press(ui, "Add Host");
    CHECK(app.engine().devices().size() == 4);
    // New cards appear in the visible area: clipped controls are not found.
    CHECK(inside(element(ui, "PC-4, 192.168.1.13/24"), element(ui, "Network topology")));
    press(ui, "Connect PC-4");
    press(ui, "Connect Switch-01");
    CHECK(app.engine().links().size() == 3);
    press(ui, "PC-4, 192.168.1.13/24");
    press(ui, "Delete device");
    CHECK(app.engine().devices().size() == 3 && app.engine().links().size() == 2);
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
    about_dialog(ui);
    std::cout << "GUI tests passed: " << test::checks << " checks.\n";
}

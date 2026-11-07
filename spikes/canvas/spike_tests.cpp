// Controller geometry tests, then the same interactions through Slint's
// testing backend with real pointer and keyboard events (no display needed).
#include "check.hpp"
#include "spike_app.hpp"

#include <slint-testing.h>

#include <cmath>
#include <iostream>
#include <string_view>

namespace {
using namespace spike;
using slint::testing::ElementHandle;

bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

void geometry() {
    CHECK(near(distance_to_segment({5, 5}, {0, 0}, {10, 0}), 5));
    CHECK(near(distance_to_segment({-3, 4}, {0, 0}, {10, 0}), 5));  // beyond an end: distance to the end
    CHECK(near(distance_to_segment({3, 4}, {0, 0}, {0, 0}), 5));     // degenerate segment

    CanvasController c;
    CHECK(c.zoom_at({100, 50}, 2) == View);
    const Vec2 p = c.to_canvas({100, 50});
    CHECK(near(p.x, 100) && near(p.y, 50));  // the point under the cursor stays put
    const Vec2 back = c.to_screen(c.to_canvas({321, 123}));
    CHECK(near(back.x, 321) && near(back.y, 123));
}

void hit_priority_and_tolerance() {
    CanvasController c;
    const int a = c.add_node(NodeKind::Host, {0, 0});
    const int b = c.add_node(NodeKind::Host, {400, 0});
    CHECK(c.connect({a, 0}, {b, 0}));
    CHECK(!c.connect({a, 0}, {b, 0}));  // ports are already in use
    CHECK(!c.connect({a, 0}, {a, 0}));
    const Vec2 port = c.to_screen(c.port_anchor({a, 0}));
    CHECK((c.hit_test(port) == Hit{HitKind::Port, a, 0}));                 // port beats card and cable
    CHECK((c.hit_test({10, 10}) == Hit{HitKind::Node, a}));
    CHECK((c.hit_test({280, card_height + 5}) == Hit{HitKind::Link, 1}));  // 5 px from the cable
    CHECK(c.hit_test({280, card_height + 9}).kind == HitKind::None);
    // Zoomed out, the tolerance stays in screen pixels.
    c.zoom_at({0, 0}, 0.3f);
    const Vec2 middle = c.to_screen({280, card_height});
    CHECK(c.hit_test({middle.x, middle.y + 5}).kind == HitKind::Link);
    CHECK(c.hit_test({middle.x, middle.y + 9}).kind == HitKind::None);
}

void linking_and_editing() {
    CanvasController c;
    const int pc = c.add_node(NodeKind::Host, {0, 0});
    const int sw = c.add_node(NodeKind::Switch, {300, 200});
    const int pc2 = c.add_node(NodeKind::Host, {600, 0});
    const Vec2 from = c.to_screen(c.port_anchor({pc, 0}));
    const Vec2 to = c.to_screen(c.port_anchor({sw, 3}));
    CHECK(c.press(from, Button::Left) & Overlay);
    CHECK(c.move({to.x, to.y}) == Overlay);
    CHECK((c.hover() == Hit{HitKind::Port, sw, 3}));
    CHECK(c.rubber_band().has_value());
    CHECK(c.release(to) & Topology);
    CHECK(c.links().size() == 1 && (c.links()[0].b == PortRef{sw, 3}));
    CHECK(!c.rubber_band().has_value());

    // Dropping on the same device, on a used port or on nothing creates no cable.
    const Vec2 from2 = c.to_screen(c.port_anchor({pc2, 0}));
    c.press(from2, Button::Left);
    CHECK(!(c.release(from2) & Topology));
    c.press(from2, Button::Left);
    CHECK(!(c.release(to) & Topology));
    c.press(from2, Button::Left);
    c.move({900, 900});
    CHECK(c.key(Key::Escape) == Overlay);  // Escape cancels the cable being drawn
    CHECK(!(c.release({900, 900}) & Topology));
    CHECK(c.links().size() == 1);

    // Dragging a card moves it by the pointer delta divided by the zoom.
    c.zoom_at({0, 0}, 2);
    const Vec2 grab = c.to_screen({330, 220});
    c.press(grab, Button::Left);
    CHECK(c.move({grab.x + 40, grab.y + 20}) == Moved);
    c.release({grab.x + 40, grab.y + 20});
    CHECK(near(c.node(sw)->pos.x, 320) && near(c.node(sw)->pos.y, 210));
    CHECK(c.key(Key::Right) == Moved && near(c.node(sw)->pos.x, 330));

    // Delete removes the selected device together with its cables.
    CHECK(c.key(Key::Delete) & Topology);
    CHECK(c.nodes().size() == 2 && c.links().empty());

    // Fit keeps every card inside the viewport.
    c.fit({800, 600});
    for (const auto& n : c.nodes()) {
        const Vec2 tl = c.to_screen(n.pos), br = c.to_screen({n.pos.x + card_width, n.pos.y + card_height});
        CHECK(tl.x >= 0 && tl.y >= 0 && br.x <= 800 && br.y <= 600);
    }
}

// --- Through the Slint UI -------------------------------------------------

using Ui = slint::ComponentHandle<SpikeWindow>;

slint::LogicalPosition on_canvas(const Ui& ui, Vec2 screen) {
    const auto canvas = ElementHandle::find_by_accessible_label(ui, "Canvas")[0].absolute_position();
    return slint::LogicalPosition({canvas.x + screen.x, canvas.y + screen.y});
}
void click(const Ui& ui, Vec2 at, slint::PointerEventButton button = slint::PointerEventButton::Left) {
    ui->window().dispatch_pointer_press_event(on_canvas(ui, at), button);
    ui->window().dispatch_pointer_release_event(on_canvas(ui, at), button);
}
void key(const Ui& ui, std::u8string_view code) {
    ui->window().dispatch_key_press_event(slint::SharedString(code));
    ui->window().dispatch_key_release_event(slint::SharedString(code));
}

void ui_cable_by_dragging(const Ui& ui, SpikeApp& app) {
    auto& c = app.canvas();
    const int pc = c.nodes()[3].id, sw = c.nodes()[2].id;
    const Vec2 from = c.to_screen(c.port_anchor({pc, 0}));
    const Vec2 to = c.to_screen(c.port_anchor({sw, 4}));
    ui->window().dispatch_pointer_press_event(on_canvas(ui, from), slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_move_event(on_canvas(ui, {(from.x + to.x) / 2, (from.y + to.y) / 2}));
    CHECK(!std::string_view(ui->get_band_path()).empty());  // rubber band follows the pointer
    ui->window().dispatch_pointer_move_event(on_canvas(ui, to));
    ui->window().dispatch_pointer_release_event(on_canvas(ui, to), slint::PointerEventButton::Left);
    CHECK(c.links().size() == 3);
    CHECK(std::string_view(ui->get_band_path()).empty());
    CHECK(!std::string_view(ui->get_selected_path()).empty());  // the new cable is selected
}

void ui_select_cable_and_delete(const Ui& ui, SpikeApp& app) {
    auto& c = app.canvas();
    const Link first = c.links()[0];
    const Vec2 a = c.to_screen(c.port_anchor(first.a)), b = c.to_screen(c.port_anchor(first.b));
    click(ui, {(a.x + b.x) / 2 + 3, (a.y + b.y) / 2 + 3});  // a few pixels off the line
    CHECK((c.selection() == Hit{HitKind::Link, first.id}));
    key(ui, slint::platform::key_codes::Delete);
    CHECK(c.links().size() == 2);
}

void ui_scale_factor(const Ui& ui, SpikeApp& app) {
    // Input arrives in logical pixels, so hit testing does not depend on the scale.
    ui->window().dispatch_scale_factor_change_event(1.5f);
    auto& c = app.canvas();
    const auto& n = c.nodes()[1];
    click(ui, c.to_screen({n.pos.x + 30, n.pos.y + 30}));
    CHECK((c.selection() == Hit{HitKind::Node, n.id}));
    ui->window().dispatch_scale_factor_change_event(1.0f);
}

void ui_context_menu(const Ui& ui, SpikeApp& app) {
    auto& c = app.canvas();
    const auto n = c.nodes()[1];
    CHECK(ElementHandle::find_by_accessible_label(ui, "Delete selection").empty());
    click(ui, c.to_screen({n.pos.x + 30, n.pos.y + 30}), slint::PointerEventButton::Right);
    CHECK((c.selection() == Hit{HitKind::Node, n.id}));  // the right press reached the controller...
    // ...and opened the menu. Headless, the menu is a separate popup window:
    // its entries are visible only as text, at popup coordinates, so choosing
    // one is checked with the real backend (canvas-spike --menu).
    CHECK(!ElementHandle::find_by_accessible_label(ui, "Delete selection").empty());
}

void ui_accented_text(const Ui& ui, SpikeApp& app) {
    auto& c = app.canvas();
    const auto& n = c.nodes()[0];
    click(ui, c.to_screen({n.pos.x + 30, n.pos.y + 30}));
    const auto field = ElementHandle::find_by_accessible_label(ui, "Device name");
    CHECK(field.size() == 1);
    // Click the middle of the field: the Fluent LineEdit ignores presses in
    // its inner padding near the right edge (they did not focus the input).
    const auto at = field[0].absolute_position();
    const auto size = field[0].size();
    const slint::LogicalPosition inside({at.x + size.width / 2, at.y + size.height / 2});
    ui->window().dispatch_pointer_press_event(inside, slint::PointerEventButton::Left);
    ui->window().dispatch_pointer_release_event(inside, slint::PointerEventButton::Left);
    for (const char* text : {"-", "è", "ü", "ñ"}) {
        ui->window().dispatch_key_press_event(text);
        ui->window().dispatch_key_release_event(text);
    }
    key(ui, slint::platform::key_codes::Return);
    CHECK(c.node(n.id)->name == "PC-1-èüñ");
}

void ui_virtualized_events(const Ui& ui) {
    // Only the visible rows of the 20,000 events are instantiated.
    int instantiated = 0;
    ElementHandle::visit_elements(ui, [&](const ElementHandle& item) -> std::optional<int> {
        const auto label = item.accessible_label();
        if (label && std::string_view(*label).starts_with("Event ")) ++instantiated;
        return std::nullopt;
    });
    std::cout << "event rows instantiated: " << instantiated << " of " << ui->get_events()->row_count() << '\n';
    CHECK(instantiated > 0 && instantiated < 100);
    ui->invoke_scroll_events(20000.f * 28);
    CHECK(!ElementHandle::find_by_accessible_label(ui, "Event 19999 · frame forwarded").empty());
}
} // namespace

int main() {
    geometry();
    hit_priority_and_tolerance();
    linking_and_editing();

    slint::testing::init();
    auto ui = SpikeWindow::create();
    ui->window().set_size(slint::LogicalSize({1280, 820}));
    spike::SpikeApp app(ui, false);
    app.small_scene();
    ui->show();
    app.fit();
    ui_cable_by_dragging(ui, app);
    ui_select_cable_and_delete(ui, app);
    ui_scale_factor(ui, app);
    ui_accented_text(ui, app);
    ui_context_menu(ui, app);

    auto big = SpikeWindow::create();
    big->window().set_size(slint::LogicalSize({1280, 820}));
    spike::SpikeApp stress(big, false);
    stress.stress_scene(150, 250, 20000);
    big->show();
    CHECK(stress.canvas().nodes().size() == 150 && stress.canvas().links().size() == 250);
    ui_virtualized_events(big);
    std::cout << "Canvas spike tests passed: " << test::checks << " checks.\n";
}

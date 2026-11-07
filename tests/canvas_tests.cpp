// The canvas: view, hit testing, and the edits pointer and keys make.
#include "app/canvas.hpp"
#include "check.hpp"
#include "explain/explain.hpp"
#include "sim_support.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
using namespace std::chrono_literals;
using namespace simtest;
using netsim::DeviceKind;
using netsim::LinkId;
using netsim::PortRef;
using resend::CanvasController;
using resend::CanvasHit;
using resend::Point;
using resend::Workspace;
using Button = CanvasController::Button;
using Key = CanvasController::Key;
using Kind = CanvasHit::Kind;

bool near(Point a, Point b) { return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f; }

// A switch at (300, 300) and two PCs above it, nothing plugged.
netsim::Scenario bench() {
    auto scenario = star(2);
    scenario.links.clear();
    scenario.devices[0].x = 300;
    scenario.devices[0].y = 300;
    scenario.devices[1].x = 100;
    scenario.devices[1].y = 0;
    scenario.devices[2].x = 500;
    scenario.devices[2].y = 0;
    return scenario;
}
const DeviceId sw{1}, pc_a{2}, pc_b{3};

// The centre of a box of a card, in screen pixels.
Point at(const CanvasController& canvas, const resend::Box& box) { return canvas.to_screen(box.center()); }
Point port(const CanvasController& canvas, const Workspace& workspace, DeviceId id, int number) {
    return at(canvas, resend::port_light(*workspace.scenario().device(id), number));
}
Point body(const CanvasController& canvas, const Workspace& workspace, DeviceId id) {
    const auto* device = workspace.scenario().device(id);
    return canvas.to_screen({static_cast<float>(device->x) + 40, static_cast<float>(device->y) + 20});
}

void view() {
    Workspace workspace(bench());
    CanvasController canvas(workspace);
    CHECK(canvas.resize(1000, 600) == CanvasController::View);
    // Fit: the cards and the room for chips above them, centred, at most 100 %.
    CHECK(canvas.zoom() == 1.0f);
    const auto top_left = canvas.to_screen({100, -resend::marker_room});
    const auto bottom_right = canvas.to_screen({660, 394});
    CHECK(std::abs(top_left.x - (1000 - bottom_right.x)) < 1e-3f && std::abs(top_left.y - (600 - bottom_right.y)) < 1e-3f);
    CHECK(near(canvas.to_canvas(canvas.to_screen({12, 34})), {12, 34}));
    // A small canvas zooms out, down to the limit; resizing follows the fit.
    canvas.resize(300, 200);
    CHECK(canvas.zoom() == CanvasController::min_zoom && canvas.compact());
    canvas.resize(700, 500);
    CHECK(canvas.zoom() > CanvasController::min_zoom && canvas.zoom() < 1);

    // The wheel zooms around the pointer, which stays over the same point.
    const Point pointer{250, 120};
    const auto under = canvas.to_canvas(pointer);
    canvas.zoom_at(pointer, 1.5f);
    CHECK(near(canvas.to_canvas(pointer), under));
    // Once the user zooms, resizing leaves the view alone.
    const auto zoom = canvas.zoom();
    canvas.resize(1200, 900);
    CHECK(canvas.zoom() == zoom);
    canvas.zoom_by(100);
    CHECK(canvas.zoom() == CanvasController::max_zoom);
    canvas.reset_zoom();
    CHECK(canvas.zoom() == 1 && near(canvas.to_screen({100, 0}), {40, 60}));
    CHECK(canvas.key(Key::Fit) == CanvasController::View && canvas.zoom() == 1);
    // Nothing to fit: the origin near the top left.
    Workspace empty(netsim::Scenario{});
    CanvasController blank(empty);
    blank.resize(800, 600);
    CHECK(blank.zoom() == 1 && near(blank.to_screen({0, 0}), {40, 60}));

    // A new card goes to the middle of the view, then next to the cards
    // already there: ten in a row, and none covers another.
    const auto card = blank.new_card_place();
    CHECK(near(blank.to_screen({card.x + resend::card_width / 2, card.y + resend::card_height / 2}), {400, 300}));
    for (int added = 0; added < 10; ++added) {
        const auto place = blank.new_card_place();
        CHECK(empty.change(netsim::AddDevice{netsim::DeviceKind::Host, static_cast<int>(std::lround(place.x)), static_cast<int>(std::lround(place.y))}, "Add").ok());
    }
    const auto& cards = empty.scenario().devices;
    for (std::size_t a = 0; a < cards.size(); ++a)
        for (std::size_t b = a + 1; b < cards.size(); ++b)
            CHECK(static_cast<float>(std::abs(cards[a].x - cards[b].x)) >= resend::card_width ||
                  static_cast<float>(std::abs(cards[a].y - cards[b].y)) >= resend::card_height);
    // The first ones stay near the middle.
    CHECK(std::abs(cards[1].x - cards[0].x) + std::abs(cards[1].y - cards[0].y) < 300);
}

void hit_testing() {
    auto scenario = bench();
    scenario.links.push_back({LinkId{1}, PortRef{pc_a, 0}, PortRef{sw, 0}});
    Workspace workspace(scenario);
    CanvasController canvas(workspace);
    canvas.resize(1000, 700);
    const auto& devices = workspace.scenario().devices;
    CHECK((canvas.hit_test(port(canvas, workspace, sw, 3)) == CanvasHit{Kind::Port, 1, 3}));
    CHECK((canvas.hit_test(port(canvas, workspace, pc_a, 0)) == CanvasHit{Kind::Port, 2, 0}));
    CHECK((canvas.hit_test(at(canvas, resend::connector_box(devices[0]))) == CanvasHit{Kind::Connector, 1}));
    CHECK((canvas.hit_test(body(canvas, workspace, pc_b)) == CanvasHit{Kind::Device, 3}));
    CHECK(canvas.hit_test({1, 1}).kind == Kind::None);
    // A cable is hit a few pixels away from its line, not further.
    const auto segment = *canvas.cable_segment(LinkId{1});
    const float dx = segment.b.x - segment.a.x, dy = segment.b.y - segment.a.y, length = std::hypot(dx, dy);
    const Point normal{-dy / length, dx / length};
    // A point `pixels` away from the middle of the cable, at the current zoom.
    const auto off = [&](float pixels) {
        const auto middle = canvas.to_screen(resend::lerp(segment.a, segment.b, 0.5f));
        return Point{middle.x + normal.x * pixels, middle.y + normal.y * pixels};
    };
    CHECK((canvas.hit_test(off(5)) == CanvasHit{Kind::Cable, 1}));
    CHECK(canvas.hit_test(off(8)).kind == Kind::None);
    // Zoomed out, the cards are simpler: no ports or connector to hit.
    canvas.zoom_at({500, 350}, 0.5f);
    CHECK(canvas.compact());
    CHECK(canvas.hit_test(port(canvas, workspace, sw, 3)).kind == Kind::Device);
    CHECK((canvas.hit_test(off(5)) == CanvasHit{Kind::Cable, 1}));
}

void dragging_cards() {
    Workspace workspace(bench());
    CanvasController canvas(workspace);
    canvas.resize(1000, 700);
    const auto first = workspace.scenario();
    const auto start = body(canvas, workspace, pc_a);
    CHECK(canvas.press(start, Button::Left) == CanvasController::Selection);
    CHECK((canvas.selection() == resend::Selection{resend::Selection::Kind::Device, 2}));
    // A press that hardly moves is a click.
    CHECK(canvas.move({start.x + 1, start.y + 1}) == CanvasController::None);
    CHECK(workspace.scenario() == first);
    for (int step = 1; step <= 20; ++step) canvas.move({start.x + 5.0f * static_cast<float>(step), start.y + 2.0f * static_cast<float>(step)});
    canvas.release({start.x + 100, start.y + 40});
    const auto* moved = workspace.scenario().device(pc_a);
    CHECK(std::abs(moved->x - (100 + std::lround(100 / canvas.zoom()))) <= 1 && std::abs(moved->y - std::lround(40 / canvas.zoom())) <= 1);
    // The whole drag is one step.
    CHECK(workspace.history().size() == 1 && workspace.history().undoable()->label == "Move PC-2");
    CHECK(workspace.undo().ok() && workspace.scenario() == first);

    // Arrows move the selected card, each press a step; Shift for one unit.
    canvas.press(body(canvas, workspace, pc_b), Button::Left);
    canvas.release(body(canvas, workspace, pc_b));
    CHECK(canvas.key(Key::Right) == CanvasController::Network && canvas.key(Key::Down, true) == CanvasController::Network);
    CHECK(workspace.scenario().device(pc_b)->x == 510 && workspace.scenario().device(pc_b)->y == 1);
    CHECK(workspace.history().size() == 2);

    // Dragging the background pans; the network does not move.
    const auto pan = canvas.pan();
    canvas.press({5, 5}, Button::Left);
    CHECK(canvas.selection().kind == resend::Selection::Kind::None);
    CHECK(canvas.move({45, 25}) == CanvasController::View);
    canvas.release({45, 25});
    CHECK(near(canvas.pan(), {pan.x + 40, pan.y + 20}));
}

void drawing_cables() {
    Workspace workspace(bench());
    CanvasController canvas(workspace);
    canvas.resize(1000, 700);
    // Port to port: from the switch's fourth port to PC-A's port.
    canvas.press(port(canvas, workspace, sw, 3), Button::Left);
    CHECK(canvas.drawing_cable() && canvas.rubber_band().has_value());
    canvas.move({600, 200});
    CHECK(near(canvas.rubber_band()->b, {600, 200}));
    canvas.release(port(canvas, workspace, pc_a, 0));
    CHECK(!canvas.drawing_cable() && workspace.scenario().links.size() == 1);
    const auto& cable = workspace.scenario().links.back();
    CHECK((cable.a == PortRef{sw, 3} && cable.b == PortRef{pc_a, 0}));
    CHECK((canvas.selection() == resend::Selection{resend::Selection::Kind::Cable, cable.id.value}));
    CHECK(canvas.status() == "Cable plugged: Switch-1 port 4 — PC-2 port 1.");
    CHECK(workspace.history().undoable()->label == "Connect Switch-1 port 4 — PC-2 port 1");

    // A used port selects its cable instead.
    canvas.press(port(canvas, workspace, sw, 3), Button::Left);
    canvas.release(port(canvas, workspace, sw, 3));
    CHECK(!canvas.drawing_cable() && canvas.selection().kind == resend::Selection::Kind::Cable);

    // From a connector onto a card: the first free ports.
    const auto& devices = workspace.scenario().devices;
    canvas.press(at(canvas, resend::connector_box(devices[2])), Button::Left);
    canvas.release(body(canvas, workspace, sw));
    CHECK((workspace.scenario().links.back().a == PortRef{pc_b, 0} && workspace.scenario().links.back().b == PortRef{sw, 0}));

    // Click and click: a connector, then a card. Escape or a click on the
    // same connector cancels, the background too.
    CHECK(workspace.change(netsim::AddDevice{DeviceKind::Host, 700, 300}, "Add").ok());
    const DeviceId pc_c{4};
    const auto connector = [&] { return at(canvas, resend::connector_box(*workspace.scenario().device(pc_c))); };
    canvas.press(connector(), Button::Left);
    canvas.release(connector());
    CHECK(canvas.drawing_cable() && canvas.cable_source()->device == pc_c);
    CHECK(canvas.key(Key::Escape) != CanvasController::None && !canvas.drawing_cable());
    canvas.press(connector(), Button::Left);
    canvas.release(connector());
    canvas.press(connector(), Button::Left);
    canvas.release(connector());
    CHECK(!canvas.drawing_cable() && canvas.status() == "Connection cancelled.");
    canvas.press(connector(), Button::Left);
    canvas.release(connector());
    canvas.press({2, 2}, Button::Left);
    CHECK(!canvas.drawing_cable() && workspace.scenario().links.size() == 2);
    canvas.press(connector(), Button::Left);
    canvas.release(connector());
    canvas.move(port(canvas, workspace, sw, 6));
    CHECK((canvas.hover() == CanvasHit{Kind::Port, 1, 6}));
    canvas.press(port(canvas, workspace, sw, 6), Button::Left);
    CHECK((workspace.scenario().links.back().a == PortRef{pc_c, 0} && workspace.scenario().links.back().b == PortRef{sw, 6}));

    // Refused: a port already used, the same device, a device without a free port.
    const auto links = workspace.scenario().links.size();
    canvas.press(port(canvas, workspace, sw, 1), Button::Left);
    canvas.release(port(canvas, workspace, sw, 2));
    CHECK(workspace.scenario().links.size() == links && canvas.status() == netsim::explain::describe(Problem::SameDevice));
    canvas.press(port(canvas, workspace, sw, 1), Button::Left);
    canvas.release(body(canvas, workspace, pc_a));
    CHECK(workspace.scenario().links.size() == links);
    canvas.press(port(canvas, workspace, sw, 1), Button::Left);
    canvas.release({3, 3});
    CHECK(workspace.scenario().links.size() == links && canvas.status() == "Connection cancelled: a cable needs another device.");

    // The keyboard way: Enter on two devices.
    CHECK(workspace.change(netsim::AddDevice{DeviceKind::Host, 900, 300}, "Add").ok());
    canvas.select({resend::Selection::Kind::Device, 5});
    canvas.key(Key::Connect);
    CHECK(canvas.drawing_cable());
    canvas.select({resend::Selection::Kind::Device, 1});
    canvas.key(Key::Connect);
    CHECK((workspace.scenario().links.back().a == PortRef{DeviceId{5}, 0} && workspace.scenario().links.back().b == PortRef{sw, 1}));

    // Cables as paths: the selected one apart, down ones apart.
    CHECK(workspace.change(netsim::SetLinkUp{LinkId{1}, false}, "Disable").ok());
    canvas.select({resend::Selection::Kind::Cable, 2});
    const auto paths = canvas.cable_paths({3});
    CHECK(paths.selected.starts_with("M") && paths.selected.find('M', 1) == std::string::npos);
    CHECK(paths.down.starts_with("M") && paths.highlight.starts_with("M"));
    CHECK(std::count(paths.up.begin(), paths.up.end(), 'M') == 1);
}

void removing() {
    auto scenario = bench();
    scenario.links.push_back({LinkId{1}, PortRef{pc_a, 0}, PortRef{sw, 0}});
    scenario.links.push_back({LinkId{2}, PortRef{pc_b, 0}, PortRef{sw, 1}});
    Workspace workspace(scenario);
    CanvasController canvas(workspace);
    canvas.resize(1000, 700);
    const auto segment = *canvas.cable_segment(LinkId{2});
    canvas.press(canvas.to_screen(resend::lerp(segment.a, segment.b, 0.5f)), Button::Left);
    CHECK((canvas.selection() == resend::Selection{resend::Selection::Kind::Cable, 2}));
    CHECK(canvas.key(Key::Delete) == (CanvasController::Network | CanvasController::Selection));
    CHECK(workspace.scenario().links.size() == 1 && workspace.history().undoable()->label == "Unplug PC-3 port 1 — Switch-1 port 2");
    canvas.select({resend::Selection::Kind::Device, 1});
    canvas.key(Key::Delete);
    CHECK(workspace.scenario().devices.size() == 2 && workspace.scenario().links.empty());
    CHECK(canvas.selection().kind == resend::Selection::Kind::None);
    CHECK(canvas.key(Key::Delete) == CanvasController::None);
    // Undo brings back what the selection pointed at; a selection of
    // something gone is dropped.
    canvas.select({resend::Selection::Kind::Cable, 1});
    CHECK(canvas.validate() == CanvasController::Selection);  // it went with the switch
    CHECK(workspace.undo().ok() && workspace.undo().ok() && workspace.scenario() == scenario);
    canvas.select({resend::Selection::Kind::Device, 3});
    CHECK(workspace.redo().ok() && canvas.validate() == CanvasController::None);
    canvas.select({resend::Selection::Kind::Cable, 2});
    CHECK(workspace.redo().ok() && canvas.validate() == CanvasController::Selection);
}

void past_instants() {
    // Back in time, the canvas changes nothing and says why.
    Workspace workspace;
    CHECK(workspace.ping(DeviceId{1}, ip("192.168.1.20")).ok());
    workspace.run_until(15ms);
    CHECK(workspace.change(netsim::SetLinkUp{LinkId{2}, false}, "Disable").ok());
    workspace.run_until(30ms);
    workspace.travel(5ms);
    CanvasController canvas(workspace);
    canvas.resize(1000, 700);
    const auto before = workspace.scenario();
    const auto start = body(canvas, workspace, DeviceId{1});
    canvas.press(start, Button::Left);
    canvas.move({start.x + 50, start.y});
    canvas.release({start.x + 50, start.y});
    CHECK(workspace.scenario() == before && canvas.refused_in_past());
    CHECK(canvas.status().starts_with("The instant shown is before later changes"));
    canvas.select({resend::Selection::Kind::Device, 1});
    CHECK(canvas.key(Key::Delete) == CanvasController::None && workspace.scenario() == before);
}

} // namespace

int main() {
    view();
    hit_testing();
    dragging_cards();
    drawing_cables();
    removing();
    past_instants();
    std::cout << "Canvas tests passed: " << test::checks << " checks.\n";
}

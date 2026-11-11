#include "app/canvas.hpp"

#include "core/text.hpp"
#include "explain/explain.hpp"
#include "scenario/edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>

namespace resend {
namespace {
using netsim::text::tr;

float distance(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }

float distance_to_segment(Point p, Point a, Point b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float squared = dx * dx + dy * dy;
    const float t = squared == 0 ? 0 : std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / squared, 0.0f, 1.0f);
    return distance(p, {a.x + dx * t, a.y + dy * t});
}

bool inside(const Box& box, Point p, float margin = 0) {
    return p.x >= box.x - margin && p.y >= box.y - margin && p.x <= box.x + box.width + margin && p.y <= box.y + box.height + margin;
}

int canvas_units(float value) {
    return static_cast<int>(std::lround(std::clamp(value, -static_cast<float>(netsim::max_coordinate), static_cast<float>(netsim::max_coordinate))));
}

void append(std::string& path, const Segment& segment) {
    char buffer[96];
    const int length = std::snprintf(buffer, sizeof buffer, "M%.1f %.1fL%.1f %.1f", segment.a.x, segment.a.y, segment.b.x, segment.b.y);
    path.append(buffer, static_cast<std::size_t>(length));
}
} // namespace

unsigned CanvasController::resize(float width, float height) {
    if (width == viewport_.x && height == viewport_.y) return None;
    viewport_ = {width, height};
    return follow_ ? fit() : None;
}

unsigned CanvasController::set_free_area(Box area) {
    if (area.x == free_.x && area.y == free_.y && area.width == free_.width && area.height == free_.height) return None;
    free_ = area;
    return follow_ ? fit() : None;
}

// The free area within the canvas; the whole canvas when the HUD leaves too
// little of it (less than a card and its margin), or none was given.
Box CanvasController::free_area() const {
    const float left = std::clamp(free_.x, 0.0f, viewport_.x), top = std::clamp(free_.y, 0.0f, viewport_.y);
    const float right = std::clamp(free_.x + free_.width, left, viewport_.x);
    const float bottom = std::clamp(free_.y + free_.height, top, viewport_.y);
    if (right - left < card_width + 40 || bottom - top < card_height + 40) return {0, 0, viewport_.x, viewport_.y};
    return {left, top, right - left, bottom - top};
}

unsigned CanvasController::zoom_at(Point screen, float factor) {
    const float next = std::clamp(zoom_ * factor, min_zoom, max_zoom);
    pan_ = {screen.x - (screen.x - pan_.x) * next / zoom_, screen.y - (screen.y - pan_.y) * next / zoom_};
    zoom_ = next;
    follow_ = false;
    return View;
}

unsigned CanvasController::zoom_by(float factor) { return zoom_at(free_area().center(), factor); }

unsigned CanvasController::reset_zoom() {
    zoom_ = 1;
    pan_ = {40, 60};
    if (!scenario().devices.empty()) {
        const auto& devices = scenario().devices;
        const auto left = std::min_element(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return a.x < b.x; })->x;
        const auto top = std::min_element(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return a.y < b.y; })->y;
        pan_ = {40 - static_cast<float>(left), 60 - static_cast<float>(top)};
    }
    follow_ = false;
    return View;
}

unsigned CanvasController::fit() {
    follow_ = true;
    const auto& devices = scenario().devices;
    if (devices.empty() || viewport_.x <= 0 || viewport_.y <= 0) {
        zoom_ = 1;
        pan_ = {40, 60};
        return View;
    }
    float left = std::numeric_limits<float>::max(), top = left;
    float right = std::numeric_limits<float>::lowest(), bottom = right;
    for (const auto& device : devices) {
        const auto card = card_box(device);
        left = std::min(left, card.x);
        top = std::min(top, card.y - marker_room);
        right = std::max(right, card.x + card.width);
        bottom = std::max(bottom, card.y + card.height);
    }
    constexpr float margin = 24;
    const auto area = free_area();
    zoom_ = std::clamp(std::min((area.width - 2 * margin) / (right - left), (area.height - 2 * margin) / (bottom - top)), min_zoom, 1.0f);
    pan_ = {area.x + (area.width - (right - left) * zoom_) / 2 - left * zoom_, area.y + (area.height - (bottom - top) * zoom_) / 2 - top * zoom_};
    return View;
}

Point CanvasController::new_card_place() const {
    const auto centre = to_canvas(free_area().center());
    const Point wanted{centre.x - card_width / 2, centre.y - card_height / 2};
    // Cards keep a gap for the cables and the markers above them.
    constexpr float gap = 30;
    const auto& devices = scenario().devices;
    const auto free = [&](Point at) {
        return std::none_of(devices.begin(), devices.end(), [&](const netsim::DeviceConfig& device) {
            return std::abs(static_cast<float>(device.x) - at.x) < card_width + gap &&
                   std::abs(static_cast<float>(device.y) - at.y) < card_height + gap;
        });
    };
    // Rings of places around the middle: a ring holds no place nearer than
    // its own distance, so the search stops once a ring cannot do better.
    constexpr float step = 20;
    std::optional<Point> best;
    float nearest = std::numeric_limits<float>::max();
    for (int ring = 0; ring <= 40; ++ring) {
        for (int i = -ring; i <= ring; ++i) {
            for (int j = -ring; j <= ring; ++j) {
                if (std::max(std::abs(i), std::abs(j)) != ring) continue;
                const Point at{wanted.x + static_cast<float>(i) * step, wanted.y + static_cast<float>(j) * step};
                if (free(at) && distance(at, wanted) < nearest) {
                    best = at;
                    nearest = distance(at, wanted);
                }
            }
        }
        if (best && nearest < static_cast<float>(ring + 1) * step) break;
    }
    return best.value_or(wanted);
}

CanvasHit CanvasController::hit_test(Point screen) const {
    const auto point = to_canvas(screen);
    // A few pixels of slack around the small targets, whatever the zoom.
    const float slack = 3 / zoom_;
    const auto& devices = scenario().devices;
    // Later cards are drawn on top, so they are tested first.
    for (auto device = devices.rbegin(); device != devices.rend(); ++device) {
        if (!inside(card_box(*device), point, slack)) continue;
        if (!compact()) {
            if (inside(connector_box(*device), point, slack)) return {CanvasHit::Kind::Connector, device->id.value};
            for (int port = 0; port < static_cast<int>(device->ports.size()); ++port)
                if (inside(port_light(*device, port), point, slack)) return {CanvasHit::Kind::Port, device->id.value, port};
        }
        if (inside(card_box(*device), point)) return {CanvasHit::Kind::Device, device->id.value};
    }
    CanvasHit best;
    float nearest = cable_tolerance;
    for (const auto& link : scenario().links) {
        const auto segment = cable_segment(link.id);
        if (!segment) continue;
        const float gap = distance_to_segment(screen, to_screen(segment->a), to_screen(segment->b));
        if (gap <= nearest) {
            nearest = gap;
            best = {CanvasHit::Kind::Cable, link.id.value};
        }
    }
    return best;
}

bool CanvasController::change(const netsim::Edit& edit, const std::string& label, bool gesture) {
    const auto outcome = workspace_.change(edit, label, gesture);
    refused_in_past_ = outcome.in_past;
    if (outcome.in_past) status_ = tr("The instant shown is before later changes: go to the latest instant to change the network.");
    else if (!outcome.ok()) status_ = why_refused(outcome);
    return outcome.ok();
}

unsigned CanvasController::select(resend::Selection selection) {
    if (selection == selection_) return None;
    selection_ = selection;
    return Selection;
}

unsigned CanvasController::validate() {
    unsigned changes = None;
    const bool gone = (selection_.kind == Selection::Kind::Device && !scenario().device(netsim::DeviceId{selection_.id})) ||
        (selection_.kind == Selection::Kind::Cable && !scenario().link(netsim::LinkId{selection_.id}));
    if (gone) changes |= select({});
    if (source_ && !scenario().device(source_->device)) {
        source_.reset();
        if (mode_ == Mode::Cable) mode_ = Mode::Idle;
        changes |= Selection | Overlay;
    }
    return changes;
}

unsigned CanvasController::press(Point screen, Button button) {
    pointer_ = press_ = screen;
    refused_in_past_ = false;
    const auto hit = hit_test(screen);
    if (button == Button::Middle) {
        mode_ = Mode::Panning;
        press_pan_ = pan_;
        return None;
    }
    if (button == Button::Right) {
        // The context menu acts on what is under the pointer.
        if (hit.kind == CanvasHit::Kind::Cable) return select({Selection::Kind::Cable, hit.id});
        if (hit.kind != CanvasHit::Kind::None) return select({Selection::Kind::Device, hit.id});
        return select({});
    }
    // A cable started with a click on a connector ends with a click elsewhere.
    if (source_ && mode_ == Mode::Idle) {
        const auto from = *source_;
        source_.reset();
        if (hit.kind == CanvasHit::Kind::Connector && hit.id == from.device.value) {
            status_ = tr("Connection cancelled.");
            return Selection | Overlay;
        }
        return connect(from, hit) | Overlay;
    }
    switch (hit.kind) {
    case CanvasHit::Kind::Port: {
        const netsim::PortRef port{netsim::DeviceId{hit.id}, static_cast<std::uint8_t>(hit.port)};
        if (const auto* cable = scenario().link_at(port)) return select({Selection::Kind::Cable, cable->id.value});
        source_ = port;
        mode_ = Mode::Cable;
        status_ = tr("Release on a free port or on another device.");
        return select({Selection::Kind::Device, hit.id}) | Overlay;
    }
    case CanvasHit::Kind::Connector:
        source_ = netsim::PortRef{netsim::DeviceId{hit.id}, 0xff};
        mode_ = Mode::Cable;
        status_ = tr("Release on another device or one of its ports, or click it.");
        return select({Selection::Kind::Device, hit.id}) | Overlay;
    case CanvasHit::Kind::Device: {
        const auto* device = scenario().device(netsim::DeviceId{hit.id});
        mode_ = Mode::Pressing;
        pressed_device_ = hit.id;
        const auto point = to_canvas(screen);
        grab_ = {point.x - static_cast<float>(device->x), point.y - static_cast<float>(device->y)};
        return select({Selection::Kind::Device, hit.id});
    }
    case CanvasHit::Kind::Cable:
        return select({Selection::Kind::Cable, hit.id});
    case CanvasHit::Kind::None:
        mode_ = Mode::Panning;
        press_pan_ = pan_;
        return select({});
    }
    return None;
}

unsigned CanvasController::move(Point screen) {
    pointer_ = screen;
    switch (mode_) {
    case Mode::Panning:
        pan_ = {press_pan_.x + screen.x - press_.x, press_pan_.y + screen.y - press_.y};
        follow_ = false;
        return View;
    case Mode::Pressing:
        if (distance(screen, press_) < drag_threshold) return None;
        mode_ = Mode::Dragging;
        [[fallthrough]];
    case Mode::Dragging: {
        const auto* device = scenario().device(netsim::DeviceId{pressed_device_});
        if (!device) return None;
        const auto point = to_canvas(screen);
        const netsim::MoveDevice edit{device->id, canvas_units(point.x - grab_.x), canvas_units(point.y - grab_.y)};
        if (edit.x == device->x && edit.y == device->y) return None;
        if (!change(edit, tr("Move {0}", {device->name}), true)) {
            mode_ = Mode::Idle;
            return None;
        }
        return Network;
    }
    case Mode::Cable:
    case Mode::Idle: {
        const auto hit = hit_test(screen);
        if (hit == hover_) return source_ ? Overlay : None;
        hover_ = hit;
        return Overlay;
    }
    }
    return None;
}

unsigned CanvasController::release(Point screen) {
    pointer_ = screen;
    const auto mode = std::exchange(mode_, Mode::Idle);
    if (mode == Mode::Dragging) workspace_.end_gesture();
    if (mode != Mode::Cable || !source_) return None;
    const auto hit = hit_test(screen);
    // Released where it started: the click-and-click way, the next click ends it.
    if ((hit.kind == CanvasHit::Kind::Connector || hit.kind == CanvasHit::Kind::Port) && hit.id == source_->device.value &&
        distance(screen, press_) < drag_threshold) {
        status_ = tr("Now click another device or one of its free ports. Escape cancels.");
        return Overlay;
    }
    const auto from = *source_;
    source_.reset();
    return connect(from, hit) | Overlay;
}

unsigned CanvasController::connect(netsim::PortRef from, const CanvasHit& target) {
    const auto* other = target.kind == CanvasHit::Kind::None || target.kind == CanvasHit::Kind::Cable ? nullptr
        : scenario().device(netsim::DeviceId{target.id});
    if (!other) {
        status_ = tr("Connection cancelled: a cable needs another device.");
        return Selection;
    }
    auto problem = netsim::Problem::None;
    std::optional<std::uint8_t> a = from.port == 0xff ? netsim::edit::free_port(scenario(), from.device) : from.port;
    std::optional<std::uint8_t> b = target.kind == CanvasHit::Kind::Port ? static_cast<std::uint8_t>(target.port)
                                                                        : netsim::edit::free_port(scenario(), other->id);
    if (other->id == from.device) problem = netsim::Problem::SameDevice;
    else if (!a || !b) problem = netsim::Problem::NoFreePort;
    if (!netsim::ok(problem)) {
        status_ = netsim::explain::describe(problem);
        return Selection;
    }
    const netsim::Connect edit{{from.device, *a}, {other->id, *b}};
    const auto ends = netsim::explain::port_text(scenario(), edit.a) + " — " + netsim::explain::port_text(scenario(), edit.b);
    if (!change(edit, tr("Connect {0}", {ends}))) return Selection;
    status_ = tr("Cable plugged: {0}.", {ends});
    return Network | select({Selection::Kind::Cable, scenario().links.back().id.value}) | Selection;
}

unsigned CanvasController::remove_selection() {
    if (selection_.kind == Selection::Kind::Device) {
        const auto* device = scenario().device(netsim::DeviceId{selection_.id});
        if (!device || !change(netsim::RemoveDevice{device->id}, tr("Remove {0}", {device->name}))) return None;
        status_ = tr("Device and its cables removed.");
    } else if (selection_.kind == Selection::Kind::Cable) {
        const auto* cable = scenario().link(netsim::LinkId{selection_.id});
        if (!cable) return None;
        const auto ends = netsim::explain::port_text(scenario(), cable->a) + " — " + netsim::explain::port_text(scenario(), cable->b);
        if (!change(netsim::Disconnect{cable->id}, tr("Unplug {0}", {ends}))) return None;
        status_ = tr("Cable unplugged.");
    } else {
        return None;
    }
    return Network | select({});
}

unsigned CanvasController::nudge(int dx, int dy) {
    const auto* device = selection_.kind == Selection::Kind::Device ? scenario().device(netsim::DeviceId{selection_.id}) : nullptr;
    if (!device) return None;
    return change(netsim::MoveDevice{device->id, device->x + dx, device->y + dy}, tr("Move {0}", {device->name})) ? Network : None;
}

unsigned CanvasController::connector(std::uint32_t device) {
    refused_in_past_ = false;
    if (!scenario().device(netsim::DeviceId{device})) return None;
    if (!source_) {
        source_ = netsim::PortRef{netsim::DeviceId{device}, 0xff};
        status_ = tr("Now click another device or one of its free ports. Escape cancels.");
        return select({Selection::Kind::Device, device}) | Overlay;
    }
    const auto from = *source_;
    source_.reset();
    if (from.device.value == device) {
        status_ = tr("Connection cancelled.");
        return Selection | Overlay;
    }
    return connect(from, {CanvasHit::Kind::Device, device}) | Overlay;
}

unsigned CanvasController::key(Key key, bool fine) {
    refused_in_past_ = false;
    const int step = fine ? 1 : 10;
    switch (key) {
    case Key::Left: return nudge(-step, 0);
    case Key::Right: return nudge(step, 0);
    case Key::Up: return nudge(0, -step);
    case Key::Down: return nudge(0, step);
    case Key::Delete: return remove_selection();
    case Key::Escape:
        if (source_) {
            source_.reset();
            mode_ = Mode::Idle;
            status_ = tr("Connection cancelled.");
            return Selection | Overlay;
        }
        return select({});
    case Key::ZoomIn: return zoom_by(1.15f);
    case Key::ZoomOut: return zoom_by(1 / 1.15f);
    case Key::Fit: return fit();
    case Key::Connect: {
        // The keyboard way: Enter on a device, then Enter on another one.
        if (selection_.kind != Selection::Kind::Device) return None;
        if (!source_) {
            source_ = netsim::PortRef{netsim::DeviceId{selection_.id}, 0xff};
            status_ = tr("Select another device and press Enter to connect them. Escape cancels.");
            return Selection | Overlay;
        }
        const auto from = *source_;
        source_.reset();
        if (from.device.value == selection_.id) {
            status_ = tr("Connection cancelled.");
            return Selection | Overlay;
        }
        return connect(from, {CanvasHit::Kind::Device, selection_.id}) | Overlay;
    }
    }
    return None;
}

std::optional<Segment> CanvasController::rubber_band() const {
    if (!source_) return std::nullopt;
    const auto* device = scenario().device(source_->device);
    if (!device) return std::nullopt;
    const auto from = source_->port == 0xff ? connector_box(*device).center() : port_light(*device, source_->port).center();
    return Segment{to_screen(from), pointer_};
}

std::optional<Segment> CanvasController::cable_segment(netsim::LinkId id) const {
    const auto* link = scenario().link(id);
    if (!link) return std::nullopt;
    const auto* a = scenario().device(link->a.device);
    const auto* b = scenario().device(link->b.device);
    if (!a || !b) return std::nullopt;
    return cable_between(card_box(*a), card_box(*b));
}

CanvasController::CablePaths CanvasController::cable_paths(const std::vector<std::uint32_t>& highlight) const {
    CablePaths paths;
    for (const auto& link : scenario().links) {
        const auto segment = cable_segment(link.id);
        if (!segment) continue;
        const Segment screen{to_screen(segment->a), to_screen(segment->b)};
        const bool lit = std::find(highlight.begin(), highlight.end(), link.id.value) != highlight.end();
        if (selection_.kind == Selection::Kind::Cable && selection_.id == link.id.value) append(paths.selected, screen);
        else if (lit) append(paths.highlight, screen);
        else append(link.up ? paths.up : paths.down, screen);
    }
    return paths;
}

} // namespace resend

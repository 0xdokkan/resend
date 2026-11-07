#include "canvas_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace spike {
namespace {
constexpr float min_zoom = 0.25f, max_zoom = 3.0f;
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
} // namespace

float distance_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a, ap = p - a;
    const float squared = ab.x * ab.x + ab.y * ab.y;
    const float t = squared == 0 ? 0 : std::clamp((ap.x * ab.x + ap.y * ab.y) / squared, 0.0f, 1.0f);
    return length(p - Vec2{a.x + ab.x * t, a.y + ab.y * t});
}

int port_count(NodeKind kind) { return kind == NodeKind::Host ? 1 : 8; }

int CanvasController::add_node(NodeKind kind, Vec2 pos) {
    const int id = next_node_++;
    nodes_.push_back({id, kind, pos, (kind == NodeKind::Host ? "PC-" : "SW-") + std::to_string(id)});
    return id;
}

const Node* CanvasController::node(int id) const {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [id](const Node& n) { return n.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}
Node* CanvasController::mutable_node(int id) { return const_cast<Node*>(std::as_const(*this).node(id)); }

bool CanvasController::rename(int id, std::string name) {
    auto* item = mutable_node(id);
    if (!item || name.empty()) return false;
    item->name = std::move(name);
    return true;
}

bool CanvasController::port_used(PortRef port) const {
    return std::any_of(links_.begin(), links_.end(), [&](const Link& l) { return l.a == port || l.b == port; });
}

bool CanvasController::connect(PortRef a, PortRef b) {
    const auto* na = node(a.node);
    const auto* nb = node(b.node);
    if (!na || !nb || a.node == b.node) return false;
    if (a.port < 0 || a.port >= port_count(na->kind) || b.port < 0 || b.port >= port_count(nb->kind)) return false;
    if (port_used(a) || port_used(b)) return false;
    links_.push_back({next_link_++, a, b});
    return true;
}

Vec2 CanvasController::port_anchor(PortRef port) const {
    const auto* item = node(port.node);
    if (!item) return {};
    if (item->kind == NodeKind::Host) return {item->pos.x + card_width / 2, item->pos.y + card_height};
    const float spacing = (card_width - 40) / 7;
    return {item->pos.x + 20 + spacing * static_cast<float>(port.port), item->pos.y + card_height};
}

unsigned CanvasController::zoom_at(Vec2 screen, float factor) {
    const float next = std::clamp(zoom_ * factor, min_zoom, max_zoom);
    pan_ = {screen.x - (screen.x - pan_.x) * next / zoom_, screen.y - (screen.y - pan_.y) * next / zoom_};
    zoom_ = next;
    return View;
}

unsigned CanvasController::fit(Vec2 viewport, float margin) {
    if (nodes_.empty()) {
        zoom_ = 1;
        pan_ = {margin, margin};
        return View;
    }
    float left = std::numeric_limits<float>::max(), top = left;
    float right = std::numeric_limits<float>::lowest(), bottom = right;
    for (const auto& n : nodes_) {
        left = std::min(left, n.pos.x);
        top = std::min(top, n.pos.y);
        right = std::max(right, n.pos.x + card_width);
        bottom = std::max(bottom, n.pos.y + card_height + port_radius);
    }
    zoom_ = std::clamp(std::min((viewport.x - 2 * margin) / (right - left),
                                (viewport.y - 2 * margin) / (bottom - top)), min_zoom, 1.5f);
    pan_ = {(viewport.x - (right - left) * zoom_) / 2 - left * zoom_,
            (viewport.y - (bottom - top) * zoom_) / 2 - top * zoom_};
    return View;
}

Hit CanvasController::hit_test(Vec2 screen, float tolerance) const {
    // Later nodes are drawn on top, so they are tested first.
    for (auto n = nodes_.rbegin(); n != nodes_.rend(); ++n)
        for (int port = 0; port < port_count(n->kind); ++port)
            if (length(screen - to_screen(port_anchor({n->id, port}))) <= port_radius * zoom_ + tolerance)
                return {HitKind::Port, n->id, port};
    for (auto n = nodes_.rbegin(); n != nodes_.rend(); ++n) {
        const Vec2 corner = to_screen(n->pos);
        if (screen.x >= corner.x && screen.y >= corner.y &&
            screen.x <= corner.x + card_width * zoom_ && screen.y <= corner.y + card_height * zoom_)
            return {HitKind::Node, n->id};
    }
    Hit best;
    float best_distance = tolerance;
    for (const auto& l : links_) {
        const float d = distance_to_segment(screen, to_screen(port_anchor(l.a)), to_screen(port_anchor(l.b)));
        if (d <= best_distance) {
            best_distance = d;
            best = {HitKind::Link, l.id};
        }
    }
    return best;
}

unsigned CanvasController::press(Vec2 screen, Button button) {
    const Hit hit = hit_test(screen);
    const Hit previous = selection_;
    pointer_ = screen;
    mode_ = Mode::Idle;
    if (button == Button::Middle || (button == Button::Left && hit.kind == HitKind::None)) {
        mode_ = Mode::Panning;
        press_screen_ = screen;
        press_pan_ = pan_;
        if (button == Button::Left) selection_ = {};
    } else if (button == Button::Right) {
        selection_ = hit.kind == HitKind::Port ? Hit{HitKind::Node, hit.id} : hit;  // target of the context menu
    } else if (hit.kind == HitKind::Port) {
        if (!port_used({hit.id, hit.port})) {
            mode_ = Mode::Linking;
            link_from_ = {hit.id, hit.port};
            status_ = "Release on a free port of another device.";
            return Overlay | (selection_ == previous ? 0u : Selection);
        }
        for (const auto& l : links_)
            if (l.a == PortRef{hit.id, hit.port} || l.b == PortRef{hit.id, hit.port}) selection_ = {HitKind::Link, l.id};
    } else if (hit.kind == HitKind::Node) {
        mode_ = Mode::Dragging;
        selection_ = hit;
        grab_offset_ = to_canvas(screen) - node(hit.id)->pos;
    } else {
        selection_ = hit;
    }
    return selection_ == previous ? None : Selection;
}

unsigned CanvasController::move(Vec2 screen) {
    pointer_ = screen;
    switch (mode_) {
    case Mode::Panning:
        pan_ = press_pan_ + (screen - press_screen_);
        return View;
    case Mode::Dragging:
        if (auto* n = mutable_node(selection_.id)) {
            n->pos = to_canvas(screen) - grab_offset_;
            moved_node_ = n->id;
            return Moved;
        }
        return None;
    case Mode::Linking: {
        const Hit target = hit_test(screen);
        hover_ = target.kind == HitKind::Port ? target : Hit{};
        return Overlay;
    }
    case Mode::Idle:
        break;
    }
    return None;
}

unsigned CanvasController::release(Vec2 screen) {
    unsigned changes = None;
    if (mode_ == Mode::Linking) {
        const Hit target = hit_test(screen);
        if (target.kind == HitKind::Port && connect(link_from_, {target.id, target.port})) {
            selection_ = {HitKind::Link, links_.back().id};
            status_ = "Link created.";
            changes = Topology | Selection;
        } else {
            status_ = "Link cancelled: it needs a free port of another device.";
        }
        hover_ = {};
        changes |= Overlay;
    }
    mode_ = Mode::Idle;
    return changes;
}

unsigned CanvasController::delete_selection() {
    if (selection_.kind == HitKind::Node) {
        const int id = selection_.id;
        std::erase_if(links_, [id](const Link& l) { return l.a.node == id || l.b.node == id; });
        std::erase_if(nodes_, [id](const Node& n) { return n.id == id; });
    } else if (selection_.kind == HitKind::Link) {
        const int id = selection_.id;
        std::erase_if(links_, [id](const Link& l) { return l.id == id; });
    } else {
        return None;
    }
    selection_ = {};
    return Topology | Selection;
}

unsigned CanvasController::key(Key key) {
    switch (key) {
    case Key::Delete:
        return delete_selection();
    case Key::Escape:
        if (mode_ == Mode::Linking) {
            mode_ = Mode::Idle;
            hover_ = {};
            status_ = "Link cancelled.";
            return Overlay;
        }
        if (selection_.kind == HitKind::None) return None;
        selection_ = {};
        return Selection;
    case Key::Left: case Key::Right: case Key::Up: case Key::Down: {
        auto* n = selection_.kind == HitKind::Node ? mutable_node(selection_.id) : nullptr;
        if (!n) return None;
        n->pos.x += key == Key::Left ? -10.f : key == Key::Right ? 10.f : 0.f;
        n->pos.y += key == Key::Up ? -10.f : key == Key::Down ? 10.f : 0.f;
        moved_node_ = n->id;
        return Moved;
    }
    case Key::ZoomIn:
        return zoom_at(pointer_, 1.15f);
    case Key::ZoomOut:
        return zoom_at(pointer_, 1 / 1.15f);
    case Key::Fit: case Key::Other:
        break;
    }
    return None;
}

std::optional<std::pair<Vec2, Vec2>> CanvasController::rubber_band() const {
    if (mode_ != Mode::Linking) return std::nullopt;
    return std::pair{to_screen(port_anchor(link_from_)), pointer_};
}

std::string CanvasController::link_path(bool selected) const {
    std::string out;
    out.reserve(links_.size() * 40);
    char buffer[64];
    for (const auto& l : links_) {
        if ((selection_.kind == HitKind::Link && selection_.id == l.id) != selected) continue;
        const Vec2 a = to_screen(port_anchor(l.a)), b = to_screen(port_anchor(l.b));
        const int n = std::snprintf(buffer, sizeof buffer, "M%.1f %.1fL%.1f %.1f", a.x, a.y, b.x, b.y);
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

} // namespace spike

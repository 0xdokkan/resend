#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Canvas geometry and interaction for the UI spike. Plain C++ with no Slint
// dependency: Slint only draws what this class computes and forwards input.
namespace spike {

struct Vec2 { float x = 0, y = 0; };

enum class NodeKind { Host, Switch };
struct PortRef {
    int node = -1, port = -1;
    bool operator==(const PortRef&) const = default;
};
struct Node { int id = 0; NodeKind kind = NodeKind::Host; Vec2 pos; std::string name; };
struct Link { int id = 0; PortRef a, b; };

enum class HitKind { None, Node, Port, Link };
struct Hit {
    HitKind kind = HitKind::None;
    int id = -1;    // node or link id
    int port = -1;  // only for HitKind::Port
    bool operator==(const Hit&) const = default;
};

enum class Button { Left, Middle, Right };
enum class Key { Delete, Escape, Left, Right, Up, Down, ZoomIn, ZoomOut, Fit, Other };

// What an input changed, so the UI glue updates only the affected models.
enum Change : unsigned {
    None = 0,
    View = 1,      // zoom or pan
    Moved = 2,     // node `moved_node` changed position
    Topology = 4,  // nodes or links added/removed
    Selection = 8,
    Overlay = 16,  // rubber band or hover
};

// Card size and port layout, in canvas units.
constexpr float card_width = 160;
constexpr float card_height = 94;
constexpr float port_radius = 7;

float distance_to_segment(Vec2 p, Vec2 a, Vec2 b);
int port_count(NodeKind kind);

class CanvasController {
public:
    // Scene
    int add_node(NodeKind kind, Vec2 pos);
    bool connect(PortRef a, PortRef b);
    bool port_used(PortRef port) const;
    Vec2 port_anchor(PortRef port) const;  // canvas units
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Link>& links() const { return links_; }
    const Node* node(int id) const;
    bool rename(int id, std::string name);

    // View: screen = canvas * zoom + pan
    float zoom() const { return zoom_; }
    Vec2 pan() const { return pan_; }
    Vec2 to_screen(Vec2 canvas) const { return {canvas.x * zoom_ + pan_.x, canvas.y * zoom_ + pan_.y}; }
    Vec2 to_canvas(Vec2 screen) const { return {(screen.x - pan_.x) / zoom_, (screen.y - pan_.y) / zoom_}; }
    unsigned zoom_at(Vec2 screen, float factor);
    unsigned fit(Vec2 viewport, float margin = 48);

    // Ports win over cards, cards over cables. The tolerance is in screen
    // pixels so cables stay clickable when zoomed out.
    Hit hit_test(Vec2 screen, float tolerance = 6) const;

    // Input, in screen (logical) pixels
    unsigned press(Vec2 screen, Button button);
    unsigned move(Vec2 screen);
    unsigned release(Vec2 screen);
    unsigned key(Key key);

    // Rendering state
    const Hit& selection() const { return selection_; }
    const Hit& hover() const { return hover_; }
    std::optional<std::pair<Vec2, Vec2>> rubber_band() const;  // screen
    int moved_node() const { return moved_node_; }
    std::string link_path(bool selected) const;  // SVG path commands, screen coordinates
    const std::string& status() const { return status_; }

private:
    enum class Mode { Idle, Panning, Dragging, Linking };
    unsigned delete_selection();
    Node* mutable_node(int id);

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    int next_node_ = 1, next_link_ = 1;
    float zoom_ = 1;
    Vec2 pan_;
    Mode mode_ = Mode::Idle;
    Vec2 press_screen_, press_pan_, grab_offset_, pointer_;
    PortRef link_from_;
    Hit selection_, hover_;
    int moved_node_ = -1;
    std::string status_;
};

} // namespace spike

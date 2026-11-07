#pragma once

#include "app/scene.hpp"
#include "app/workspace.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace resend {

// What lies under a point of the canvas. Port lights and connectors are on
// the cards, so they win over the card; cards are drawn over the cables.
struct CanvasHit {
    enum class Kind { None, Device, Port, Connector, Cable };
    Kind kind = Kind::None;
    std::uint32_t id = 0;  // device or cable
    int port = -1;         // Port: zero-based
    bool operator==(const CanvasHit&) const = default;
};

// The selected device or cable, if any.
struct Selection {
    enum class Kind { None, Device, Cable };
    Kind kind = Kind::None;
    std::uint32_t id = 0;
    bool operator==(const Selection&) const = default;
};

// The canvas without Slint: the view (zoom and pan), what the pointer and
// the keys do, and the edits they make on the workspace. The window draws
// what this class computes and forwards its input in logical pixels.
//
// A card is dragged by its body. A cable starts from a port light, or from
// the connector, which takes the first free port, and ends on a port light
// or on a card's first free port; a click on a connector then another one
// works too. A click near a cable selects it.
class CanvasController {
public:
    enum class Button { Left, Middle, Right };
    enum class Key { Left, Right, Up, Down, Delete, Escape, ZoomIn, ZoomOut, Fit, Connect };
    // What an input changed, so the window updates only what it must.
    enum Change : unsigned {
        None = 0,
        View = 1,       // zoom or pan
        Network = 2,    // the scenario: a move, a cable, a removal
        Selection = 4,  // the selected device or cable, or the cable being drawn
        Overlay = 8,    // the cable being drawn or what is under the pointer
    };
    static constexpr float min_zoom = 0.35f, max_zoom = 2.2f;
    // Below this zoom, cards show only their icon and name.
    static constexpr float compact_zoom = 0.6f;
    // How far from a cable a click still selects it, in pixels.
    static constexpr float cable_tolerance = 6;
    // How far the pointer moves before a press on a card becomes a drag.
    static constexpr float drag_threshold = 3;

    explicit CanvasController(Workspace& workspace) : workspace_(workspace) {}

    // View: screen = canvas × zoom + pan, in logical pixels.
    float zoom() const { return zoom_; }
    Point pan() const { return pan_; }
    Point to_screen(Point canvas) const { return {canvas.x * zoom_ + pan_.x, canvas.y * zoom_ + pan_.y}; }
    Point to_canvas(Point screen) const { return {(screen.x - pan_.x) / zoom_, (screen.y - pan_.y) / zoom_}; }
    bool compact() const { return zoom_ < compact_zoom; }
    // The canvas size. While the view follows the network (after Fit, until
    // the user pans or zooms), a new size fits it again.
    unsigned resize(float width, float height);
    unsigned zoom_at(Point screen, float factor);
    unsigned zoom_by(float factor);  // around the centre
    unsigned reset_zoom();           // 100 %, the network's corner at the top left
    unsigned fit();
    // Where a new card goes: the middle of the view, or the free place
    // nearest to it, so that it covers no other card.
    Point new_card_place() const;

    CanvasHit hit_test(Point screen) const;
    unsigned press(Point screen, Button button);
    unsigned move(Point screen);
    unsigned release(Point screen);
    unsigned key(Key key, bool fine = false);
    // The connector of a device used without a pointer, by keyboard or
    // screen reader: the first starts a cable, the next ends it there.
    unsigned connector(std::uint32_t device);
    // What the last refused or completed action was about, for the status bar.
    const std::string& status() const { return status_; }
    // The status not shown yet, emptied: the same message twice is shown twice.
    std::string take_status() { return std::exchange(status_, {}); }
    // Whether the last input was refused because the instant shown is in the past.
    bool refused_in_past() const { return refused_in_past_; }

    const resend::Selection& selection() const { return selection_; }
    unsigned select(resend::Selection selection);
    // Drops a selection or a cable source that no longer exists, e.g. after undo.
    unsigned validate();
    // The port a cable being drawn starts from: a port, or a device's
    // connector (port -1) in the click-and-click way.
    std::optional<netsim::PortRef> cable_source() const { return source_; }
    bool drawing_cable() const { return mode_ == Mode::Cable || source_.has_value(); }
    bool panning() const { return mode_ == Mode::Panning; }
    const CanvasHit& hover() const { return hover_; }
    // The cable being drawn, from its source to the pointer, in screen pixels.
    std::optional<Segment> rubber_band() const;

    // Cables as path commands in screen pixels: those up or down, and the
    // selected one, which the window draws thicker. `highlight` are drawn
    // in the colour of the selected frame's path.
    struct CablePaths {
        std::string up, down, highlight, selected;
    };
    CablePaths cable_paths(const std::vector<std::uint32_t>& highlight = {}) const;
    // A cable's ends in canvas units, from card border to card border.
    std::optional<Segment> cable_segment(netsim::LinkId id) const;

private:
    enum class Mode { Idle, Pressing, Dragging, Panning, Cable };
    const netsim::Scenario& scenario() const { return workspace_.scenario(); }
    // Makes a change and says what happened.
    bool change(const netsim::Edit& edit, const std::string& label, bool gesture = false);
    unsigned connect(netsim::PortRef from, const CanvasHit& target);
    unsigned remove_selection();
    unsigned nudge(int dx, int dy);

    Workspace& workspace_;
    float zoom_ = 1;
    Point pan_{40, 60};
    Point viewport_{};
    bool follow_ = true;  // fit again when the canvas is resized
    Mode mode_ = Mode::Idle;
    Point press_{}, pointer_{};
    Point press_pan_{};
    Point grab_{};  // the press point in the card, canvas units
    std::uint32_t pressed_device_ = 0;
    std::optional<netsim::PortRef> source_;  // the cable being drawn
    resend::Selection selection_;
    CanvasHit hover_;
    std::string status_;
    bool refused_in_past_ = false;
};

} // namespace resend

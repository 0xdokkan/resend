#pragma once

#include "app/playback.hpp"
#include "scenario/scenario.hpp"
#include "sim/simulation.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace resend {

// What the canvas shows at the displayed instant, computed from the layout,
// the simulation and the playback without any Slint type, so it can be
// tested on its own.

struct Point {
    float x = 0, y = 0;
};
struct Box {
    float x = 0, y = 0, width = 0, height = 0;
    Point center() const { return {x + width / 2, y + height / 2}; }
};

// Device card size in canvas units; the Slint side receives it at startup.
constexpr float card_width = 160;
constexpr float card_height = 94;
constexpr int max_ports = 8;

Box card_box(const netsim::DeviceConfig& device);
// The port light of `port` on the card's front panel, in canvas units.
Box port_light(const netsim::DeviceConfig& device, int port);

// A straight cable between two cards, from border to border, `gap` units
// outside each card. Equal centres give a zero-length cable.
struct Segment {
    Point a, b;
};
Segment cable_between(const Box& a, const Box& b, float gap = 5);
Point lerp(Point a, Point b, float t);

// Edit: a change made while the simulation runs. Guard: the simulator
// stepped in, which is never network behaviour.
enum class Tone { Learn, Forward, Muted, Wait, Drop, Success, Edit, Guard };

struct PacketSprite {
    std::size_t transmission = 0;  // index in the trace's transmissions
    int pdu = -1;
    Point at, tail;  // the tail trails behind the packet along the cable
    netsim::PduKind kind{};
    bool broadcast = false;
    std::string label;        // short text on the packet, e.g. "ARP req"
    std::string description;  // for screen readers and tests
};
struct Marker {
    int device = -1;
    Point anchor;   // top centre of the device card
    int stack = 0;  // 0 right above the card, then upwards
    std::string text;
    Tone tone = Tone::Learn;
    float opacity = 1;
};
struct Scene {
    std::vector<PacketSprite> packets;
    std::vector<Marker> markers;
    // Per device id, a bit per port that sent or received a frame just now.
    std::vector<std::pair<int, unsigned>> port_activity;
};

// How long a decision stays on screen, in simulated time, and when it
// starts to fade. One hop on the default cables lasts 10 ms.
constexpr netsim::SimTime marker_life = std::chrono::milliseconds(8);
constexpr netsim::SimTime marker_fade = std::chrono::milliseconds(5);
constexpr netsim::SimTime activity_life = std::chrono::milliseconds(4);
// A card shows at most this many chips, the most recent: a storm leaves dozens.
constexpr int max_chips = 5;

// `layout` gives positions and names: the scenario being edited, which the
// simulation was built from; cards may have moved since.
Scene build_scene(const netsim::Scenario& layout, const netsim::Simulation& simulation, const PlaybackController& playback);

// Short packet label and full name of a frame kind.
std::string packet_label(netsim::PduKind kind);
std::string frame_name(netsim::PduKind kind);
// The frame by layer, one line per header: Ethernet, then ARP or IPv4 and ICMP.
std::string describe_layers(const netsim::EthernetFrame& frame);

} // namespace resend

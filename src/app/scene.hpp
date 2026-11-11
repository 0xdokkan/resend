#pragma once

#include "app/playback.hpp"
#include "look/look.hpp"
#include "scenario/scenario.hpp"
#include "sim/simulation.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace resend {

// What the canvas shows at the displayed instant, computed from the layout,
// the simulation and the playback without any Slint type, so it can be
// tested on its own.

struct Point {
    float x = 0, y = 0;
    bool operator==(const Point&) const = default;
};
struct Box {
    float x = 0, y = 0, width = 0, height = 0;
    Point center() const { return {x + width / 2, y + height / 2}; }
};

// Device card size in canvas units; the Slint side receives it at startup.
constexpr float card_width = 160;
constexpr float card_height = 94;
constexpr int max_ports = 8;

// Space above the cards for the decision chips, in canvas units.
constexpr float marker_room = 40;

Box card_box(const netsim::DeviceConfig& device);
// The port light of `port` on the card's front panel, in canvas units.
Box port_light(const netsim::DeviceConfig& device, int port);
// The connector at the card's bottom right corner, which starts a cable.
Box connector_box(const netsim::DeviceConfig& device);

// A straight cable between two cards, from border to border, `gap` units
// outside each card. Equal centres give a zero-length cable.
struct Segment {
    Point a, b;
};
Segment cable_between(const Box& a, const Box& b, float gap = 5);
Point lerp(Point a, Point b, float t);
// Dashes along cables that carry a frame, `dash` long every `period` canvas
// units, shifted by `phase` (0 to 1) of a period from each cable's start, so
// that they move with simulated time towards the receiver.
constexpr float dash_length = 6, dash_period = 16;
std::vector<Segment> dashes(const std::vector<Segment>& cables, float phase);
// Their phase at an instant: a period every 2 ms of simulated time.
float dash_phase(netsim::SimTime now);

// Edit: a change made while the simulation runs. Guard: the simulator
// stepped in, which is never network behaviour.
enum class Tone { Learn, Forward, Muted, Wait, Drop, Success, Edit, Guard };

struct PacketSprite {
    std::size_t transmission = 0;  // index in the trace's transmissions
    int pdu = -1;
    Point at, tail;  // the tail trails behind the packet along the cable
    Segment cable;   // the cable it is on, from the sender to the receiver
    netsim::PduKind kind{};
    bool broadcast = false;
    std::string label;        // short text on the packet, e.g. "ARP req"
    std::string description;  // for screen readers and tests
};
struct Marker {
    std::size_t record = 0;  // the record it shows, its index in the trace
    int device = -1;
    Point anchor;   // top centre of the device card
    int stack = 0;  // 0 right above the card, then upwards
    std::string text;
    Tone tone = Tone::Learn;
    float opacity = 1;
    // A sprite drawn before the text, for what has a symbol: "power" for a
    // device switched on or off, "warning" for the storm guard.
    std::string icon;
};
// How much the canvas moves (G4.4). Full: packets travel along the cables
// and every effect plays, particles and all. Reduced: events appear one
// instant at a time, and effects without particles or shaking. Off: one
// instant at a time, with the markers and the texts only.
enum class Intensity { Full, Reduced, Off };
std::string_view name(Intensity intensity);  // "full", "reduced", "off"
std::optional<Intensity> intensity_named(std::string_view text);

// The reactions of the canvas to what happens (G4.1). Each comes from a kind
// of record, in the table below, and each of those records also has a
// marker or an event, so nothing is shown by motion alone.
enum class EffectKind {
    Arrival,  // a pop where a frame comes in
    Learn,    // a spark on the port of a switch that learns an address
    Flood,    // a fan-out from a switch, one for each copy it sends
    Drop,     // a puff where a frame or a datagram stops for a problem
    Reply,    // a burst at the source when its reply comes back
    Failure,  // the source shakes when its ping fails
    Guard,    // a flash over the canvas when the storm guard steps in
};
struct EffectSource {
    netsim::TraceKind record;
    EffectKind effect;
};
inline constexpr EffectSource effect_sources[] = {
    {netsim::TraceKind::FrameReceived, EffectKind::Arrival},  {netsim::TraceKind::MacLearned, EffectKind::Learn},
    {netsim::TraceKind::FrameFlooded, EffectKind::Flood},     {netsim::TraceKind::FrameDropped, EffectKind::Drop},
    {netsim::TraceKind::DatagramDropped, EffectKind::Drop},   {netsim::TraceKind::PingReply, EffectKind::Reply},
    {netsim::TraceKind::PingSucceeded, EffectKind::Reply},    {netsim::TraceKind::PingFailed, EffectKind::Failure},
    {netsim::TraceKind::SimulatorGuard, EffectKind::Guard},
};
inline constexpr std::array<EffectKind, 7> effect_kinds = {EffectKind::Arrival, EffectKind::Learn, EffectKind::Flood, EffectKind::Drop,
                                                           EffectKind::Reply,   EffectKind::Failure, EffectKind::Guard};
std::string_view name(EffectKind kind);
// How long an effect lasts, in simulated time: at 1× a millisecond lasts
// 60 ms. `big`: the reply that ends a ping.
netsim::SimTime effect_life(EffectKind kind, bool big = false);
// At most this many effects at once, the oldest going first.
constexpr std::size_t max_effects = 64;

// An effect: a kind, a place, an age and a tone. Slint only draws it, from
// the shapes below.
struct Effect {
    EffectKind kind = EffectKind::Arrival;
    std::size_t record = 0;  // its record's index in the trace
    int device = -1;
    Point at;       // canvas units
    float age = 0;  // 0 at its record's instant, towards 1 at the end of its life
    // The family it is drawn in (ui/theme.slint): "ask" for ARP, "ok" for
    // ICMP and replies, "link", "drop", "guard".
    std::string tone;
    bool big = false;  // a reply: the one that ends the ping
    bool operator==(const Effect&) const = default;
};
// What is drawn: a square ring, a filled square (a particle, a copy that
// fans out) or a flash over the whole canvas. Sizes are half widths in
// canvas units; the ring's line is two art pixels.
struct Shape {
    enum class Form { Ring, Dot, Flash };
    Form form = Form::Ring;
    Point at;
    float size = 0;
    std::string tone;
    float opacity = 1;
    bool operator==(const Shape&) const = default;
};
// Particles stay within this many shapes, whatever the effects.
constexpr std::size_t max_shapes = 320;

struct Scene {
    std::vector<PacketSprite> packets;
    std::vector<Marker> markers;
    // Oldest first, at most max_effects; none with effects off.
    std::vector<Effect> effects;
    std::vector<Shape> shapes;
    // Per device id, how far its sprite is moved off its place, in canvas
    // units: a hop when a frame arrives, a shake when a ping fails. Full
    // effects only.
    std::vector<std::pair<int, Point>> nudges;
    // Per device id, a bit per port that sent or received a frame just now.
    std::vector<std::pair<int, unsigned>> port_activity;
    // Per device id, what it is doing just now: sending, receiving, or in
    // error after a frame stopped there for a reason that is a problem.
    // Devices doing none of these are not listed.
    std::vector<std::pair<int, look::DeviceState>> device_activity;
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
Scene build_scene(const netsim::Scenario& layout, const netsim::Simulation& simulation, const PlaybackController& playback,
                  Intensity intensity = Intensity::Full);
// The effects alive at `now`, their shapes and the nudges (src/app/effects.cpp).
void add_effects(Scene& scene, const netsim::Scenario& layout, const netsim::Simulation& simulation, netsim::SimTime now,
                 Intensity intensity);

// The sprite of a frame kind (assets/sprites/packets.txt), every kind its own.
std::string_view packet_sprite(netsim::PduKind kind);
inline constexpr std::array<netsim::PduKind, 7> pdu_kinds = {
    netsim::PduKind::ArpRequest, netsim::PduKind::ArpReply,     netsim::PduKind::EchoRequest, netsim::PduKind::EchoReply,
    netsim::PduKind::OtherIcmp,  netsim::PduKind::TimeExceeded, netsim::PduKind::Unreachable};
// A drop that is a problem, not a frame or a datagram for someone else.
bool is_problem(netsim::Reason reason);

// Short packet label and full name of a frame kind.
std::string packet_label(netsim::PduKind kind);
std::string frame_name(netsim::PduKind kind);

} // namespace resend

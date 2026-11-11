#pragma once

#include "sim/simulation.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

// What resend plays (G5): cues from the records of the trace, like the
// effects of the canvas, and from the interface. No audio here: the
// synthesizer and its sounds are in src/audio/.
namespace resend {

enum class CueKind {
    Hop,      // a frame reaches a device: the next note of a pentatonic scale
    Ask,      // a host or a router sends an ARP request: a rising pair
    Answer,   // and its ARP reply: a falling pair
    Thud,     // a frame or a datagram stops for a problem
    Resolve,  // a ping ends with its reply: the phrase resolves
    Fail,     // a ping fails
    Alarm,    // the storm guard steps in
    Goal,     // the interface: a goal just met
    Win,      // a lab solved
    Click,    // a button pressed
};
inline constexpr std::array<CueKind, 10> cue_kinds = {CueKind::Hop,  CueKind::Ask,   CueKind::Answer, CueKind::Thud, CueKind::Resolve,
                                                      CueKind::Fail, CueKind::Alarm, CueKind::Goal,   CueKind::Win,  CueKind::Click};
std::string_view name(CueKind kind);

// The cues of the trace and the records they come from. Each of those
// records is also drawn: as an effect (src/app/scene.hpp's effect_sources),
// or, for what a device sends, as the packet leaving it.
struct CueSource {
    netsim::TraceKind record;
    CueKind cue;
};
inline constexpr CueSource cue_sources[] = {
    {netsim::TraceKind::FrameReceived, CueKind::Hop},     {netsim::TraceKind::FrameSent, CueKind::Ask},
    {netsim::TraceKind::FrameSent, CueKind::Answer},      {netsim::TraceKind::FrameDropped, CueKind::Thud},
    {netsim::TraceKind::DatagramDropped, CueKind::Thud},  {netsim::TraceKind::PingSucceeded, CueKind::Resolve},
    {netsim::TraceKind::PingFailed, CueKind::Fail},       {netsim::TraceKind::SimulatorGuard, CueKind::Alarm},
};

struct Cue {
    CueKind kind = CueKind::Hop;
    netsim::SimTime time{};
    int note = 0;            // a hop: its degree of the scale, 0 to 7
    std::size_t record = 0;  // the record's index in the trace
    bool operator==(const Cue&) const = default;
};

// The degrees of the scale: hops count up through it, whichever ping they
// belong to, so that pings that run together stay in tune.
constexpr int scale_length = 8;

// The cues of the records from `from` to `to` (exclusive). `hops` is the
// number of arrivals before `from`, and is advanced past them.
std::vector<Cue> cues_of(const netsim::Simulation& simulation, std::size_t from, std::size_t to, int& hops);

// Follows a trace as the playback shows it: take() gives the cues of the
// records added since the last call. After the timeline jumps, skip() moves
// past what was replayed without a sound.
class CueEngine {
public:
    std::vector<Cue> take(const netsim::Simulation& simulation);
    void skip(const netsim::Simulation& simulation);

private:
    std::size_t next_ = 0;
    int hops_ = 0;
};

// What one moment plays, after the limits of G5.5: a flood is one chord of
// at most three notes, not seven; a cue of a kind played less than 30 ms
// ago is not played again.
struct Play {
    CueKind kind = CueKind::Hop;
    std::vector<int> notes;  // a hop's chord
    bool operator==(const Play&) const = default;
};
class CueMixer {
public:
    using RealTime = std::chrono::nanoseconds;
    static constexpr RealTime merge_window = std::chrono::milliseconds(30);
    static constexpr std::size_t max_chord = 3;
    // `now` is real time on any steady clock.
    std::vector<Play> mix(const std::vector<Cue>& cues, RealTime now);

private:
    std::array<std::optional<RealTime>, cue_kinds.size()> last_{};
};

} // namespace resend

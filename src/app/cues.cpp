#include "app/cues.hpp"

#include "app/scene.hpp"

#include <algorithm>

namespace resend {
namespace {
using netsim::TraceKind;

// A host or a router sends what it says itself; a switch only passes frames on.
bool speaks(const netsim::Simulation& simulation, netsim::DeviceId device) {
    const auto* config = simulation.known_device(device);
    return config && config->kind != netsim::DeviceKind::Switch;
}

} // namespace

std::string_view name(CueKind kind) {
    switch (kind) {
    case CueKind::Hop: return "hop";
    case CueKind::Ask: return "ask";
    case CueKind::Answer: return "answer";
    case CueKind::Thud: return "thud";
    case CueKind::Resolve: return "resolve";
    case CueKind::Fail: return "fail";
    case CueKind::Alarm: return "alarm";
    case CueKind::Goal: return "goal";
    case CueKind::Win: return "win";
    case CueKind::Click: return "click";
    }
    return "hop";
}

std::vector<Cue> cues_of(const netsim::Simulation& simulation, std::size_t from, std::size_t to, int& hops) {
    const auto& trace = simulation.trace();
    const auto& records = trace.records();
    to = std::min(to, records.size());
    std::vector<Cue> cues;
    for (std::size_t index = from; index < to; ++index) {
        const auto& record = records[index];
        const auto add = [&](CueKind kind, int note = 0) { cues.push_back({kind, record.time, note, index}); };
        switch (record.kind) {
        case TraceKind::FrameReceived:
            add(CueKind::Hop, hops % scale_length);
            ++hops;
            break;
        case TraceKind::FrameSent: {
            if (!record.pdu || !speaks(simulation, record.device)) break;
            const auto kind = netsim::kind_of(trace.pdu(*record.pdu));
            if (kind == netsim::PduKind::ArpRequest) add(CueKind::Ask);
            if (kind == netsim::PduKind::ArpReply) add(CueKind::Answer);
            break;
        }
        case TraceKind::FrameDropped:
        case TraceKind::DatagramDropped:
            if (is_problem(record.reason)) add(CueKind::Thud);
            break;
        case TraceKind::PingSucceeded: add(CueKind::Resolve); break;
        case TraceKind::PingFailed: add(CueKind::Fail); break;
        case TraceKind::SimulatorGuard: add(CueKind::Alarm); break;
        default: break;
        }
    }
    return cues;
}

std::vector<Cue> CueEngine::take(const netsim::Simulation& simulation) {
    const auto size = simulation.trace().records().size();
    if (size < next_) next_ = 0, hops_ = 0;  // a new run
    auto cues = cues_of(simulation, next_, size, hops_);
    next_ = size;
    return cues;
}

void CueEngine::skip(const netsim::Simulation& simulation) {
    const auto size = simulation.trace().records().size();
    if (size < next_) next_ = 0, hops_ = 0;
    (void)cues_of(simulation, next_, size, hops_);  // the scale goes on
    next_ = size;
}

std::vector<Play> CueMixer::mix(const std::vector<Cue>& cues, RealTime now) {
    std::vector<Play> plays;
    for (const auto& cue : cues) {
        const auto slot = static_cast<std::size_t>(cue.kind);
        auto found = std::find_if(plays.begin(), plays.end(), [&](const Play& play) { return play.kind == cue.kind; });
        if (found != plays.end()) {
            // The same moment: a hop adds its note to the chord, others merge.
            if (cue.kind == CueKind::Hop && found->notes.size() < max_chord &&
                std::find(found->notes.begin(), found->notes.end(), cue.note) == found->notes.end())
                found->notes.push_back(cue.note);
            continue;
        }
        if (last_[slot] && now - *last_[slot] < merge_window) continue;
        Play play{cue.kind, {}};
        if (cue.kind == CueKind::Hop) play.notes.push_back(cue.note);
        plays.push_back(std::move(play));
    }
    for (const auto& play : plays) last_[static_cast<std::size_t>(play.kind)] = now;
    return plays;
}

} // namespace resend

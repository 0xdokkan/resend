// The canvas's effects (G4): derived from the trace and the instant shown,
// each from a kind of record in one table, within their budget, and at
// three intensities.
#include "app/playback.hpp"
#include "app/scene.hpp"
#include "check.hpp"
#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using netsim::SimTime;
using resend::EffectKind;
using resend::Intensity;
using resend::PlaybackController;
using resend::Scene;
using resend::Shape;

netsim::Scenario example(const std::string& name) {
    std::ifstream file(std::filesystem::path(RESEND_EXAMPLES_DIR) / (name + ".json"), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string error;
    auto scenario = netsim::scenario_from_json(text, error);
    if (!scenario) std::cerr << name << ": " << error << '\n';
    CHECK(scenario.has_value());
    return *scenario;
}

// A network and a ping on it, rebuilt the same each time it is asked for.
struct Case {
    std::string label;
    netsim::Scenario network;
    netsim::DeviceId from;
    netsim::Ipv4Address to;
    std::uint16_t count = 1;
    netsim::Simulation start() const {
        netsim::Simulation simulation(network);
        netsim::Problem problem{};
        netsim::PingOptions options;
        options.count = count;
        CHECK(simulation.start_ping(from, to, problem, options).has_value());
        return simulation;
    }
};

std::vector<Case> cases() {
    std::vector<Case> out;
    const auto address = [](const char* text) { return *netsim::Ipv4Address::parse(text); };
    out.push_back({"a ping on the sample LAN", netsim::sample_lan(), netsim::DeviceId{1}, address("192.168.1.20")});
    out.push_back({"three requests", netsim::sample_lan(), netsim::DeviceId{1}, address("192.168.1.20"), 3});
    auto off = netsim::sample_lan();
    off.device(netsim::DeviceId{2})->powered = false;
    out.push_back({"to a PC that is off", off, netsim::DeviceId{1}, address("192.168.1.20")});
    // The storm: a loop of switches, stopped by the guard.
    const auto loop = example("switch-loop");
    const netsim::DeviceConfig *first = nullptr, *last = nullptr;
    for (const auto& device : loop.devices)
        if (device.kind == netsim::DeviceKind::Host) {
            if (!first) first = &device;
            last = &device;
        }
    CHECK(first && last && first != last && last->ports[0].ipv4);
    out.push_back({"a loop of switches", loop, first->id, last->ports[0].ipv4->address()});
    const auto routed = example("two-lans-router");
    out.push_back({"across a router", routed, netsim::DeviceId{1}, address("10.2.0.20")});
    return out;
}

// The instants a playback at 1× shows, frame by frame, and their scenes.
struct Frame {
    SimTime now;
    Scene scene;
};
std::vector<Frame> play(const Case& item, Intensity intensity, int every = 3) {
    auto simulation = item.start();
    PlaybackController playback(simulation);
    playback.set_reduced_motion(intensity != Intensity::Full);
    playback.play();
    std::vector<Frame> frames;
    for (int frame = 0; frame < 20'000 && playback.playing(); ++frame) {
        playback.tick(16'666'667ns);
        if (frame % every == 0) frames.push_back({playback.now(), build_scene(item.network, simulation, playback, intensity)});
    }
    CHECK(!playback.playing());
    return frames;
}

void every_effect_comes_from_the_table() {
    // Each kind of effect has a kind of record, and no record two effects.
    std::set<netsim::TraceKind> sources;
    for (const auto kind : resend::effect_kinds)
        CHECK(std::any_of(std::begin(resend::effect_sources), std::end(resend::effect_sources), [&](const auto& s) { return s.effect == kind; }));
    for (const auto& source : resend::effect_sources) CHECK(sources.insert(source.record).second);

    std::set<EffectKind> seen;
    for (const auto& item : cases()) {
        auto finished = item.start();
        while (finished.step()) {}
        netsim::explain::Explainer explainer;
        explainer.update(finished);
        const auto& records = finished.trace().records();
        for (const auto& frame : play(item, Intensity::Full, 1)) {
            for (const auto& effect : frame.scene.effects) {
                seen.insert(effect.kind);
                CHECK(effect.record < records.size());
                const auto& record = records[effect.record];
                const auto source = std::find_if(std::begin(resend::effect_sources), std::end(resend::effect_sources),
                                                 [&](const auto& s) { return s.record == record.kind; });
                CHECK(source != std::end(resend::effect_sources) && source->effect == effect.kind);
                CHECK(effect.age >= 0 && effect.age < 1 && record.time <= frame.now);
                // Nothing in motion alone: the record has a marker on screen,
                // or an event in the timeline.
                const bool marked = std::any_of(frame.scene.markers.begin(), frame.scene.markers.end(),
                                                [&](const auto& marker) { return marker.record == effect.record; });
                const bool listed = std::any_of(explainer.events().begin(), explainer.events().end(),
                                                [&](const auto& event) { return event.episode == record.episode; });
                if (!marked && !listed) std::cerr << item.label << ": " << resend::name(effect.kind) << " without a marker or an event\n";
                CHECK(marked || listed);
            }
        }
    }
    for (const auto kind : resend::effect_kinds) {
        if (!seen.contains(kind)) std::cerr << "never seen: " << resend::name(kind) << '\n';
        CHECK(seen.contains(kind));
    }
}

void dragging_gives_what_playing_gave() {
    // The scene at an instant is the same whether the playback got there
    // frame by frame or the timeline jumped to it.
    for (const auto& item : cases()) {
        int compared = 0;
        for (const auto& frame : play(item, Intensity::Full, 7)) {
            auto simulation = item.start();
            PlaybackController playback(simulation);
            playback.seek(frame.now);
            const auto scene = build_scene(item.network, simulation, playback, Intensity::Full);
            CHECK(scene.effects == frame.scene.effects);
            CHECK(scene.shapes == frame.scene.shapes);
            CHECK(scene.nudges == frame.scene.nudges);
            compared += scene.effects.empty() ? 0 : 1;
        }
        CHECK(compared > 0);
    }
}

// A switch with seven others around it, seven hosts on each: one broadcast
// makes 68 effects alive at 21 ms, the cables' delays being what they are.
Case crowd() {
    std::string devices, links;
    int next_link = 1;
    const auto link = [&](int a, int pa, int b, int pb, const char* delay) {
        links += std::string(links.empty() ? "" : ",\n") + "{\"id\": " + std::to_string(next_link++) + ", \"a\": {\"device\": " + std::to_string(a) +
                 ", \"port\": " + std::to_string(pa) + "}, \"b\": {\"device\": " + std::to_string(b) + ", \"port\": " + std::to_string(pb) +
                 "}, \"delay\": \"" + delay + "\"}";
    };
    const auto device = [&](int id, const std::string& body) { devices += std::string(devices.empty() ? "" : ",\n") + "{\"id\": " + std::to_string(id) + ", " + body + "}"; };
    device(1, "\"kind\": \"switch\", \"name\": \"Core\"");
    int id = 2;
    for (int edge = 0; edge < 7; ++edge) {
        const int switch_id = id++;
        device(switch_id, "\"kind\": \"switch\", \"name\": \"Edge-" + std::to_string(edge + 1) + "\"");
        link(1, edge + 1, switch_id, 8, edge == 0 ? "10ms" : "1ms");
        for (int host = 0; host < 7; ++host) {
            const int host_id = id++;
            const auto address = "10.0." + std::to_string(edge) + "." + std::to_string(host + 1) + "/16";
            device(host_id, "\"kind\": \"host\", \"name\": \"H" + std::to_string(edge + 1) + std::to_string(host + 1) + "\", \"ports\": [{\"ipv4\": \"" + address + "\"}]");
            // The first edge's own hosts get the broadcast when the others' switches do.
            link(switch_id, host + 1, host_id, 1, edge == 0 && host > 0 ? "11ms" : "10ms");
        }
    }
    const auto text = "{\"format\": \"resend-scenario\", \"version\": 3, \"devices\": [" + devices + "], \"links\": [" + links + "]}";
    std::string error;
    auto scenario = netsim::scenario_from_json(text, error);
    if (!scenario) std::cerr << "crowd: " << error << '\n';
    CHECK(scenario.has_value());
    // H11 asks for an address nobody has: the request floods everywhere.
    return {"a crowd", *scenario, netsim::DeviceId{3}, *netsim::Ipv4Address::parse("10.0.9.9")};
}

void effects_stay_within_their_budget() {
    // Dozens of frames arrive at once: the newest effects are kept.
    const auto storm = crowd();
    auto simulation = storm.start();
    std::size_t most = 0;
    while (simulation.step()) {
        PlaybackController playback(simulation);
        const auto scene = build_scene(storm.network, simulation, playback, Intensity::Full);
        most = std::max(most, scene.effects.size());
        CHECK(scene.effects.size() <= resend::max_effects && scene.shapes.size() <= resend::max_shapes);
        for (std::size_t i = 1; i < scene.effects.size(); ++i) CHECK(scene.effects[i - 1].record <= scene.effects[i].record);
        if (scene.effects.size() == resend::max_effects) {
            // The newest are kept: every frame that arrived last has its pop.
            const auto& records = simulation.trace().records();
            std::size_t newest = records.size();
            while (newest > 0 && records[newest - 1].kind != netsim::TraceKind::FrameReceived) --newest;
            CHECK(newest > 0 && scene.effects.back().record >= newest - 1);
        }
        if (simulation.now() > 45ms) break;
    }
    CHECK(most == resend::max_effects);
}

void three_intensities() {
    const auto item = cases()[0];
    // Right after the reply comes back to PC-A.
    auto simulation = item.start();
    PlaybackController playback(simulation);
    playback.seek(82ms);
    const auto full = build_scene(item.network, simulation, playback, Intensity::Full);
    const auto reduced = build_scene(item.network, simulation, playback, Intensity::Reduced);
    const auto off = build_scene(item.network, simulation, playback, Intensity::Off);
    const auto dots = [](const Scene& scene) {
        return std::count_if(scene.shapes.begin(), scene.shapes.end(), [](const Shape& shape) { return shape.form == Shape::Form::Dot; });
    };
    CHECK(!full.effects.empty() && full.effects.back().kind == EffectKind::Reply && full.effects.back().big);
    // Full: confetti and rings. Reduced: the same effects, rings only, and
    // nothing moves a device. Off: none at all.
    CHECK(dots(full) >= 12 && dots(reduced) == 0 && reduced.nudges.empty());
    CHECK(reduced.effects == full.effects && !reduced.shapes.empty());
    CHECK(off.effects.empty() && off.shapes.empty() && off.nudges.empty());
    // With effects off nothing else is lost: the same markers and packets.
    CHECK(off.markers.size() == full.markers.size() && off.packets.size() == full.packets.size());
    for (std::size_t i = 0; i < off.markers.size(); ++i) CHECK(off.markers[i].text == full.markers[i].text);

    // A frame arriving makes its device hop, at full effects only.
    auto again = item.start();
    PlaybackController arriving(again);
    arriving.seek(11ms);
    const auto hop = build_scene(item.network, again, arriving, Intensity::Full);
    CHECK(std::any_of(hop.nudges.begin(), hop.nudges.end(), [](const auto& nudge) { return nudge.second.y < 0; }));

    // The names, and their way back.
    for (const auto intensity : {Intensity::Full, Intensity::Reduced, Intensity::Off}) CHECK(resend::intensity_named(resend::name(intensity)) == intensity);
    CHECK(!resend::intensity_named("loud"));
}

void a_ping_ends_with_its_burst_played() {
    // The playback goes on past the last record for the afterglow, so the
    // burst of the reply plays to its end and then the canvas is still.
    const auto item = cases()[0];
    const auto frames = play(item, Intensity::Full, 1);
    CHECK(!frames.empty() && frames.back().scene.effects.empty());
    float oldest = 0;
    for (const auto& frame : frames)
        for (const auto& effect : frame.scene.effects)
            if (effect.big) oldest = std::max(oldest, effect.age);
    CHECK(oldest > 0.9f);
}

} // namespace

int main() {
    every_effect_comes_from_the_table();
    dragging_gives_what_playing_gave();
    effects_stay_within_their_budget();
    three_intensities();
    a_ping_ends_with_its_burst_played();
    std::cout << "effects tests passed (" << test::checks << " checks)\n";
}

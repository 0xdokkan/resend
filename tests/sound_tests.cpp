// What resend plays (G5): the cues of a run, compared with golden files in
// tests/golden/cues; what a ping ends on; a trace that sound leaves alone;
// a mix that never clips; and nothing heard that is not also seen. To
// accept an intended change, run with RESEND_UPDATE_GOLDEN=1 and review
// the diff.
#include "app/cues.hpp"
#include "app/playback.hpp"
#include "app/scene.hpp"
#include "audio/output.hpp"
#include "audio/sounds.hpp"
#include "audio/synth.hpp"
#include "check.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using resend::Cue;
using resend::CueKind;
using resend::Play;

constexpr auto frame_time = 16'666'667ns;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

netsim::Scenario example(const std::string& name) {
    const auto text = read_file(std::filesystem::path(RESEND_EXAMPLES_DIR) / (name + ".json"));
    std::string error;
    auto scenario = netsim::scenario_from_json(text, error);
    if (!scenario) std::cerr << name << ": " << error << '\n';
    CHECK(scenario.has_value());
    return *scenario;
}

// A network and a ping on it; `succeeds` is what the ping should do.
struct Case {
    std::string file, label;
    netsim::Scenario network;
    netsim::DeviceId from;
    netsim::Ipv4Address to;
    bool succeeds = true;
    netsim::Simulation start() const {
        netsim::Simulation simulation(network);
        netsim::Problem problem{};
        CHECK(simulation.start_ping(from, to, problem, {}).has_value());
        return simulation;
    }
};

std::vector<Case> cases() {
    const auto address = [](const char* text) { return *netsim::Ipv4Address::parse(text); };
    std::vector<Case> out;
    out.push_back({"sample-ping", "a ping on the sample LAN", netsim::sample_lan(), netsim::DeviceId{1}, address("192.168.1.20")});
    out.push_back({"two-lans-router", "a ping across a router", example("two-lans-router"), netsim::DeviceId{1}, address("10.2.0.20")});
    out.push_back({"wrong-gateway", "a ping through a gateway nobody has", example("wrong-gateway"), netsim::DeviceId{1},
                   address("10.2.0.20"), false});
    out.push_back({"routing-loop", "a ping into a routing loop", example("routing-loop"), netsim::DeviceId{1}, address("10.9.9.9"), false});
    const auto loop = example("switch-loop");
    const netsim::DeviceConfig *first = nullptr, *last = nullptr;
    for (const auto& device : loop.devices)
        if (device.kind == netsim::DeviceKind::Host) {
            if (!first) first = &device;
            last = &device;
        }
    CHECK(first && last && first != last && last->ports[0].ipv4);
    out.push_back({"switch-loop", "a ping into a loop of switches", loop, first->id, last->ports[0].ipv4->address(), false});
    return out;
}

// One frame of a playback at 1×: the cues of the records it added, what of
// them is heard, and its scene.
struct Frame {
    int number = 0;
    netsim::SimTime now{};
    std::vector<Cue> cues;
    std::vector<Play> heard;
    resend::Scene scene;
};
struct Run {
    std::vector<Frame> frames;  // the frames with a cue
    std::vector<netsim::TraceRecord> records;
};
Run play(const Case& item, bool sound = true) {
    auto simulation = item.start();
    resend::PlaybackController playback(simulation);
    playback.play();
    resend::CueEngine engine;
    resend::CueMixer mixer;
    Run run;
    for (int number = 1; number < 20'000 && playback.playing(); ++number) {
        playback.tick(frame_time);
        if (!sound) continue;
        auto cues = engine.take(simulation);
        if (cues.empty()) continue;
        auto heard = mixer.mix(cues, number * frame_time);
        run.frames.push_back({number, playback.now(), std::move(cues), std::move(heard),
                              resend::build_scene(item.network, simulation, playback, resend::Intensity::Full)});
    }
    CHECK(!playback.playing());
    run.records = simulation.trace().records();
    return run;
}

std::string notes(const Play& play) {
    std::string text(resend::name(play.kind));
    for (std::size_t i = 0; i < play.notes.size(); ++i) text += (i == 0 ? " " : "+") + std::to_string(play.notes[i]);
    return text;
}

std::string transcript(const Case& item, const Run& run) {
    std::ostringstream out;
    out << "# " << item.label << ", at 1x, 60 frames a second: the records' cues, then what is heard\n";
    for (const auto& frame : run.frames) {
        out << "frame " << frame.number << "  " << netsim::format_ms(frame.now) << "\n ";
        for (const auto& cue : frame.cues) {
            out << ' ' << resend::name(cue.kind);
            if (cue.kind == CueKind::Hop) out << ' ' << cue.note;
            out << " #" << cue.record << ';';
        }
        out << "\n  heard:";
        for (const auto& play : frame.heard) out << ' ' << notes(play) << ';';
        out << '\n';
    }
    return out.str();
}

int golden_files() {
    const std::filesystem::path golden = RESEND_GOLDEN_DIR;
    const char* update = std::getenv("RESEND_UPDATE_GOLDEN");
    const bool write = update && std::string(update) == "1";
    int changed = 0;
    for (const auto& item : cases()) {
        const auto path = golden / (item.file + ".txt");
        const auto output = transcript(item, play(item));
        if (write) {
            std::filesystem::create_directories(golden);
            std::ofstream(path, std::ios::binary) << output;
            continue;
        }
        if (read_file(path) != output) {
            std::cerr << "Differs from " << path.string() << ":\n" << output << '\n';
            ++changed;
        }
        CHECK(read_file(path) == output);
    }
    if (write) std::cout << "Golden files written to " << golden.string() << ".\n";
    return changed;
}

// A ping that succeeds ends on its resolve; one that fails never plays it.
void a_ping_ends_as_it_went() {
    for (const auto& item : cases()) {
        const auto run = play(item);
        CHECK(!run.frames.empty());
        std::vector<CueKind> heard;
        for (const auto& frame : run.frames)
            for (const auto& one : frame.heard) heard.push_back(one.kind);
        const bool resolved = std::find(heard.begin(), heard.end(), CueKind::Resolve) != heard.end();
        if (item.succeeds) {
            if (heard.empty() || heard.back() != CueKind::Resolve) std::cerr << "  " << item.label << " does not end on its resolve\n";
            CHECK(!heard.empty() && heard.back() == CueKind::Resolve);
        } else {
            CHECK(!resolved);
            CHECK(std::find(heard.begin(), heard.end(), CueKind::Fail) != heard.end() ||
                  std::find(heard.begin(), heard.end(), CueKind::Alarm) != heard.end());
        }
        // A flood is one chord, never more than three notes.
        for (const auto& frame : run.frames)
            for (const auto& one : frame.heard) {
                CHECK(one.notes.size() <= resend::CueMixer::max_chord);
                CHECK(std::count_if(frame.heard.begin(), frame.heard.end(), [&](const Play& p) { return p.kind == one.kind; }) == 1);
            }
    }
    // The loops are heard: the routing loop thuds as its TTL runs out, the
    // storm sounds its alarm.
    const auto all = cases();
    const auto kinds = [](const Run& run) {
        std::vector<CueKind> out;
        for (const auto& frame : run.frames)
            for (const auto& one : frame.heard) out.push_back(one.kind);
        return out;
    };
    const auto routing = kinds(play(all[3]));
    CHECK(std::find(routing.begin(), routing.end(), CueKind::Thud) != routing.end());
    const auto storm = kinds(play(all[4]));
    CHECK(std::find(storm.begin(), storm.end(), CueKind::Alarm) != storm.end());
}

// Sound only listens: the trace is the same heard or not.
void sound_leaves_the_trace_alone() {
    for (const auto& item : cases()) CHECK(play(item, true).records == play(item, false).records);
    // And the null output is an output like any other, which nobody hears.
    resend::audio::Synth synth;
    const auto none = resend::audio::null_output("for the tests");
    CHECK(!none->audible() && none->description() == "for the tests");
}

// The mixer: one cue of a kind per merge window, the scale climbing.
void the_mixer_keeps_it_quiet() {
    resend::CueMixer mixer;
    const auto hop = [](int note) { return Cue{CueKind::Hop, {}, note, 0}; };
    auto heard = mixer.mix({hop(0), hop(1), hop(1), hop(2), hop(3), Cue{CueKind::Ask, {}, 0, 0}}, 0ms);
    CHECK(heard.size() == 2 && heard[0].notes == std::vector<int>({0, 1, 2}) && heard[1].kind == CueKind::Ask);
    CHECK(mixer.mix({hop(4)}, 20ms).empty());
    CHECK(mixer.mix({hop(4)}, 31ms).size() == 1);
    CHECK(mixer.mix({Cue{CueKind::Ask, {}, 0, 0}}, 40ms).size() == 1);
}

// Eight voices at full volume, the loudest sounds there are, never clip;
// a ninth takes the place of the oldest.
void eight_voices_never_clip() {
    resend::audio::Synth synth;
    synth.set_volume(1);
    const resend::audio::Sound loud = {{resend::audio::Wave::Square, 110, 0, 0, 1.0f, 1.0f}};
    for (int i = 0; i < 8; ++i) synth.play(loud);
    synth.play(resend::audio::win());
    synth.play(resend::audio::thud());
    CHECK(synth.voices() <= resend::audio::max_voices);
    std::vector<float> samples(resend::audio::sample_rate);
    synth.render(samples.data(), samples.size());
    float peak = 0;
    for (const float sample : samples) peak = std::max(peak, std::abs(sample));
    CHECK(peak > 0.5f && peak < 1.0f);
    CHECK(std::all_of(samples.begin(), samples.end(), [](float sample) { return std::isfinite(sample); }));
    // The limiter is linear where a single sound is, and stays below 1.
    CHECK(resend::audio::limit(0.5f) == 0.5f && resend::audio::limit(-0.5f) == -0.5f);
    CHECK(resend::audio::limit(4) < 1 && resend::audio::limit(-4) > -1 && resend::audio::limit(1) > 0.8f);
    // Every sound is short, quiet and in tune; stopping silences them.
    for (const auto& sound : {resend::audio::ask(), resend::audio::answer(), resend::audio::thud(), resend::audio::resolve(),
                              resend::audio::fail(), resend::audio::alarm(), resend::audio::goal(), resend::audio::win(),
                              resend::audio::click(), resend::audio::hop(std::vector<int>{0, 2, 4})}) {
        CHECK(!sound.empty() && sound.size() <= resend::audio::max_voices);
        for (const auto& note : sound) CHECK(note.start + note.length <= 1.0f && note.volume <= 1.0f && note.frequency > 0);
    }
    synth.stop();
    CHECK(synth.voices() == 0);
    synth.render(samples.data(), 64);
    CHECK(std::all_of(samples.begin(), samples.begin() + 64, [](float sample) { return sample == 0; }));
}

// Nothing is heard that is not also seen: a cue's record is an effect of
// the canvas in the frame it plays, or, for what a device says, the packet
// leaving it.
void every_cue_is_seen() {
    for (const auto& source : resend::cue_sources) {
        const bool drawn = std::any_of(std::begin(resend::effect_sources), std::end(resend::effect_sources),
                                       [&](const auto& effect) { return effect.record == source.record; });
        CHECK(drawn || source.record == netsim::TraceKind::FrameSent);
    }
    for (const auto& item : cases()) {
        const auto run = play(item);
        for (const auto& frame : run.frames)
            for (const auto& cue : frame.cues) {
                const auto& record = run.records[cue.record];
                bool seen = false;
                if (cue.kind == CueKind::Ask || cue.kind == CueKind::Answer) {
                    seen = std::any_of(frame.scene.packets.begin(), frame.scene.packets.end(), [&](const auto& packet) {
                        return packet.pdu == static_cast<int>(record.pdu->value) && packet.cable.a == packet.tail;
                    });
                    if (!seen)
                        seen = std::any_of(frame.scene.packets.begin(), frame.scene.packets.end(),
                                           [&](const auto& packet) { return packet.pdu == static_cast<int>(record.pdu->value); });
                } else {
                    seen = std::any_of(frame.scene.effects.begin(), frame.scene.effects.end(),
                                       [&](const auto& effect) { return effect.record == cue.record; });
                }
                if (!seen) std::cerr << "  " << item.label << ": " << resend::name(cue.kind) << " #" << cue.record << " is not seen\n";
                CHECK(seen);
            }
    }
}

} // namespace

int main() {
    const int changed = golden_files();
    a_ping_ends_as_it_went();
    sound_leaves_the_trace_alone();
    the_mixer_keeps_it_quiet();
    eight_voices_never_clip();
    every_cue_is_seen();
    std::cout << "Sound tests passed: " << test::checks << " checks.\n";
    return changed == 0 ? 0 : 1;
}

// Playback and scene: the app logic that needs no window.
#include "app/playback.hpp"
#include "app/scene.hpp"
#include "check.hpp"
#include "scenario/edit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

namespace {
using namespace std::chrono_literals;
using resend::PlaybackController;
using netsim::SimTime;

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
bool near(resend::Point a, resend::Point b) { return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f; }

// A ping from PC-A started at time 0, on the sample network unless told otherwise.
netsim::Simulation sample_ping(netsim::Scenario scenario = netsim::sample_lan(), const char* destination = "192.168.1.20") {
    netsim::Simulation simulation(std::move(scenario));
    netsim::Problem problem{};
    CHECK(simulation.start_ping(netsim::DeviceId{1}, *netsim::Ipv4Address::parse(destination), problem).has_value());
    return simulation;
}
bool succeeded(const netsim::Simulation& simulation) {
    const auto& pings = simulation.pings();
    return !pings.empty() && std::all_of(pings.begin(), pings.end(), [](const auto& ping) { return ping.state == netsim::PingState::Succeeded; });
}
void finish(netsim::Simulation& simulation) {
    while (simulation.step()) {}
}

void playback_follows_simulated_time() {
    auto engine = sample_ping();
    PlaybackController playback(engine);
    CHECK(!playback.playing() && !playback.finished() && playback.now() == 0ns);
    CHECK(!playback.tick(1s));  // paused: nothing moves
    CHECK(engine.trace().records().empty());

    playback.play();
    // The ARP request is due at once; then 10 ms of cable take 600 ms.
    CHECK(playback.tick(300ms));
    CHECK(playback.now() == 5ms);
    CHECK(engine.now() == 5ms);  // the engine is exactly at the displayed time
    auto flying = playback.in_flight();
    CHECK(flying.size() == 1 && flying[0].transmission == 0 && near(flying[0].progress, 0.5));

    CHECK(playback.tick(300ms));
    CHECK(playback.now() == 10ms);
    CHECK(engine.trace().records().size() == 6);  // the switch decided at 10 ms
    flying = playback.in_flight();
    CHECK(flying.size() == 1 && flying[0].transmission == 1 && near(flying[0].progress, 0));

    // The rest of the ping: seven more hops.
    for (int i = 0; i < 7; ++i) CHECK(playback.tick(600ms));
    CHECK(playback.now() == 80ms);
    CHECK(succeeded(engine));
    CHECK(!playback.tick(1s));  // nothing left: playback stops by itself
    CHECK(!playback.playing() && playback.finished());
    CHECK(playback.now() == 80ms);
}

void speed_and_steps() {
    auto engine = sample_ping();
    PlaybackController playback(engine);
    playback.set_speed(2);
    playback.play();
    playback.tick(150ms);
    CHECK(playback.now() == 5ms);
    playback.set_speed(0.25);
    playback.tick(600ms);
    CHECK(playback.now() == 7500us);
    playback.set_speed(100);
    CHECK(playback.speed() == PlaybackController::max_speed);
    playback.set_speed(0);
    CHECK(playback.speed() == PlaybackController::min_speed);
    playback.set_speed(1);

    // Step: animates up to the next event, processes it and pauses there.
    playback.step();
    CHECK(playback.playing());
    playback.tick(10s);
    CHECK(!playback.playing() && playback.now() == 10ms);
    CHECK(engine.trace().records().back().time == 10ms);
    playback.step();
    playback.tick(300ms);
    CHECK(playback.playing() && playback.now() == 15ms);
    playback.pause();
    CHECK(!playback.tick(300ms) && playback.now() == 15ms);

    // A step with events already due runs them at once.
    auto fresh = sample_ping();
    PlaybackController stepper(fresh);
    stepper.step();
    CHECK(!stepper.playing() && fresh.trace().transmissions().size() == 1 && stepper.now() == 0ns);
    CHECK(stepper.in_flight().size() == 1 && near(stepper.in_flight()[0].progress, 0));
}

// Ticks of uneven length, as a real frame timer produces: the event is
// reached exactly, never skipped, and playback stops by itself at the end.
void uneven_ticks_land_on_events() {
    auto engine = sample_ping();
    PlaybackController playback(engine);
    playback.play();
    const std::chrono::nanoseconds ticks[] = {16'666'667ns, 16'666'666ns, 17ms, 15'999'999ns, 33'333'333ns};
    for (int frame = 0; frame < 100'000 && playback.playing(); ++frame) {
        const auto before = engine.trace().records().size();
        const auto shown = playback.now();
        playback.tick(ticks[static_cast<std::size_t>(frame) % std::size(ticks)]);
        // Records appear between the instant shown before the tick and the one after.
        for (auto i = before; i < engine.trace().records().size(); ++i)
            CHECK(engine.trace().records()[i].time >= shown && engine.trace().records()[i].time <= playback.now());
        CHECK(!engine.next_event_time() || *engine.next_event_time() > playback.now());
    }
    CHECK(succeeded(engine) && playback.finished() && !playback.playing());
    CHECK(playback.now() == 80ms);

    // The same with steps.
    auto stepped = sample_ping();
    PlaybackController stepper(stepped);
    int steps = 0;
    while (!stepper.finished() && steps < 100) {
        stepper.step();
        ++steps;
        for (int frame = 0; frame < 1000 && stepper.playing(); ++frame) stepper.tick(ticks[static_cast<std::size_t>(frame) % std::size(ticks)]);
        CHECK(!stepper.playing());
    }
    CHECK(steps == 9 && succeeded(stepped));  // 0 ms, then one step per hop
}

void idle_gaps_are_compressed() {
    // The cable is down: no frame leaves, and three ARP timeouts of 1 s follow.
    auto unplugged = netsim::sample_lan();
    unplugged.links[0].up = false;
    auto engine = sample_ping(unplugged);
    PlaybackController playback(engine);
    playback.play();
    playback.tick(400ms);
    CHECK(playback.now() == 500ms);  // a 1 s wait lasts 800 ms at 1×
    playback.tick(400ms);
    CHECK(playback.now() == 1s);
    CHECK(engine.trace().records().back().kind == netsim::TraceKind::FrameDropped);  // second attempt at 1 s
    playback.tick(1600ms);
    CHECK(playback.now() == 3s && playback.finished() && !playback.playing());
    CHECK(!succeeded(engine));

    // Short idle gaps keep the normal pace: compression only speeds up.
    auto local = sample_ping(netsim::sample_lan(), "192.168.1.10");  // local ping, done at once
    PlaybackController still(local);
    still.play();
    still.tick(1ms);
    CHECK(still.finished() && succeeded(local));
}

void reduced_motion_shows_one_instant_at_a_time() {
    auto engine = sample_ping();
    PlaybackController playback(engine);
    playback.set_reduced_motion(true);
    playback.play();
    playback.tick(1ms);
    CHECK(playback.now() == 0ns && engine.trace().transmissions().size() == 1);
    playback.tick(698ms);
    CHECK(playback.now() == 0ns);
    playback.tick(1ms);
    CHECK(playback.now() == 10ms);  // straight to the next event, no motion in between
    playback.pause();
    playback.step();
    CHECK(playback.now() == 20ms && !playback.playing());
}

void playback_never_changes_the_result() {
    auto reference = sample_ping();
    finish(reference);

    for (const double speed : {0.25, 4.0}) {
        for (const auto tick : {std::chrono::nanoseconds(7ms), std::chrono::nanoseconds(5s)}) {
            auto engine = sample_ping();
            PlaybackController playback(engine);
            playback.set_speed(speed);
            playback.play();
            for (int i = 0; i < 100'000 && playback.playing(); ++i) playback.tick(tick);
            CHECK(engine.trace().records() == reference.trace().records());
            CHECK(engine.trace().transmissions() == reference.trace().transmissions());
        }
    }
}

void seek_and_reset() {
    auto scenario = netsim::sample_lan();
    netsim::DeviceId observer{};
    CHECK(netsim::edit::add_device(scenario, netsim::DeviceKind::Host, 0, 0, &observer) == netsim::Problem::None);
    CHECK(netsim::edit::connect(scenario, observer, netsim::DeviceId{3}) == netsim::Problem::None);
    auto engine = sample_ping(scenario);
    PlaybackController playback(engine);
    playback.seek(15ms);
    // Two copies of the flooded ARP request, halfway along their cables.
    const auto flying = playback.in_flight();
    CHECK(flying.size() == 2);
    for (const auto& item : flying) {
        CHECK(near(item.progress, 0.5));
        CHECK(engine.trace().transmissions()[item.transmission].start == 10ms);
    }
    playback.seek(5ms);  // never backwards
    CHECK(playback.now() == 15ms);

    engine = netsim::Simulation(scenario);  // rebuilt, as after an edit
    playback.reset();
    CHECK(playback.now() == 0ns && playback.in_flight().empty() && playback.finished());
}

void cable_geometry() {
    const resend::Box left{0, 0, 160, 94}, right{300, 0, 160, 94};
    auto cable = resend::cable_between(left, right);
    CHECK(near(cable.a, {165, 47}) && near(cable.b, {295, 47}));
    cable = resend::cable_between(right, left, 0);
    CHECK(near(cable.a, {300, 47}) && near(cable.b, {160, 47}));
    // Below and to the right: the cable leaves through the bottom edge.
    const resend::Box below{200, 300, 160, 94};
    cable = resend::cable_between(left, below, 0);
    CHECK(near(cable.a, {80 + 200 * 47.0f / 300, 94}));
    CHECK(near(cable.b, {280 - 200 * 47.0f / 300, 300}));
    const auto same = resend::cable_between(left, left);
    CHECK(near(same.a, left.center()) && near(same.b, left.center()));
    CHECK(near(resend::lerp({0, 0}, {10, 20}, 0.25f), {2.5f, 5}));
}

void scene_at_fifteen_ms() {
    auto engine = sample_ping();
    PlaybackController playback(engine);
    playback.seek(15ms);
    const auto scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.packets.size() == 1);
    const auto& packet = scene.packets[0];
    CHECK(packet.pdu == 0 && packet.broadcast && packet.kind == netsim::PduKind::ArpRequest);
    CHECK(packet.label == "ARP req");
    CHECK(packet.description == "ARP Request, broadcast, Switch-01 → PC-B");
    const auto& layout = engine.scenario();
    const auto cable = resend::cable_between(resend::card_box(*layout.device(netsim::DeviceId{3})),
                                             resend::card_box(*layout.device(netsim::DeviceId{2})));
    CHECK(near(packet.at, resend::lerp(cable.a, cable.b, 0.5f)));
    // The tail trails behind, towards the switch.
    CHECK(std::hypot(packet.at.x - packet.tail.x, packet.at.y - packet.tail.y) > 27.9f);
    CHECK((packet.tail.x - packet.at.x) * (cable.a.x - cable.b.x) > 0);

    // The switch's decisions at 10 ms are still on screen, oldest on top.
    CHECK(scene.markers.size() == 2);
    CHECK(scene.markers[0].text == "learn PC-A → p1" && scene.markers[0].device == 3);
    CHECK(scene.markers[1].text == "flood ×1" && scene.markers[1].tone == resend::Tone::Forward);
    CHECK(scene.markers[0].stack == 1 && scene.markers[1].stack == 0);
    CHECK(near(scene.markers[0].anchor, {310 + 80, 280}));
    CHECK(scene.markers[0].opacity == 1.0f);
    CHECK(scene.port_activity.empty());  // the switch's ports lit at 10 ms, 5 ms ago

    playback.seek(17ms);
    const auto later = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(later.markers.size() == 2 && later.markers[0].opacity < 1.0f && later.markers[0].opacity > 0.1f);
    playback.seek(19ms);
    CHECK(resend::build_scene(engine.scenario(), engine, playback).markers.empty());
}

void scene_decisions() {
    auto engine = sample_ping();
    PlaybackController playback(engine);
    playback.seek(30ms);
    auto scene = resend::build_scene(engine.scenario(), engine, playback);
    // The reply goes back through the port the switch learned at 10 ms.
    CHECK(scene.markers.size() == 2);
    CHECK(scene.markers[0].text == "learn PC-B → p2");
    CHECK(scene.markers[1].text == "forward → p1");
    CHECK(scene.port_activity.size() == 1 && (scene.port_activity[0] == std::pair{3, 0b11U}));

    playback.seek(40ms);
    scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.markers.size() == 1 && scene.markers[0].text == "ARP + 192.168.1.20" && scene.markers[0].device == 1);

    // The result stays on the source host until new frames are sent.
    playback.seek(500ms);
    scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.packets.empty());
    CHECK(scene.markers.size() == 1 && scene.markers[0].text == "✓ ping reply" && scene.markers[0].tone == resend::Tone::Success);

    // A powered-off destination: the request stops where it arrives.
    auto powered_off = netsim::sample_lan();
    powered_off.devices[1].powered = false;
    auto off = sample_ping(powered_off);
    PlaybackController watch(off);
    watch.seek(20ms);
    scene = resend::build_scene(off.scenario(), off, watch);
    CHECK(scene.markers.size() == 1 && scene.markers[0].text == "× device off" && scene.markers[0].device == 2);
    watch.seek(3s);
    scene = resend::build_scene(off.scenario(), off, watch);
    CHECK(scene.markers.size() == 2 && scene.markers[0].text == "ARP timeout" && scene.markers[1].text == "× ping: no ARP reply");
}

void frame_layers() {
    auto engine = sample_ping();
    finish(engine);
    const auto& pdus = engine.trace().pdus();
    CHECK(resend::describe_layers(pdus[0]) ==
          "Ethernet II · dst ff:ff:ff:ff:ff:ff · src 02:00:00:00:00:01 · type 0x0806 (ARP)\n"
          "ARP · request · sender 02:00:00:00:00:01 192.168.1.10 · target 192.168.1.20");
    CHECK(resend::describe_layers(pdus[1]) ==
          "Ethernet II · dst 02:00:00:00:00:01 · src 02:00:00:00:00:02 · type 0x0806 (ARP)\n"
          "ARP · reply · sender 02:00:00:00:00:02 192.168.1.20 · target 02:00:00:00:00:01 192.168.1.10");
    CHECK(resend::describe_layers(pdus[2]) ==
          "Ethernet II · dst 02:00:00:00:00:02 · src 02:00:00:00:00:01 · type 0x0800 (IPv4)\n"
          "IPv4 · 192.168.1.10 → 192.168.1.20 · TTL 64 · id 1 · protocol 1 (ICMP)\n"
          "ICMP · type 8 Echo Request · code 0 · identifier 1 · sequence 1");
    CHECK(resend::describe_layers(pdus[3]).ends_with("ICMP · type 0 Echo Reply · code 0 · identifier 1 · sequence 1"));
    CHECK(resend::packet_label(netsim::PduKind::EchoReply) == "Echo rep");
}
} // namespace

int main() {
    playback_follows_simulated_time();
    speed_and_steps();
    uneven_ticks_land_on_events();
    idle_gaps_are_compressed();
    reduced_motion_shows_one_instant_at_a_time();
    playback_never_changes_the_result();
    seek_and_reset();
    cable_geometry();
    scene_at_fifteen_ms();
    scene_decisions();
    frame_layers();
    std::cout << "App tests passed: " << test::checks << " checks.\n";
}

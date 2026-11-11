// Playback and scene: the app logic that needs no window.
#include "app/playback.hpp"
#include "app/scene.hpp"
#include "check.hpp"
#include "scenario/edit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
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
    CHECK(succeeded(engine) && playback.finished());
    // Nothing left: the afterglow plays out at the normal pace, 12 ms in
    // 720 ms, so that markers and effects end; then playback stops by itself.
    CHECK(playback.tick(360ms) && playback.playing() && playback.now() == 86ms);
    CHECK(playback.tick(1s) && !playback.playing());
    CHECK(playback.now() == 92ms && !playback.tick(1s));
    CHECK(engine.trace().records().back().time == 80ms);  // and adds nothing to the trace
    playback.play();
    CHECK(!playback.playing());  // played out: Run has nothing left to do
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
    CHECK(playback.now() == 80ms + PlaybackController::afterglow);

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
    const auto close_to = [](netsim::SimTime a, netsim::SimTime b) { return a - b < 1us && b - a < 1us; };
    playback.play();
    // The drop first plays out at the normal pace: 12 ms in 720 ms.
    playback.tick(720ms);
    CHECK(playback.now() == PlaybackController::afterglow);
    // Then the rest of the 1 s wait lasts 800 ms at 1×.
    playback.tick(400ms);
    CHECK(close_to(playback.now(), 12ms + 494ms));
    playback.tick(400ms);
    CHECK(playback.now() == 1s);
    CHECK(engine.trace().records().back().kind == netsim::TraceKind::FrameDropped);  // second attempt at 1 s
    playback.tick(1520ms);
    CHECK(close_to(playback.now(), 2s));
    playback.tick(1520ms);
    CHECK(close_to(playback.now(), 3s) && playback.finished() && playback.playing());
    playback.tick(1s);  // the afterglow of the last record
    CHECK(playback.now() == 3s + PlaybackController::afterglow && !playback.playing());
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
    CHECK(scene.device_activity.empty());  // and it sent then

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
    // It received the reply and sent it on at once: sending shows.
    CHECK(scene.device_activity.size() == 1 && (scene.device_activity[0] == std::pair{3, resend::look::DeviceState::Sending}));
    playback.seek(32ms);
    CHECK(resend::build_scene(engine.scenario(), engine, playback).device_activity.size() == 1);
    playback.seek(35ms);
    CHECK(resend::build_scene(engine.scenario(), engine, playback).device_activity.empty());

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
    // A drop that is a problem puts its device in error, for as long as its marker.
    CHECK(std::ranges::count(scene.device_activity, std::pair{2, resend::look::DeviceState::Error}) == 1);
    CHECK(resend::is_problem(netsim::Reason::TtlExceeded) && !resend::is_problem(netsim::Reason::NotForUs) &&
          !resend::is_problem(netsim::Reason::ArpForOtherHost) && !resend::is_problem(netsim::Reason::Ipv4NotForUs));
    watch.seek(3s);
    scene = resend::build_scene(off.scenario(), off, watch);
    CHECK(scene.markers.size() == 2 && scene.markers[0].text == "ARP timeout" && scene.markers[1].text == "× ping: no ARP reply");
}

void scene_with_live_changes() {
    // The cable to PC-B disabled while the ARP request is on it: the frame
    // stops where it arrives.
    auto engine = sample_ping();
    PlaybackController playback(engine);
    playback.seek(15ms);
    CHECK(engine.apply(netsim::SetLinkUp{netsim::LinkId{2}, false}) == netsim::Problem::None);
    playback.seek(20ms);
    auto scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.packets.empty());
    CHECK(scene.markers.size() == 1 && scene.markers[0].text == "× link down" && scene.markers[0].device == 2);

    // A device edit leaves a chip of its own on the device.
    CHECK(engine.apply(netsim::SetPowered{netsim::DeviceId{2}, false}) == netsim::Problem::None);
    scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.markers.back().text == "powered off" && scene.markers.back().icon == "power" && scene.markers.back().tone == resend::Tone::Edit);
    CHECK(engine.apply(netsim::SetAddress{{netsim::DeviceId{2}, 0}, *netsim::Ipv4Prefix::parse("192.168.1.21/24")}) ==
          netsim::Problem::None);
    scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.markers.back().text == "IP 192.168.1.21/24");
    // A cable edit leaves none: the cable shows its state.
    const auto before = scene.markers.size();
    CHECK(engine.apply(netsim::SetLinkUp{netsim::LinkId{2}, true}) == netsim::Problem::None);
    CHECK(resend::build_scene(engine.scenario(), engine, playback).markers.size() == before);

    // A removed PC: frames to it disappear with its card.
    auto removed = sample_ping();
    PlaybackController watch(removed);
    watch.seek(15ms);
    CHECK(removed.apply(netsim::RemoveDevice{netsim::DeviceId{2}}) == netsim::Problem::None);
    CHECK(resend::build_scene(removed.scenario(), removed, watch).packets.empty());
}

void scene_of_a_storm() {
    // Three switches in a ring: the guard's chip, then nothing on the cables.
    netsim::Scenario ring;
    for (int i = 0; i < 3; ++i) CHECK(netsim::edit::add_device(ring, netsim::DeviceKind::Switch, 200 * i, 300) == netsim::Problem::None);
    netsim::DeviceId pc{};
    CHECK(netsim::edit::add_device(ring, netsim::DeviceKind::Host, 0, 0, &pc) == netsim::Problem::None);
    CHECK(netsim::edit::connect(ring, pc, netsim::DeviceId{1}) == netsim::Problem::None);
    CHECK(netsim::edit::connect(ring, netsim::DeviceId{1}, netsim::DeviceId{2}) == netsim::Problem::None);
    CHECK(netsim::edit::connect(ring, netsim::DeviceId{2}, netsim::DeviceId{3}) == netsim::Problem::None);
    CHECK(netsim::edit::connect(ring, netsim::DeviceId{3}, netsim::DeviceId{1}) == netsim::Problem::None);
    for (auto& cable : ring.links) cable.delay = 1ms;  // many decisions within a chip's life
    netsim::Simulation engine(ring);
    netsim::Problem problem{};
    CHECK(engine.start_ping(pc, *netsim::Ipv4Address::parse("192.168.1.99"), problem).has_value());
    PlaybackController playback(engine);
    // At 4 ms both copies of the broadcast come back to Switch-1 at once,
    // one from each side of the ring: each flood sends two copies, like the
    // first one at 1 ms.
    playback.seek(4ms);
    const auto early = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(std::count_if(early.markers.begin(), early.markers.end(), [](const auto& marker) {
              return marker.device == 1 && marker.text == "flood ×2";
          }) == 3);
    playback.set_reduced_motion(true);  // a step is then immediate
    const auto& records = engine.trace().records();
    while (std::none_of(records.begin(), records.end(), [](const auto& record) { return record.kind == netsim::TraceKind::SimulatorGuard; }))
        playback.step();
    const auto guard = std::find_if(records.begin(), records.end(), [](const auto& record) { return record.kind == netsim::TraceKind::SimulatorGuard; });
    CHECK(playback.now() == guard->time);
    const auto scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(std::any_of(scene.markers.begin(), scene.markers.end(), [](const auto& marker) {
        return marker.text == "storm stopped" && marker.icon == "warning" && marker.tone == resend::Tone::Guard;
    }));
    CHECK(std::none_of(scene.packets.begin(), scene.packets.end(), [&](const auto& packet) {
        return packet.pdu == static_cast<int>(guard->pdu->value);
    }));
    // The storm leaves many decisions; each card shows its latest few.
    std::map<int, int> chips;
    for (const auto& marker : scene.markers) {
        CHECK(marker.stack < resend::max_chips);
        ++chips[marker.device];
    }
    CHECK(std::any_of(chips.begin(), chips.end(), [](const auto& item) { return item.second == resend::max_chips; }));
}

void scene_of_a_long_ping() {
    // A ping of several requests: a chip per reply, then the tally.
    netsim::Simulation engine(netsim::sample_lan());
    netsim::Problem problem{};
    CHECK(engine.start_ping(netsim::DeviceId{1}, *netsim::Ipv4Address::parse("192.168.1.20"), problem, {.count = 3}).has_value());
    PlaybackController playback(engine);
    playback.seek(1040ms);
    auto scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.markers.size() == 1 && scene.markers[0].text == "✓ reply 2" && scene.markers[0].device == 1);
    playback.seek(2040ms);
    scene = resend::build_scene(engine.scenario(), engine, playback);
    CHECK(scene.markers.size() == 2 && scene.markers[0].text == "✓ reply 3" && scene.markers[1].text == "✓ ping 3/3");

    // A reply after its request timed out.
    netsim::Simulation late(netsim::sample_lan());
    CHECK(late.start_ping(netsim::DeviceId{1}, *netsim::Ipv4Address::parse("192.168.1.20"), problem, {.timeout = 10ms}).has_value());
    PlaybackController watch(late);
    watch.seek(80ms);
    scene = resend::build_scene(late.scenario(), late, watch);
    CHECK(std::any_of(scene.markers.begin(), scene.markers.end(), [](const auto& marker) { return marker.text == "× reply too late"; }));
}

void frame_names() {
    CHECK(resend::packet_label(netsim::PduKind::EchoReply) == "Echo rep");
    CHECK(resend::frame_name(netsim::PduKind::ArpRequest) == "ARP Request");
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
    scene_with_live_changes();
    scene_of_a_storm();
    scene_of_a_long_ping();
    frame_names();
    std::cout << "App tests passed: " << test::checks << " checks.\n";
}

// The workspace: undo and redo, the command log and going back in time.
#include "app/playback.hpp"
#include "app/timeline.hpp"
#include "app/workspace.hpp"
#include "check.hpp"
#include "sim_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace simtest;
using netsim::DeviceKind;
using netsim::SimTime;
using netsim::LinkId;
using netsim::PortRef;
using resend::Workspace;

const DeviceId pc_a{1}, pc_b{2}, sw{3};

std::uint64_t trace_hash(const Simulation& simulation) {
    Hash hash;
    add_trace(hash, simulation);
    return hash.value;
}

void edits_before_anything_happened() {
    // Nothing has happened yet: an edit gives a new network, no command is
    // recorded, and undo gives back the network as it was.
    Workspace workspace;
    const auto first = workspace.scenario();
    CHECK(!workspace.modified() && !workspace.history().undoable());
    const auto generation = workspace.generation();
    CHECK(workspace.change(netsim::AddDevice{DeviceKind::Host, 40, 50}, "Add a host").ok());
    CHECK(workspace.generation() == generation + 1 && workspace.commands().empty());
    CHECK(workspace.scenario().devices.size() == 4 && workspace.scenario().devices.back().id == DeviceId{4});
    CHECK(workspace.scenario() == workspace.design() && workspace.modified());
    CHECK(workspace.history().undoable()->label == "Add a host");

    // Moves do not rebuild the simulation.
    CHECK(workspace.change(netsim::MoveDevice{pc_a, 7, 8}, "Move PC-A").ok());
    CHECK(workspace.generation() == generation + 1 && workspace.scenario().device(pc_a)->x == 7);

    CHECK(workspace.undo().ok() && workspace.undo().ok());
    CHECK(workspace.scenario() == first && !workspace.modified());
    CHECK(!workspace.history().undoable() && workspace.history().redoable()->label == "Add a host");
    CHECK(workspace.undo().ok() && workspace.scenario() == first);  // nothing left to undo
    CHECK(workspace.redo().ok() && workspace.scenario().devices.back().id == DeviceId{4});
    CHECK(workspace.redo().ok() && workspace.scenario().device(pc_a)->x == 7);
    CHECK(!workspace.history().redoable());

    // A refused step changes nothing, all edits or none; one that changes
    // nothing is not a step.
    const auto before = workspace.scenario();
    const auto steps = workspace.history().size();
    const auto refused = workspace.change({netsim::Rename{pc_a, "Desk"}, netsim::SetPowered{DeviceId{9}, false}}, "Two edits");
    CHECK(!refused.ok() && refused.problem == Problem::NoSuchDevice && !refused.in_past);
    CHECK(workspace.scenario() == before && workspace.history().size() == steps);
    CHECK(workspace.change(netsim::SetLinkUp{LinkId{1}, true}, "Enable cable 1").ok());
    CHECK(workspace.history().size() == steps);

    // Two new devices in one step get two ids, and come back with them.
    CHECK(workspace.change({netsim::AddDevice{DeviceKind::Switch, 0, 0}, netsim::AddDevice{DeviceKind::Host, 0, 0}}, "Add two").ok());
    const auto& devices = workspace.scenario().devices;
    CHECK(devices.size() == 6 && devices[4].id == DeviceId{5} && devices[5].id == DeviceId{6});
    const auto two = workspace.scenario();
    CHECK(workspace.undo().ok() && workspace.scenario() == before);
    CHECK(workspace.redo().ok() && workspace.scenario() == two);
}

void drags_are_one_step() {
    Workspace workspace;
    const auto first = workspace.scenario();
    for (int x = 0; x < 30; ++x) CHECK(workspace.change(netsim::MoveDevice{pc_a, x, 2 * x}, "Move PC-A", true).ok());
    workspace.end_gesture();
    CHECK(workspace.history().size() == 1);
    CHECK(workspace.history().undoable()->forward.size() == 1 && workspace.history().undoable()->backward.size() == 1);
    // The next drag of the same card is a step of its own.
    CHECK(workspace.change(netsim::MoveDevice{pc_a, 100, 100}, "Move PC-A", true).ok());
    CHECK(workspace.change(netsim::MoveDevice{pc_a, 120, 100}, "Move PC-A", true).ok());
    workspace.end_gesture();
    CHECK(workspace.history().size() == 2);
    CHECK(workspace.undo().ok() && workspace.scenario().device(pc_a)->x == 29);
    CHECK(workspace.undo().ok() && workspace.scenario() == first);
    // Nudges with the keyboard are steps each.
    CHECK(workspace.change(netsim::MoveDevice{pc_a, 110, 130}, "Move PC-A").ok());
    CHECK(workspace.change(netsim::MoveDevice{pc_a, 120, 130}, "Move PC-A").ok());
    CHECK(workspace.history().size() == 2 && !workspace.history().redoable());
    // A gesture does not join a step of another kind.
    CHECK(workspace.change(netsim::MoveDevice{pc_b, 0, 0}, "Move PC-B", true).ok());
    CHECK(workspace.history().size() == 3);
}

void live_edits_are_commands() {
    // Once a ping is sent, edits and undos happen at the instant on screen
    // and are recorded as commands.
    Workspace workspace;
    const auto first = workspace.scenario();
    CHECK(workspace.ping(pc_a, ip("192.168.1.20")).ok());
    CHECK(workspace.commands().size() == 1);
    const auto generation = workspace.generation();
    workspace.run_until(15ms);
    CHECK(workspace.change(netsim::RemoveDevice{sw}, "Remove Switch-01").ok());
    CHECK(workspace.generation() == generation && workspace.commands().size() == 2);
    CHECK(workspace.commands().back().time == 15ms);
    workspace.run_until(16ms);
    CHECK(workspace.undo().ok() && workspace.scenario() == first);
    CHECK(workspace.commands().size() == 3 && workspace.commands().back().time == 16ms);
    CHECK(count(workspace.simulation(), TraceKind::EditApplied) == 2);
    workspace.run_until(5s);
    CHECK(workspace.simulation().pings().front().state == netsim::PingState::Succeeded);

    // A refused ping is no command.
    const auto refused = workspace.ping(sw, ip("192.168.1.20"));
    CHECK(refused.problem == Problem::SourceNotHost && workspace.commands().size() == 3);

    // Reset: a new simulation of the same network, history kept.
    workspace.reset();
    CHECK(workspace.generation() == generation + 1 && workspace.commands().empty());
    CHECK(workspace.simulation().trace().records().empty() && workspace.history().redoable());
    CHECK(workspace.redo().ok() && !workspace.scenario().device(sw));
    CHECK(workspace.generation() == generation + 2);  // nothing had happened: a new network

    // Another network: no history, nothing modified.
    workspace.replace(star(2));
    CHECK(!workspace.history().undoable() && !workspace.history().redoable() && !workspace.modified());
    CHECK(workspace.change(netsim::Rename{DeviceId{2}, "Desk"}, "Rename").ok() && workspace.modified());
    workspace.mark_saved();
    CHECK(!workspace.modified());
    CHECK(workspace.undo().ok() && workspace.modified());
    CHECK(workspace.redo().ok() && !workspace.modified());
    workspace.mark_modified();
    CHECK(workspace.modified());
}

// A ping, edits and an undo at various instants, then the run to its end.
Workspace eventful_run() {
    Workspace workspace;
    CHECK(workspace.ping(pc_a, ip("192.168.1.20"), {.count = 3}).ok());
    workspace.run_until(15ms);
    CHECK(workspace.change(netsim::SetLinkUp{LinkId{2}, false}, "Disable cable 2").ok());
    CHECK(workspace.change(netsim::MoveDevice{pc_b, 600, 200}, "Move PC-B").ok());
    workspace.run_until(1200ms);
    CHECK(workspace.undo().ok());  // the move
    CHECK(workspace.undo().ok());  // cable 2 back up
    CHECK(workspace.ping(pc_b, ip("192.168.1.10")).ok());
    workspace.run_until(1500ms);
    CHECK(workspace.change(netsim::AddDevice{DeviceKind::Host, 0, 0}, "Add a host").ok());
    CHECK(workspace.change(netsim::Connect{PortRef{DeviceId{4}, 0}, PortRef{sw, 4}}, "Connect").ok());
    CHECK(workspace.ping(DeviceId{4}, ip("192.168.1.10")).ok());
    while (const auto next = workspace.next_event_time()) workspace.run_until(*next);
    return workspace;
}

void travel_in_time() {
    // Going back replays the commands on a new simulation: the same run.
    auto workspace = eventful_run();
    const auto end = workspace.end_time();
    const auto full = trace_hash(workspace.simulation());
    const auto latest = workspace.scenario();
    CHECK(workspace.commands().size() == 9 && !workspace.in_past());
    CHECK(workspace.simulation().pings().size() == 3);

    // A run made directly up to each instant matches the replay to it.
    for (const SimTime instant : {SimTime{0}, SimTime{15ms}, SimTime{20ms}, SimTime{1200ms}, SimTime{1300ms}, SimTime{1500ms}, end}) {
        const auto generation = workspace.generation();
        const bool back = instant < workspace.simulation().now();
        workspace.travel(instant);
        CHECK(workspace.simulation().now() == instant);
        CHECK(workspace.generation() == generation + (back ? 1 : 0));
        auto direct = eventful_run();
        direct.travel(instant);
        CHECK(trace_hash(workspace.simulation()) == trace_hash(direct.simulation()));
        CHECK(workspace.scenario() == direct.scenario());
        CHECK(workspace.design() == latest);
        CHECK(workspace.in_past() == (instant < 1500ms));
    }
    CHECK(trace_hash(workspace.simulation()) == full);

    // In the past the network cannot change, and the history stays.
    workspace.travel(15ms);
    CHECK(workspace.in_past() && workspace.scenario().links.size() == 2);
    const auto refused = workspace.change(netsim::SetLinkUp{LinkId{1}, false}, "Disable cable 1");
    CHECK(refused.in_past && !refused.ok());
    CHECK(workspace.undo().in_past && workspace.redo().in_past && workspace.ping(pc_a, ip("192.168.1.20")).in_past);
    CHECK(workspace.end_time() == end && workspace.history().undoable()->label == "Connect");

    // The playback makes the commands again as it passes their instant.
    resend::PlaybackController playback(workspace);
    playback.set_reduced_motion(true);
    playback.play();
    for (int frames = 0; playback.playing() && frames < 100'000; ++frames) playback.tick(1s);
    CHECK(!workspace.in_past() && workspace.scenario() == latest);
    CHECK(trace_hash(workspace.simulation()) == full);

    // Back in time with nothing ahead, a change discards only what the
    // simulation had computed after it.
    workspace.travel(1600ms);
    CHECK(!workspace.in_past());
    CHECK(workspace.change(netsim::SetLinkUp{LinkId{1}, false}, "Disable cable 1").ok());
    CHECK(workspace.end_time() == 1600ms && workspace.commands().back().time == 1600ms);
}

void event_filters() {
    const auto workspace = eventful_run();
    netsim::explain::Explainer explainer;
    explainer.update(workspace.simulation());
    const auto& events = explainer.events();
    const auto all = resend::filter_events(events, {});
    CHECK(all.size() == events.size() && !resend::EventFilter{}.active());
    const auto count_of = [&](const resend::EventFilter& filter) { return resend::filter_events(events, filter).size(); };
    const auto with = [&](const char* protocol) {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const auto& event) { return event.protocol == protocol; }));
    };
    CHECK(with("ARP") > 0 && with("ICMP") > 0 && with("EDIT") == 4);  // moves are silent
    resend::EventFilter filter;
    filter.arp = false;
    CHECK(filter.active() && count_of(filter) == events.size() - with("ARP"));
    filter.icmp = filter.ipv4 = filter.simulator = false;
    CHECK(count_of(filter) == with("EDIT"));
    filter = {};
    filter.edits = false;
    CHECK(count_of(filter) == events.size() - with("EDIT"));
    // Problems: what was dropped, lost or failed.
    filter = {};
    filter.problems_only = true;
    const auto problems = resend::filter_events(events, filter);
    CHECK(!problems.empty() && problems.size() < events.size());
    for (const auto index : problems) CHECK(!events[index].success);
    // One device: what it sent and what reached it, in order.
    filter = {};
    filter.device = DeviceId{4};
    const auto pc_c = resend::filter_events(events, filter);
    CHECK(!pc_c.empty() && std::is_sorted(pc_c.begin(), pc_c.end()));
    for (const auto index : pc_c) CHECK(events[index].from == DeviceId{4} || events[index].to == DeviceId{4});
    filter.device = DeviceId{99};
    CHECK(count_of(filter) == 0);
}

void random_commands_and_undos() {
    // The exit check of milestone M4: 50 random commands, then 50 undos,
    // give back the first network exactly, while pings run and time passes;
    // 50 redos give back the last one.
    int steps = 0, live = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        netsim::Rng rng(seed);
        Workspace workspace(star(static_cast<int>(rng.between(1, 5))));
        const auto first = workspace.scenario();
        int made = 0;
        for (int attempt = 0; made < 50 && attempt < 1000; ++attempt) {
            if (rng.chance(1, 5) && !workspace.scenario().devices.empty()) {
                const auto& devices = workspace.scenario().devices;
                const auto source = devices[rng.below(devices.size())].id;
                const auto destination = Ipv4Address(0xc0a80100U | static_cast<std::uint32_t>(rng.between(9, 14)));
                workspace.ping(source, destination);
            }
            const auto edit = random_edit(rng, workspace.scenario());
            const bool gesture = rng.chance(1, 4);
            const auto steps_before = workspace.history().size();
            if (workspace.change(edit, "Step " + std::to_string(made), gesture).ok() && workspace.history().size() > steps_before) {
                ++made;
                if (!workspace.commands().empty()) ++live;
            }
            if (gesture) workspace.end_gesture();
            workspace.run_until(workspace.simulation().now() + std::chrono::milliseconds(rng.between(0, 30)));
        }
        CHECK(made == 50);
        steps += made;
        const auto last = workspace.scenario();
        for (int count = 0; count < made; ++count) {
            CHECK(workspace.undo().ok());
            workspace.run_until(workspace.simulation().now() + std::chrono::milliseconds(rng.between(0, 30)));
        }
        CHECK(workspace.scenario() == first && !workspace.history().undoable());
        CHECK(!workspace.modified());
        for (int count = 0; count < made; ++count) CHECK(workspace.redo().ok());
        CHECK(workspace.scenario() == last && !workspace.history().redoable());

        // Replaying the whole run gives the same trace.
        const auto end = workspace.end_time();
        const auto full = trace_hash(workspace.simulation());
        workspace.travel(0ns);
        workspace.travel(end);
        CHECK(trace_hash(workspace.simulation()) == full && workspace.scenario() == last);
    }
    CHECK(live > 1000);
    std::cout << "  " << steps << " random steps undone and redone, " << live << " of them on a running network\n";
}

} // namespace

int main() {
    edits_before_anything_happened();
    drags_are_one_step();
    live_edits_are_commands();
    travel_in_time();
    event_filters();
    random_commands_and_undos();
    std::cout << "Workspace tests passed: " << test::checks << " checks.\n";
}

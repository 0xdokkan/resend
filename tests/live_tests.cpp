// The network while it runs: edits at the current time, caches and tables
// that age, ping as an application, and the storm guard.

#include "check.hpp"
#include "core/rng.hpp"
#include "explain/explain.hpp"
#include "sim_support.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace simtest;
using netsim::ArpState;
using netsim::DeviceKind;
using netsim::EchoState;
using netsim::LinkId;
using netsim::PingState;
using netsim::PortRef;
using netsim::TraceRecord;
namespace edit = netsim::edit;

// The sample LAN: PC-A (192.168.1.10) on cable 1 to the switch's first
// port, PC-B (192.168.1.20) on cable 2 to its second; 10 ms each.
const DeviceId pc_a{1}, pc_b{2}, sw{3};

std::vector<TraceRecord> records_of(const Simulation& simulation, TraceKind kind) {
    std::vector<TraceRecord> found;
    for (const auto& record : simulation.trace().records())
        if (record.kind == kind) found.push_back(record);
    return found;
}

netsim::Ipv4Prefix prefix(const char* text) { return *netsim::Ipv4Prefix::parse(text); }

void cable_down_in_flight() {
    // A frame on a cable that stops carrying is lost when it reaches the
    // end, even if the cable carries again by then: it is another connection.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(15ms);  // the ARP request is on PC-B's cable
    CHECK(simulation.frames_in_flight() == 1);
    CHECK(simulation.apply(netsim::SetLinkUp{LinkId{2}, false}) == Problem::None);
    CHECK(simulation.apply(netsim::SetLinkUp{LinkId{2}, true}) == Problem::None);
    CHECK(simulation.frames_in_flight() == 1);  // until it reaches the end
    simulation.run_until(20ms);
    CHECK(simulation.frames_in_flight() == 0);
    CHECK(count(simulation, TraceKind::FrameDropped, pc_b, Reason::LinkDown) == 1);
    CHECK(count(simulation, TraceKind::FrameReceived, pc_b) == 0);

    // Each edit is an episode of its own; the log keeps the cable as it was.
    const auto& edits = simulation.edits();
    CHECK(edits.size() == 2 && edits[0].time == 15ms && edits[1].time == 15ms);
    CHECK((edits[0].edit == netsim::Edit{netsim::SetLinkUp{LinkId{2}, false}}));
    CHECK(edits[0].links.size() == 1 && edits[0].links[0].up && !edits[1].links[0].up);
    const auto applied = records_of(simulation, TraceKind::EditApplied);
    CHECK(applied.size() == 2);
    for (std::uint32_t i = 0; i < applied.size(); ++i) CHECK(applied[i].value == i && applied[i].link == LinkId{2});
    CHECK(applied[0].episode < applied[1].episode);

    // The second ARP attempt crosses the cable.
    drain(simulation);
    const auto& status = *simulation.ping(id);
    CHECK(status.state == PingState::Succeeded && status.echoes.size() == 1);
    CHECK(status.echoes[0].sent == 0ms && status.echoes[0].finished == 1080ms);
    CHECK(arp_requests(simulation, pc_a) == 2);
}

void delays_and_ports() {
    // A new delay applies to the frames sent after it; the frame on the
    // cable keeps its own.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(5ms);
    CHECK(simulation.apply(netsim::SetDelay{LinkId{1}, 1ms}) == Problem::None);
    drain(simulation);
    CHECK(simulation.trace().transmissions().at(0).arrival == 10ms);
    CHECK(count(simulation, TraceKind::FrameDropped, std::nullopt, Reason::LinkDown) == 0);
    // ARP reply at the switch at 30 ms, at PC-A at 31; the Echo Reply at 53.
    CHECK(simulation.ping(id)->finished == 53ms);

    // Disabling a port stops its cable: PC-B's ARP reply is lost at the
    // switch, and the switch cannot reach PC-B any more.
    Simulation second(netsim::sample_lan());
    const auto lost = ping(second, pc_a, ip("192.168.1.20"));
    second.run_until(25ms);
    CHECK(second.apply(netsim::SetPortUp{PortRef{sw, 1}, false}) == Problem::None);
    const auto note = records_of(second, TraceKind::EditApplied).at(0);
    CHECK(note.device == sw && note.port == 1 && note.link == LinkId{2});
    drain(second);
    CHECK(count(second, TraceKind::FrameDropped, sw, Reason::LinkDown) == 1);
    CHECK(count(second, TraceKind::FrameReceived, pc_b) == 1);
    CHECK(second.ping(lost)->reason == Reason::ArpUnresolved);
}

void unplug_and_plug() {
    // Unplugged while a frame is on it: the frame is lost and the cable gone.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(15ms);
    CHECK(simulation.apply(netsim::Disconnect{LinkId{2}}) == Problem::None);
    CHECK(!simulation.scenario().link(LinkId{2}));
    CHECK(simulation.edits().back().links.size() == 1 && simulation.edits().back().links[0].id == LinkId{2});
    drain(simulation);
    CHECK(count(simulation, TraceKind::FrameDropped, pc_b, Reason::LinkDown) == 1);
    CHECK(simulation.ping(id)->reason == Reason::ArpUnresolved);
    // Plugged in again: a new cable with a new id, so the records about the
    // old one keep their meaning.
    CHECK(simulation.apply(netsim::Connect{PortRef{pc_b, 0}, PortRef{sw, 1}}) == Problem::None);
    CHECK(simulation.scenario().links.back().id == LinkId{3});
    CHECK(records_of(simulation, TraceKind::EditApplied).back().link == LinkId{3});
    const auto again = ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.ping(again)->state == PingState::Succeeded);
}

void moved_cable() {
    // PC-3's cable moves from switch port 2 to port 5. The switch sends
    // PC-3's frames to the old port until a frame from PC-3 arrives on the
    // new one.
    const auto scenario = star(2);
    const DeviceId sw1{1}, pc2{2}, pc3{3};
    const auto mac3 = scenario.device(pc3)->ports[0].mac;
    Simulation simulation(scenario);
    ping(simulation, pc2, address_of(scenario, pc3));
    drain(simulation);
    CHECK(simulation.apply(netsim::Replug{LinkId{2}, PortRef{sw1, 1}, PortRef{sw1, 4}}) == Problem::None);
    CHECK(simulation.scenario().link(LinkId{2})->b == (PortRef{sw1, 4}));

    const auto lost = ping(simulation, pc2, address_of(scenario, pc3));
    drain(simulation);
    CHECK(simulation.ping(lost)->reason == Reason::EchoTimeout);
    const auto drops = records_of(simulation, TraceKind::FrameDropped);
    CHECK(std::count_if(drops.begin(), drops.end(), [&](const auto& record) {
              return record.device == sw1 && record.reason == Reason::NoCarrier && record.port == 1;
          }) == 1);

    // PC-3 speaks: the switch learns the new port, and PC-2 reaches it.
    const auto back = ping(simulation, pc3, address_of(scenario, pc2));
    drain(simulation);
    CHECK(simulation.ping(back)->state == PingState::Succeeded);
    const auto table = simulation.mac_table(sw1);
    CHECK(std::any_of(table.begin(), table.end(), [&](const auto& entry) { return entry.mac == mac3 && entry.port == 4; }));
    const auto there = ping(simulation, pc2, address_of(scenario, pc3));
    drain(simulation);
    CHECK(simulation.ping(there)->state == PingState::Succeeded);
}

void power_cycles() {
    Simulation simulation(netsim::sample_lan());
    ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);  // 80 ms
    CHECK(simulation.arp_cache(pc_a).size() == 1 && simulation.arp_cache(pc_b).size() == 1);
    CHECK(simulation.mac_table(sw).size() == 2);

    // PC-B goes off while an Echo Request travels to it: the frames that
    // reach it are dropped.
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"), {.count = 3});
    simulation.run_until(95ms);
    CHECK(simulation.apply(netsim::SetPowered{pc_b, false}) == Problem::None);
    CHECK(simulation.arp_cache(pc_b).empty());
    simulation.run_until(1500ms);
    CHECK(count(simulation, TraceKind::FrameDropped, pc_b, Reason::DeviceDown) == 2);  // at 100 and 1100 ms

    // PC-A goes off: its ping ends at once. The request it was waiting for
    // is lost with it and the third is never sent; its timers are gone.
    CHECK(simulation.apply(netsim::SetPowered{pc_a, false}) == Problem::None);
    const auto& status = *simulation.ping(id);
    CHECK(status.state == PingState::Failed && status.reason == Reason::EchoTimeout && status.finished == 1500ms);
    CHECK(status.echoes.size() == 2 && status.echoes[0].reason == Reason::EchoTimeout);
    CHECK(status.echoes[1].state == EchoState::Lost && status.echoes[1].reason == Reason::DeviceDown);
    CHECK(simulation.arp_cache(pc_a).empty() && !simulation.pending());
    CHECK(simulation.mac_table(sw).size() == 2);  // the switch stayed on
    Problem problem = Problem::None;
    CHECK(!simulation.start_ping(pc_a, ip("192.168.1.20"), problem) && problem == Problem::SourcePoweredOff);

    // Back on, both start empty and resolve again.
    CHECK(simulation.apply(netsim::SetPowered{pc_a, true}) == Problem::None);
    CHECK(simulation.apply(netsim::SetPowered{pc_b, true}) == Problem::None);
    const auto after = ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.ping(after)->state == PingState::Succeeded && arp_requests(simulation, pc_a) == 2);
    // A switch forgets its table too.
    CHECK(simulation.apply(netsim::SetPowered{sw, false}) == Problem::None);
    CHECK(simulation.apply(netsim::SetPowered{sw, true}) == Problem::None);
    CHECK(simulation.mac_table(sw).empty());
}

void removal() {
    // The switch removed while a frame is on one of its cables: both cables
    // go, the frame is lost, and the switch is still known by name.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(15ms);
    CHECK(simulation.apply(netsim::RemoveDevice{sw}) == Problem::None);
    CHECK(!simulation.scenario().device(sw) && simulation.scenario().links.empty());
    const auto* removed = simulation.known_device(sw);
    CHECK(removed && removed->name == "Switch-01" && !simulation.known_device(DeviceId{9}));
    CHECK(simulation.edits().back().links.size() == 2);
    CHECK(simulation.mac_table(sw).empty() && simulation.arp_cache(sw).empty());
    CHECK(simulation.apply(netsim::SetPowered{sw, true}) == Problem::NoSuchDevice);
    drain(simulation);
    CHECK(count(simulation, TraceKind::FrameDropped, pc_b, Reason::LinkDown) == 1);
    CHECK(simulation.ping(id)->reason == Reason::ArpUnresolved);
    CHECK(count(simulation, TraceKind::FrameDropped, pc_a, Reason::NoCarrier) == 2);

    // Ids are never given out twice: the scenario alone would hand out 3
    // and cable 1 again.
    CHECK(simulation.apply(netsim::AddDevice{DeviceKind::Switch, 0, 0}) == Problem::None);
    const auto added = simulation.scenario().devices.back().id;
    CHECK(added == DeviceId{4} && simulation.scenario().devices.back().name == "Switch-4");
    CHECK(simulation.known_device(sw)->name == "Switch-01");
    CHECK(simulation.apply(netsim::Connect{PortRef{pc_a, 0}, PortRef{added, 0}}) == Problem::None);
    CHECK(simulation.scenario().links.back().id == LinkId{3});
    CHECK(simulation.apply(netsim::Connect{PortRef{pc_b, 0}, PortRef{added, 1}}) == Problem::None);
    const auto again = ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.ping(again)->state == PingState::Succeeded);

    // A PC removed while it pings: its ping ends.
    const auto running = ping(simulation, pc_a, ip("192.168.1.20"), {.count = 5});
    simulation.run_until(simulation.now() + 1500ms);
    CHECK(simulation.apply(netsim::RemoveDevice{pc_a}) == Problem::None);
    const auto& status = *simulation.ping(running);
    CHECK(status.state == PingState::Succeeded && status.echoes.size() == 2 && status.replies() == 2);
    CHECK(!simulation.pending());
}

void restored_device() {
    // Undoing a removal while frames fly: the switch comes back with its
    // cables, at the same place in the lists, but with empty tables, and
    // the frames that were on its cables stay lost.
    Simulation simulation(netsim::sample_lan());
    const auto first = simulation.scenario();
    ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(25ms);
    CHECK(!simulation.mac_table(sw).empty());
    const auto back = edit::inverse(simulation.scenario(), netsim::RemoveDevice{sw});
    CHECK(back.has_value());
    CHECK(simulation.apply(netsim::RemoveDevice{sw}) == Problem::None);
    simulation.run_until(26ms);
    CHECK(simulation.apply(*back) == Problem::None);
    CHECK(simulation.scenario() == first);
    CHECK(simulation.mac_table(sw).empty());
    CHECK(simulation.edits().back().device->name == "Switch-01" && simulation.edits().back().links.size() == 2);
    drain(simulation);
    CHECK(count(simulation, TraceKind::FrameDropped, {}, Reason::LinkDown) == 1);

    // Unplugged and plugged back: the frame on the cable is lost all the same.
    const auto unplug = edit::inverse(simulation.scenario(), netsim::Disconnect{LinkId{2}});
    const auto later = ping(simulation, pc_a, ip("192.168.1.20"), {.count = 2});
    simulation.run_until(simulation.now() + 15ms);
    CHECK(simulation.frames_in_flight() == 1);
    CHECK(simulation.apply(netsim::Disconnect{LinkId{2}}) == Problem::None);
    CHECK(simulation.apply(*unplug) == Problem::None);
    CHECK(simulation.scenario() == first);
    drain(simulation);
    CHECK(count(simulation, TraceKind::FrameDropped, {}, Reason::LinkDown) == 2);
    // The first request is lost on the cable; the second one gets through.
    CHECK(simulation.ping(later)->state == PingState::Succeeded && simulation.ping(later)->replies() == 1);

    netsim::explain::Explainer explainer;
    explainer.update(simulation);
    std::vector<std::string> edits;
    for (const auto& event : explainer.events())
        if (event.protocol == "EDIT") edits.push_back(event.summary);
    CHECK(edits.size() == 4);
    CHECK(edits.size() == 4 && edits[1] == "Switch-01 restored" && edits[3] == "Cable plugged back · PC-B port 1 — Switch-01 port 2");
}

void silent_edits() {
    // Names and positions do not change how the network behaves: a running
    // simulation takes them without a record.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(15ms);
    const auto records = simulation.trace().records().size();
    CHECK(simulation.apply(netsim::Rename{pc_a, "Laptop"}) == Problem::None);
    CHECK(simulation.apply(netsim::MoveDevice{pc_a, 7, 8}) == Problem::None);
    CHECK(simulation.scenario().device(pc_a)->name == "Laptop" && simulation.scenario().device(pc_a)->x == 7);
    // Nor does a refused edit, which changes nothing at all.
    const auto before = simulation.scenario();
    CHECK(simulation.apply(netsim::Rename{pc_a, ""}) == Problem::InvalidName);
    CHECK(simulation.apply(netsim::Connect{PortRef{pc_a, 0}, PortRef{sw, 5}}) == Problem::PortInUse);
    CHECK(simulation.apply(netsim::SetAddress{PortRef{sw, 0}, prefix("10.0.0.1/8")}) == Problem::AddressOnSwitch);
    CHECK(simulation.scenario() == before);
    CHECK(simulation.trace().records().size() == records && simulation.edits().empty());
    CHECK(simulation.frames_in_flight() == 1);
    drain(simulation);
    CHECK(simulation.ping(id)->finished == 80ms);
}

void incomplete_entries() {
    // While unresolved, the entry is incomplete and holds the waiting
    // datagrams, three at most: newer ones push out the oldest. One ARP
    // request a second serves them all.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.99"), {.count = 5, .interval = 200ms});
    simulation.run_until(900ms);
    const auto cache = simulation.arp_cache(pc_a);
    CHECK(cache.size() == 1 && cache[0].ip == ip("192.168.1.99") && cache[0].state == ArpState::Incomplete);
    CHECK(cache[0].mac == netsim::MacAddress{} && cache[0].queued == netsim::arp_queue_limit);
    CHECK(count(simulation, TraceKind::PingLost, pc_a, Reason::ArpQueueFull) == 2);
    drain(simulation);
    CHECK(arp_requests(simulation, pc_a) == 3 && simulation.now() == 3s);
    const auto& status = *simulation.ping(id);
    CHECK(status.state == PingState::Failed && status.reason == Reason::ArpQueueFull && status.echoes.size() == 5);
    CHECK(status.echoes[0].reason == Reason::ArpQueueFull && status.echoes[0].finished == 600ms);
    CHECK(status.echoes[1].reason == Reason::ArpQueueFull && status.echoes[1].finished == 800ms);
    for (std::size_t i = 2; i < 5; ++i) CHECK(status.echoes[i].reason == Reason::ArpUnresolved && status.echoes[i].finished == 3s);
    CHECK(simulation.arp_cache(pc_a).empty());
}

void entries_expire() {
    // A reachable entry lasts a minute from its last update; the next
    // datagram after that finds it expired and asks again.
    Simulation simulation(netsim::sample_lan());
    ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    auto cache = simulation.arp_cache(pc_a);
    CHECK(cache.size() == 1 && cache[0].state == ArpState::Reachable && cache[0].expires == 40ms + 60s);
    CHECK(cache[0].mac == simulation.scenario().device(pc_b)->ports[0].mac && cache[0].queued == 0);
    CHECK(simulation.arp_cache(pc_b).at(0).expires == 20ms + 60s);  // learned from the request
    simulation.run_until(40ms + 60s - 1us);
    CHECK(simulation.arp_cache(pc_a).size() == 1);
    simulation.run_until(40ms + 60s);
    CHECK(simulation.arp_cache(pc_a).empty());
    const auto later = ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.ping(later)->state == PingState::Succeeded && arp_requests(simulation, pc_a) == 2);
    const auto expired = records_of(simulation, TraceKind::ArpEntryExpired);
    CHECK(expired.size() == 1 && expired[0].device == pc_a && expired[0].value == ip("192.168.1.20").bits());
}

void merge_rule() {
    // RFC 826: an ARP packet updates an entry the receiver already has, and
    // creates one only when it asks for the receiver.
    const auto scenario = star(3);  // PC-2 .11, PC-3 .12, PC-4 .13
    const DeviceId pc2{2}, pc3{3}, pc4{4};
    Simulation simulation(scenario);
    ping(simulation, pc2, ip("192.168.1.12"));
    drain(simulation);
    CHECK(simulation.arp_cache(pc4).empty());  // it heard the request, not for it
    CHECK(simulation.arp_cache(pc3).size() == 1);
    ping(simulation, pc4, ip("192.168.1.11"));
    drain(simulation);
    CHECK(simulation.arp_cache(pc4).size() == 1);
    // PC-2 asks for an address nobody has: PC-3 and PC-4 refresh their entry for it.
    const auto start = simulation.now();
    ping(simulation, pc2, ip("192.168.1.99"));
    simulation.run_until(start + 20ms);
    CHECK(simulation.arp_cache(pc3).at(0).expires == start + 20ms + 60s);
    CHECK(simulation.arp_cache(pc4).at(0).expires == start + 20ms + 60s);
    drain(simulation);
}

void stale_entry_after_address_change() {
    // PC-2 knows 192.168.1.12 as PC-3. PC-3 changes to .14 and PC-4 takes
    // .12: PC-2 keeps sending to PC-3, which ignores the datagrams, until
    // its entry expires and ARP finds the new owner.
    const auto scenario = star(3);
    const DeviceId pc2{2}, pc3{3}, pc4{4};
    Simulation simulation(scenario);
    ping(simulation, pc2, ip("192.168.1.12"));
    drain(simulation);
    CHECK(simulation.apply(netsim::SetAddress{PortRef{pc3, 0}, prefix("192.168.1.14/24")}) == Problem::None);
    CHECK(simulation.apply(netsim::SetAddress{PortRef{pc4, 0}, prefix("192.168.1.12/24")}) == Problem::None);
    const auto heard = count(simulation, TraceKind::FrameReceived, pc4);
    const auto stale = ping(simulation, pc2, ip("192.168.1.12"));
    drain(simulation);
    CHECK(simulation.ping(stale)->reason == Reason::EchoTimeout);
    CHECK(count(simulation, TraceKind::FrameDropped, pc3, Reason::Ipv4NotForUs) == 1);
    CHECK(count(simulation, TraceKind::FrameReceived, pc4) == heard);

    simulation.run_until(40ms + 60s);
    const auto fresh = ping(simulation, pc2, ip("192.168.1.12"));
    drain(simulation);
    CHECK(simulation.ping(fresh)->state == PingState::Succeeded);
    CHECK(count(simulation, TraceKind::ArpEntryExpired, pc2) == 1);
    CHECK(simulation.arp_cache(pc2).at(0).mac == scenario.device(pc4)->ports[0].mac);
}

void mac_ageing() {
    auto scenario = star(2);
    scenario.devices[0].mac_ageing = 10s;
    const DeviceId sw1{1}, pc2{2}, pc3{3};
    const auto mac2 = scenario.device(pc2)->ports[0].mac;
    const auto mac3 = scenario.device(pc3)->ports[0].mac;
    Simulation simulation(scenario);
    ping(simulation, pc2, address_of(scenario, pc3));
    drain(simulation);
    // Each entry remembers when a frame from its address last arrived.
    auto table = simulation.mac_table(sw1);
    CHECK(table.size() == 2);
    CHECK(table[0].mac == mac2 && table[0].port == 0 && table[0].updated == 50ms);
    CHECK(table[1].mac == mac3 && table[1].port == 1 && table[1].updated == 70ms);
    simulation.run_until(10s + 50ms - 1us);
    CHECK(simulation.mac_table(sw1).size() == 2);
    simulation.run_until(10s + 50ms);
    table = simulation.mac_table(sw1);
    CHECK(table.size() == 1 && table[0].mac == mac3);
    simulation.run_until(10s + 70ms);
    CHECK(simulation.mac_table(sw1).empty());

    // The next frame for PC-3 finds its entry aged out and is flooded; both
    // addresses are learned again.
    const auto flooded = count(simulation, TraceKind::FrameFlooded, sw1);
    const auto id = ping(simulation, pc2, address_of(scenario, pc3));
    drain(simulation);
    CHECK(simulation.ping(id)->state == PingState::Succeeded);
    CHECK(count(simulation, TraceKind::MacEntryAged, sw1) == 1);
    CHECK(count(simulation, TraceKind::FrameFlooded, sw1) == flooded + 1);
    CHECK(count(simulation, TraceKind::MacLearned, sw1) == 4);

    // The ageing time changes while the network runs.
    CHECK(simulation.apply(netsim::SetAgeing{sw1, 300s}) == Problem::None);
    simulation.run_until(simulation.now() + 100s);
    CHECK(simulation.mac_table(sw1).size() == 2);
    CHECK(simulation.apply(netsim::SetAgeing{pc2, 300s}) == Problem::NotASwitch);
    CHECK(simulation.apply(netsim::SetAgeing{sw1, 1s}) == Problem::InvalidAgeing);
}

void replies_resolve_the_sender() {
    // PC-B, just restarted, has no entry for PC-A: it asks before answering.
    Simulation simulation(netsim::sample_lan());
    ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.apply(netsim::SetPowered{pc_b, false}) == Problem::None);
    CHECK(simulation.apply(netsim::SetPowered{pc_b, true}) == Problem::None);
    const auto start = simulation.now();
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"));
    drain(simulation);
    CHECK(simulation.ping(id)->state == PingState::Succeeded && simulation.ping(id)->finished == start + 80ms);
    CHECK(arp_requests(simulation, pc_b) == 1);
    // Its ARP request leaves before the reply that waited for it.
    std::vector<netsim::PduKind> sent;
    for (const auto& record : simulation.trace().records())
        if (record.device == pc_b && record.kind == TraceKind::FrameSent && record.time > start)
            sent.push_back(netsim::kind_of(simulation.trace().pdu(*record.pdu)));
    CHECK((sent == std::vector{netsim::PduKind::ArpRequest, netsim::PduKind::EchoReply}));

    // When the sender cannot be resolved, the reply is dropped after three
    // attempts.
    CHECK(simulation.apply(netsim::SetPowered{pc_b, false}) == Problem::None);
    CHECK(simulation.apply(netsim::SetPowered{pc_b, true}) == Problem::None);
    const auto again = simulation.now();
    const auto lost = ping(simulation, pc_a, ip("192.168.1.20"));
    simulation.run_until(again + 25ms);  // PC-B's ARP request is on its way
    CHECK(simulation.apply(netsim::SetLinkUp{LinkId{1}, false}) == Problem::None);
    drain(simulation);
    CHECK(simulation.ping(lost)->reason == Reason::EchoTimeout);
    const auto dropped = records_of(simulation, TraceKind::DatagramDropped);
    CHECK(dropped.size() == 1 && dropped[0].device == pc_b && dropped[0].reason == Reason::ArpUnresolved);
    CHECK(dropped[0].value == ip("192.168.1.10").bits() && dropped[0].time == again + 20ms + 3s);
}

void ping_application() {
    Simulation simulation(netsim::sample_lan());
    const auto refused = [&](DeviceId source, netsim::PingOptions options) {
        Problem problem = Problem::None;
        CHECK(!simulation.start_ping(source, ip("192.168.1.20"), problem, options));
        return problem;
    };
    for (const auto options : {netsim::PingOptions{.count = 0}, netsim::PingOptions{.count = 1001},
                               netsim::PingOptions{.interval = 9ms}, netsim::PingOptions{.interval = 61s},
                               netsim::PingOptions{.timeout = 9ms}, netsim::PingOptions{.timeout = 61s}})
        CHECK(refused(pc_a, options) == Problem::InvalidPingOptions);
    CHECK(refused(sw, {.count = 0}) == Problem::SourceNotHost);  // the source first
    CHECK(simulation.pings().empty());

    const auto id = ping(simulation, pc_a, ip("192.168.1.20"), {.count = 3, .interval = 500ms});
    drain(simulation);
    const auto& status = *simulation.ping(id);
    CHECK(status.state == PingState::Succeeded && status.replies() == 3 && status.finished == 1040ms);
    const netsim::SimTime sent[] = {0ms, 500ms, 1000ms}, finished[] = {80ms, 540ms, 1040ms};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& echo = status.echoes[i];
        CHECK(echo.sequence == i + 1 && echo.state == EchoState::Replied && echo.reason == Reason::None);
        CHECK(echo.sent == sent[i] && echo.finished == finished[i]);
        CHECK(simulation.trace().records().at(echo.record).kind == TraceKind::PingReply);
    }
    CHECK(count(simulation, TraceKind::PingStarted) == 1 && count(simulation, TraceKind::PingReply) == 3);
    CHECK(count(simulation, TraceKind::PingSucceeded) == 1);
    // On the wire: the ping's id as identifier, sequence numbers from 1,
    // 56 data bytes and TTL 64, both ways.
    std::vector<std::uint16_t> requests, replies;
    for (const auto& frame : simulation.trace().pdus()) {
        const auto* packet = frame.ipv4();
        if (!packet) continue;
        CHECK(packet->ttl == 64 && packet->icmp.data_size == 56 && packet->icmp.identifier == id.value);
        (packet->icmp.type == netsim::icmp::echo_request ? requests : replies).push_back(packet->icmp.sequence);
    }
    CHECK((requests == std::vector<std::uint16_t>{1, 2, 3} && replies == requests));

    // Its own address: every request answered at once.
    const auto start = simulation.now();
    const auto local = ping(simulation, pc_a, ip("192.168.1.10"), {.count = 2, .interval = 10ms});
    drain(simulation);
    const auto& self = *simulation.ping(local);
    CHECK(self.state == PingState::Succeeded && self.replies() == 2 && self.finished == start + 10ms);
    CHECK(self.echoes[1].sent == start + 10ms && self.echoes[1].finished == start + 10ms);
}

void timeouts_and_late_replies() {
    // A timeout shorter than the round trip: the reply comes too late and
    // is ignored, with a record that says so.
    Simulation simulation(netsim::sample_lan());
    const auto id = ping(simulation, pc_a, ip("192.168.1.20"), {.timeout = 10ms});
    drain(simulation);
    const auto& status = *simulation.ping(id);
    // The wait starts when the request leaves, after ARP.
    CHECK(status.reason == Reason::EchoTimeout && status.echoes[0].finished == 50ms);
    CHECK(count(simulation, TraceKind::FrameDropped, pc_a, Reason::UnmatchedReply) == 1);

    // An interval shorter than the round trip: the requests overlap and
    // each finds its own reply.
    const auto start = simulation.now();
    const auto burst = ping(simulation, pc_a, ip("192.168.1.20"), {.count = 3, .interval = 10ms});
    drain(simulation);
    const auto& overlap = *simulation.ping(burst);
    CHECK(overlap.replies() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(overlap.echoes[i].sent == start + static_cast<int>(i) * 10ms);
        CHECK(overlap.echoes[i].finished == overlap.echoes[i].sent + 40ms);
    }
    // Requests queued behind ARP leave together and count from when ping sent them.
    Simulation queued(netsim::sample_lan());
    const auto waiting = ping(queued, pc_a, ip("192.168.1.20"), {.count = 3, .interval = 10ms});
    drain(queued);
    const auto& together = *queued.ping(waiting);
    CHECK(together.echoes[0].finished == 80ms && together.echoes[1].finished == 80ms && together.echoes[2].finished == 80ms);
    CHECK(together.echoes[2].sent == 20ms);
}

Scenario triangle() {
    // Three switches in a ring, a PC on the first and one on the second.
    Scenario scenario;
    for (int i = 0; i < 3; ++i) CHECK(edit::add_device(scenario, DeviceKind::Switch, 0, 0) == Problem::None);
    CHECK(edit::connect(scenario, DeviceId{1}, DeviceId{2}) == Problem::None);
    CHECK(edit::connect(scenario, DeviceId{2}, DeviceId{3}) == Problem::None);
    CHECK(edit::connect(scenario, DeviceId{3}, DeviceId{1}) == Problem::None);
    for (const auto switch_id : {DeviceId{1}, DeviceId{2}}) {
        DeviceId host{};
        CHECK(edit::add_device(scenario, DeviceKind::Host, 0, 0, &host) == Problem::None);
        CHECK(edit::connect(scenario, host, switch_id) == Problem::None);
    }
    CHECK(netsim::validate(scenario).empty());
    return scenario;
}

void storm_guard() {
    // Every broadcast goes round the ring both ways for ever. The guard
    // stops each one at the limit and removes its copies from the cables.
    const auto scenario = triangle();
    const DeviceId a{4}, b{5};
    Simulation simulation(scenario);
    CHECK(simulation.storm_limit() == 64);
    const auto id = ping(simulation, a, address_of(scenario, b));
    drain(simulation);
    CHECK(simulation.frames_in_flight() == 0);
    const auto guards = records_of(simulation, TraceKind::SimulatorGuard);
    CHECK(guards.size() == 3);  // one per ARP attempt
    const auto& trace = simulation.trace();
    for (const auto& guard : guards) {
        CHECK(guard.value == 64 && netsim::kind_of(trace.pdu(*guard.pdu)) == netsim::PduKind::ArpRequest);
        int sent = 0, stopped = 0;
        for (const auto& record : trace.records())
            if (record.kind == TraceKind::FrameSent && record.pdu == guard.pdu) {
                ++sent;
                CHECK(record.seq < guard.seq);  // nothing after the guard
            }
        for (const auto& item : trace.transmissions())
            if (item.pdu == *guard.pdu && item.stopped) {
                ++stopped;
                CHECK(item.arrival == guard.time && item.start <= guard.time);
            }
        CHECK(sent == 64 && stopped > 0);
    }
    // MAC tables flap while the copies circle: no ARP reply gets through.
    CHECK(simulation.ping(id)->reason == Reason::ArpUnresolved);
    // The explanation keeps the guard apart from network behaviour.
    netsim::explain::Explainer explainer;
    explainer.update(simulation);
    const auto& events = explainer.events();
    CHECK(std::count_if(events.begin(), events.end(), [](const auto& event) {
              return event.simulator && event.protocol == "SIMULATOR";
          }) == 3);

    // Without the loop the same network works, and the guard never trips.
    auto open = scenario;
    CHECK(edit::remove_link(open, LinkId{3}) == Problem::None);
    Simulation fine(open);
    const auto works = ping(fine, a, address_of(open, b));
    drain(fine);
    CHECK(fine.ping(works)->state == PingState::Succeeded && count(fine, TraceKind::SimulatorGuard) == 0);
    // Large networks get a higher limit: four copies per cable.
    Scenario chain;
    for (std::uint32_t i = 1; i <= 18; ++i) {
        CHECK(edit::add_device(chain, DeviceKind::Switch, 0, 0) == Problem::None);
        if (i > 1) CHECK(edit::connect(chain, DeviceId{i - 1}, DeviceId{i}) == Problem::None);
    }
    CHECK(Simulation(chain).storm_limit() == 4 * 17);
}

// The randomised check of conservation: frames sent = frames delivered or
// dropped on arrival + frames the guard stopped + frames in flight.
struct Ledger {
    std::size_t seen = 0, arrived = 0, guards = 0, stopped = 0;
    void update(const Simulation& simulation) {
        const auto& records = simulation.trace().records();
        bool guarded = false;
        for (; seen < records.size(); ++seen) {
            const auto& record = records[seen];
            if (record.kind == TraceKind::FrameReceived ||
                (record.kind == TraceKind::FrameDropped && (record.reason == Reason::LinkDown || record.reason == Reason::DeviceDown)))
                ++arrived;
            if (record.kind == TraceKind::SimulatorGuard) guarded = true, ++guards;
        }
        if (guarded) {
            const auto& sent = simulation.trace().transmissions();
            stopped = static_cast<std::size_t>(std::count_if(sent.begin(), sent.end(), [](const auto& item) { return item.stopped; }));
        }
    }
    void check(const Simulation& simulation) {
        update(simulation);
        CHECK(simulation.trace().transmissions().size() == arrived + stopped + simulation.frames_in_flight());
    }
};

struct LiveTally {
    int applied = 0, refused = 0, guards = 0, lost_on_cable = 0, pings = 0, replies = 0, echoes = 0;
};

void random_live_run(std::uint64_t seed, Hash& hash, LiveTally& tally) {
    netsim::Rng rng(seed);
    Scenario scenario;
    const auto devices = rng.between(2, 10);
    for (std::int64_t i = 0; i < devices; ++i) {
        DeviceId id{};
        CHECK(edit::add_device(scenario, rng.chance(2, 5) ? DeviceKind::Switch : DeviceKind::Host, 0, 0, &id) == Problem::None);
        if (i > 0 && rng.chance(4, 5)) edit::connect(scenario, id, DeviceId{static_cast<std::uint32_t>(rng.between(1, i))});
    }
    // A few more cables: loops are allowed. Draws go into variables, as the
    // order in which function arguments are evaluated differs between compilers.
    for (auto extra = rng.between(0, 3); extra > 0; --extra) {
        const DeviceId a{static_cast<std::uint32_t>(rng.between(1, devices))};
        const DeviceId b{static_cast<std::uint32_t>(rng.between(1, devices))};
        edit::connect(scenario, a, b);
    }
    for (auto& cable : scenario.links) cable.delay = delays[rng.below(std::size(delays))];
    CHECK(netsim::validate(scenario).empty());

    Simulation simulation(scenario);
    Ledger ledger;
    const auto start_ping = [&] {
        const auto& devices_now = simulation.scenario().devices;
        if (devices_now.empty()) return;
        const auto source = devices_now[rng.below(devices_now.size())].id;
        const auto destination = Ipv4Address(0xc0a80100U | static_cast<std::uint32_t>(rng.between(9, 20)));
        const netsim::PingOptions options{.count = static_cast<std::uint16_t>(rng.between(1, 4)),
                                          .interval = std::chrono::milliseconds(rng.between(10, 1000)),
                                          .timeout = std::chrono::milliseconds(rng.between(10, 1000))};
        Problem problem = Problem::None;
        simulation.start_ping(source, destination, problem, options);
        hash.add(static_cast<std::uint64_t>(problem));
    };
    for (auto pings = rng.between(1, 3); pings > 0; --pings) start_ping();

    for (int action = 0; action < 300; ++action) {
        const auto roll = rng.below(20);
        if (roll == 0) {
            start_ping();
        } else if (roll <= 2) {
            const auto change = random_edit(rng, simulation.scenario());
            const auto records = simulation.trace().records().size();
            const auto before = simulation.scenario();
            const auto problem = simulation.apply(change);
            hash.add(static_cast<std::uint64_t>(problem));
            if (ok(problem)) {
                ++tally.applied;
            } else {
                ++tally.refused;
                CHECK(simulation.scenario() == before && simulation.trace().records().size() == records);
            }
        } else if (!simulation.step()) {
            simulation.run_until(simulation.now() + std::chrono::milliseconds(rng.between(1, 300)));
        }
        ledger.check(simulation);
    }
    for (int steps = 0; simulation.step(); ++steps) {
        ledger.check(simulation);
        CHECK(steps < 200'000);
    }
    CHECK(simulation.frames_in_flight() == 0);
    ledger.check(simulation);

    // Every ping ended, and every request it sent has a fate.
    for (const auto& status : simulation.pings()) {
        CHECK(status.state != PingState::Running);
        CHECK(status.echoes.size() <= status.options.count);
        CHECK((status.state == PingState::Succeeded) == (status.replies() > 0));
        for (const auto& echo : status.echoes) CHECK(echo.state != EchoState::Waiting && echo.finished >= echo.sent);
        ++tally.pings;
        tally.replies += status.replies();
        tally.echoes += static_cast<int>(status.echoes.size());
    }
    // Time and episodes never go back; frames stopped by the guard are the
    // guard's.
    const auto& records = simulation.trace().records();
    for (std::size_t i = 1; i < records.size(); ++i)
        CHECK(records[i].time >= records[i - 1].time && records[i].episode >= records[i - 1].episode);
    for (const auto& item : simulation.trace().transmissions())
        if (item.stopped)
            CHECK(std::any_of(records.begin(), records.end(), [&](const auto& record) {
                return record.kind == TraceKind::SimulatorGuard && record.pdu == item.pdu;
            }));
    tally.guards += static_cast<int>(ledger.guards);
    tally.lost_on_cable += count(simulation, TraceKind::FrameDropped, std::nullopt, Reason::LinkDown);

    // The explanation covers whatever happened.
    netsim::explain::Explainer explainer;
    explainer.update(simulation);
    CHECK(!explainer.events().empty() || records.empty());

    add_trace(hash, simulation);
    hash.add(simulation.edits().size());
}

void random_live_undo() {
    // Random edits on running networks, each with its inverse taken first,
    // then undone in reverse order while the frames keep flying: the network
    // comes back exactly, and no frame is ever counted twice or forgotten.
    int undone = 0;
    for (std::uint64_t seed = 1; seed <= 100; ++seed) {
        netsim::Rng rng(seed);
        Simulation simulation(star(static_cast<int>(rng.between(2, 5))));
        const auto first = simulation.scenario();
        Ledger ledger;
        std::vector<netsim::Edit> undo;
        for (int count = 0; count < 50; ++count) {
            const auto destination = Ipv4Address(0xc0a80100U | static_cast<std::uint32_t>(rng.between(9, 14)));
            if (rng.chance(1, 4) && !simulation.scenario().devices.empty()) {
                Problem problem = Problem::None;
                simulation.start_ping(simulation.scenario().devices.back().id, destination, problem);
            }
            const auto change = simulation.resolve(random_edit(rng, simulation.scenario()));
            const auto back = edit::inverse(simulation.scenario(), change);
            if (ok(simulation.apply(change))) {
                CHECK(back.has_value());
                if (back) undo.push_back(*back);
            }
            simulation.run_until(simulation.now() + std::chrono::milliseconds(rng.between(0, 20)));
            ledger.check(simulation);
        }
        while (!undo.empty()) {
            CHECK(simulation.apply(undo.back()) == Problem::None);
            undo.pop_back();
            ++undone;
            simulation.run_until(simulation.now() + std::chrono::milliseconds(rng.between(0, 20)));
            ledger.check(simulation);
        }
        CHECK(simulation.scenario() == first);
        drain(simulation);
        ledger.check(simulation);
    }
    CHECK(undone > 1000);
}

void random_live_networks() {
    // 300 random networks, loops included, edited while they run. Hashed
    // like the loop-free runs: the constant changes only with the model.
    constexpr std::uint64_t expected = 0x1015f582ba154f07;
    Hash hash;
    LiveTally tally;
    for (std::uint64_t seed = 1; seed <= 300; ++seed) random_live_run(seed, hash, tally);
    std::cout << "  300 live networks: " << tally.applied << " edits applied, " << tally.refused << " refused, " << tally.pings
              << " pings with " << tally.echoes << " requests and " << tally.replies << " replies, " << tally.lost_on_cable
              << " frames lost on a cable, " << tally.guards << " storms stopped; hash 0x" << std::hex << hash.value << std::dec
              << '\n';
    // The generator reaches every situation the checks are about.
    CHECK(tally.applied > 100 && tally.refused > 0 && tally.replies > 0 && tally.lost_on_cable > 0 && tally.guards > 0);
    CHECK(hash.value == expected);
}
} // namespace

int main() {
    cable_down_in_flight();
    delays_and_ports();
    unplug_and_plug();
    moved_cable();
    power_cycles();
    removal();
    restored_device();
    silent_edits();
    incomplete_entries();
    entries_expire();
    merge_rule();
    stale_entry_after_address_change();
    mac_ageing();
    replies_resolve_the_sender();
    ping_application();
    timeouts_and_late_replies();
    storm_guard();
    random_live_undo();
    random_live_networks();
    std::cout << "Live network tests passed: " << test::checks << " checks.\n";
}

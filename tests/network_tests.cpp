#include "check.hpp"
#include "network.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>

namespace {
using namespace std::chrono_literals;
using netsim::PduKind;
using netsim::Reason;
using netsim::TraceKind;

void drain(netsim::Engine& engine) {
    int count = 0;
    while (engine.pending()) {
        CHECK(engine.step());
        CHECK(++count < 2000);
    }
    CHECK(!engine.step());
}
bool contains(const netsim::Engine& engine, const std::string& text) {
    return std::any_of(engine.events().begin(), engine.events().end(), [&](const auto& event) {
        return event.summary.find(text) != std::string::npos || event.detail.find(text) != std::string::npos;
    });
}
// Counts trace records of one kind, optionally at one device and for one reason.
struct Match {
    TraceKind kind;
    std::optional<int> device{};
    std::optional<Reason> reason{};
};
int count(const netsim::Engine& engine, Match match) {
    const auto& records = engine.trace().records();
    return static_cast<int>(std::count_if(records.begin(), records.end(), [&](const auto& record) {
        return record.kind == match.kind && (!match.device || record.device.value == static_cast<std::uint32_t>(*match.device)) &&
            (!match.reason || record.reason == *match.reason);
    }));
}
int frame_count(const netsim::Engine& engine, int from, int to, const std::string& type) {
    return static_cast<int>(std::count_if(engine.events().begin(), engine.events().end(), [&](const auto& event) {
        return event.from == from && event.to == to && event.detail.find(type) != std::string::npos;
    }));
}
void basic_ping_and_cache() {
    netsim::Engine engine;
    engine.seed_demo();
    CHECK(engine.start_ping(1, "192.168.1.20"));
    CHECK(!engine.start_ping(1, "192.168.1.20"));
    drain(engine);
    CHECK(engine.ping_succeeded());
    CHECK(engine.now() == 80ms);

    // PC-A (1) port 1 —link 1— port 1 Switch-01 (3) port 2 —link 2— port 1 PC-B (2),
    // 10 ms per hop. Ports and PDUs are zero-based.
    struct Step {
        netsim::SimTime time;
        TraceKind kind;
        std::uint32_t device;
        int port, link, pdu;
    };
    const Step expected[] = {
        {0ms, TraceKind::FrameSent, 1, 0, 1, 0},  // ARP Request, broadcast
        {10ms, TraceKind::FrameReceived, 3, 0, 1, 0},
        {10ms, TraceKind::MacLearned, 3, 0, 1, 0},
        {10ms, TraceKind::FrameFlooded, 3, 0, 1, 0},
        {10ms, TraceKind::FrameSent, 3, 1, 2, 0},
        {20ms, TraceKind::FrameReceived, 2, 0, 2, 0},
        {20ms, TraceKind::ArpCacheUpdated, 2, 0, 2, 0},
        {20ms, TraceKind::FrameSent, 2, 0, 2, 1},  // ARP Reply, unicast
        {30ms, TraceKind::FrameReceived, 3, 1, 2, 1},
        {30ms, TraceKind::MacLearned, 3, 1, 2, 1},
        {30ms, TraceKind::FrameForwarded, 3, 1, 2, 1},
        {30ms, TraceKind::FrameSent, 3, 0, 1, 1},
        {40ms, TraceKind::FrameReceived, 1, 0, 1, 1},
        {40ms, TraceKind::ArpCacheUpdated, 1, 0, 1, 1},
        {40ms, TraceKind::FrameSent, 1, 0, 1, 2},  // Echo Request
        {50ms, TraceKind::FrameReceived, 3, 0, 1, 2},
        {50ms, TraceKind::FrameForwarded, 3, 0, 1, 2},
        {50ms, TraceKind::FrameSent, 3, 1, 2, 2},
        {60ms, TraceKind::FrameReceived, 2, 0, 2, 2},
        {60ms, TraceKind::FrameSent, 2, 0, 2, 3},  // Echo Reply
        {70ms, TraceKind::FrameReceived, 3, 1, 2, 3},
        {70ms, TraceKind::FrameForwarded, 3, 1, 2, 3},
        {70ms, TraceKind::FrameSent, 3, 0, 1, 3},
        {80ms, TraceKind::FrameReceived, 1, 0, 1, 3},
        {80ms, TraceKind::PingSucceeded, 1, 0, 1, 3},
    };
    const auto& trace = engine.trace();
    CHECK(trace.records().size() == std::size(expected));
    for (std::size_t i = 0; i < std::min(trace.records().size(), std::size(expected)); ++i) {
        const auto& record = trace.records()[i];
        const auto& step = expected[i];
        CHECK(record.seq == i);
        CHECK(record.time == step.time);
        CHECK(record.kind == step.kind);
        CHECK(record.reason == Reason::None);
        CHECK(record.device == netsim::DeviceId{step.device});
        CHECK(record.port == step.port);
        CHECK(record.link == netsim::LinkId{static_cast<std::uint32_t>(step.link)});
        CHECK(record.pdu == netsim::PduId{static_cast<std::uint32_t>(step.pdu)});
    }

    const auto a = *engine.device(1), b = *engine.device(2);
    CHECK(trace.pdus().size() == 4);
    CHECK((trace.pdus()[0] == netsim::Frame{PduKind::ArpRequest, a.mac, netsim::MacAddress::broadcast(), a.ip, b.ip, 0}));
    CHECK((trace.pdus()[1] == netsim::Frame{PduKind::ArpReply, b.mac, a.mac, b.ip, a.ip, 0}));
    const auto ping_id = trace.pdus()[2].ping_id;
    CHECK(ping_id != 0);
    CHECK((trace.pdus()[2] == netsim::Frame{PduKind::EchoRequest, a.mac, b.mac, a.ip, b.ip, ping_id}));
    CHECK((trace.pdus()[3] == netsim::Frame{PduKind::EchoReply, b.mac, a.mac, b.ip, a.ip, ping_id}));

    // What the tables hold is what the trace says was learned.
    CHECK(engine.arp_table(1).size() == 1);
    CHECK(engine.arp_table(2).size() == 1);
    CHECK(engine.arp_table(1)[0].ip == b.ip && engine.arp_table(1)[0].mac == b.mac);
    CHECK(engine.arp_table(2)[0].ip == a.ip && engine.arp_table(2)[0].mac == a.mac);
    auto table = engine.mac_table(3);
    CHECK(table.size() == 2);
    CHECK(table[0].mac == a.mac && table[0].port == 0);
    CHECK(table[1].mac == b.mac && table[1].port == 1);

    // Second ping: both caches hit, so only ICMP crosses the switch and
    // nothing new is learned. The timer of the first ping was cancelled.
    const auto previous = trace.records().size();
    CHECK(engine.start_ping(1, "192.168.1.20"));
    drain(engine);
    CHECK(engine.ping_succeeded());
    CHECK(engine.now() == 120ms);
    CHECK(trace.records().size() == previous + 11);
    for (std::size_t i = previous; i < trace.records().size(); ++i) {
        const auto& record = trace.records()[i];
        CHECK(record.kind != TraceKind::MacLearned && record.kind != TraceKind::ArpCacheUpdated);
        CHECK(record.pdu && !netsim::is_arp(trace.pdu(*record.pdu).kind));
    }
    CHECK(trace.records().back().kind == TraceKind::PingSucceeded);
    CHECK(trace.pdus().size() == 6);
    CHECK(trace.pdus()[4].ping_id != ping_id && trace.pdus()[4].ping_id == trace.pdus()[5].ping_id);
}
// Frames on the cables, as the animation reads them.
void transmissions_and_run_until() {
    netsim::Engine engine;
    engine.seed_demo();
    CHECK(engine.start_ping(1, "192.168.1.20"));
    CHECK(engine.next_event_time() == 0ms);
    // Nothing runs before its time, and every entry due at the time runs.
    engine.run_until(0ms);
    CHECK(engine.trace().transmissions().size() == 1);
    CHECK(engine.next_event_time() == 10ms);
    engine.run_until(9ms);
    CHECK(engine.now() == 9ms);
    CHECK(engine.trace().records().size() == 1);
    engine.run_until(10ms);
    CHECK(engine.trace().records().size() == 5);
    CHECK(engine.next_event_time() == 20ms);
    engine.run_until(1s);
    CHECK(!engine.pending() && !engine.next_event_time());
    CHECK(engine.now() == 1s);  // the clock moves on even with nothing to do
    CHECK(engine.ping_succeeded());

    // PC-A (1) port 0 —link 1— port 0 Switch (3) port 1 —link 2— port 0 PC-B (2).
    struct Hop { int pdu, link, from, from_port, to, to_port; netsim::SimTime start; };
    const Hop hops[] = {
        {0, 1, 1, 0, 3, 0, 0ms}, {0, 2, 3, 1, 2, 0, 10ms},   // ARP Request, flooded
        {1, 2, 2, 0, 3, 1, 20ms}, {1, 1, 3, 0, 1, 0, 30ms},  // ARP Reply
        {2, 1, 1, 0, 3, 0, 40ms}, {2, 2, 3, 1, 2, 0, 50ms},  // Echo Request
        {3, 2, 2, 0, 3, 1, 60ms}, {3, 1, 3, 0, 1, 0, 70ms},  // Echo Reply
    };
    const auto& sent = engine.trace().transmissions();
    CHECK(sent.size() == std::size(hops));
    for (std::size_t i = 0; i < std::min(sent.size(), std::size(hops)); ++i) {
        const auto& t = sent[i];
        const auto& hop = hops[i];
        CHECK(t.pdu == netsim::PduId{static_cast<std::uint32_t>(hop.pdu)});
        CHECK(t.link == netsim::LinkId{static_cast<std::uint32_t>(hop.link)});
        CHECK(t.from == netsim::DeviceId{static_cast<std::uint32_t>(hop.from)} && t.from_port == hop.from_port);
        CHECK(t.to == netsim::DeviceId{static_cast<std::uint32_t>(hop.to)} && t.to_port == hop.to_port);
        CHECK(t.start == hop.start && t.arrival == hop.start + 10ms);
        // Each transmission points at its FrameSent record.
        const auto& record = engine.trace().records().at(t.record);
        CHECK(record.kind == TraceKind::FrameSent && record.time == t.start && record.pdu == t.pdu && record.link == t.link);
    }

    // A ping started later begins at the current time, not at the last event.
    CHECK(engine.start_ping(1, "192.168.1.20"));
    engine.run_until(2s);
    CHECK(engine.trace().transmissions()[8].start == 1s);
    CHECK(engine.ping_succeeded());

    // A flood puts one copy on every other active cable at the same instant.
    engine.seed_demo();
    const int observer = engine.add_device(netsim::DeviceKind::Host, 0, 0);
    CHECK(engine.connect(observer, 3));
    CHECK(engine.start_ping(1, "192.168.1.20"));
    engine.run_until(10ms);
    const auto& copies = engine.trace().transmissions();
    CHECK(copies.size() == 3);
    CHECK(copies[1].start == 10ms && copies[2].start == 10ms);
    CHECK(copies[1].pdu == copies[0].pdu && copies[2].pdu == copies[0].pdu);
    CHECK(copies[1].to == netsim::DeviceId{2} && copies[2].to == netsim::DeviceId{static_cast<std::uint32_t>(observer)});
    // Events name the frame they are about.
    CHECK(engine.events().size() == 2 && engine.events()[0].pdu == 0 && engine.events()[1].pdu == 0);
}
void broadcast_and_unicast() {
    netsim::Engine engine;
    engine.seed_demo();
    const int observer = engine.add_device(netsim::DeviceKind::Host, 0, 0);
    CHECK(engine.connect(observer, 3));
    CHECK(engine.start_ping(1, "192.168.1.20"));
    drain(engine);
    CHECK(engine.ping_succeeded());
    CHECK(frame_count(engine, 3, observer, "ARP Request") == 1);
    CHECK(frame_count(engine, 3, 1, "ARP Request") == 0);
    CHECK(frame_count(engine, 3, observer, "ARP Reply") == 0);
    CHECK(frame_count(engine, 3, observer, "Echo Request") == 0);
    CHECK(engine.arp_table(observer).empty());
    CHECK(engine.mac_table(3).size() == 2);
    // The observer only sees the broadcast request, which asks for someone else.
    CHECK(count(engine, {TraceKind::FrameReceived, observer}) == 1);
    CHECK(count(engine, {TraceKind::FrameDropped, observer, Reason::ArpForOtherHost}) == 1);
    CHECK(count(engine, {TraceKind::FrameDropped}) == 1);
}
void route_failures() {
    netsim::Engine engine;
    engine.seed_demo();
    CHECK(engine.start_ping(1, "10.0.0.1"));
    drain(engine);
    CHECK(!engine.ping_succeeded());
    CHECK(engine.events().size() == 1);
    CHECK(engine.events()[0].protocol == "IPv4");
    CHECK(!engine.events()[0].success);
    CHECK(engine.trace().records().size() == 1);
    CHECK(count(engine, {TraceKind::PingFailed, 1, Reason::NoRoute}) == 1);

    CHECK(engine.configure_host(1, "Wide subnet", "192.168.1.10", 16));
    CHECK(engine.configure_host(2, "Narrow subnet", "192.168.2.20", 24));
    CHECK(engine.start_ping(1, "192.168.2.20"));
    drain(engine);
    CHECK(!engine.ping_succeeded());
    CHECK(contains(engine, "no return route"));
    CHECK(contains(engine, "reply timed out"));
    CHECK(!engine.arp_table(1).empty());
    CHECK(count(engine, {TraceKind::FrameDropped, 2, Reason::NoRoute}) == 1);
    CHECK(count(engine, {TraceKind::PingFailed, 1, Reason::EchoTimeout}) == 1);
}
void links_and_timeouts() {
    netsim::Engine engine;
    engine.seed_demo();
    CHECK(engine.set_link_enabled(1, false));
    CHECK(engine.start_ping(1, "192.168.1.20"));
    drain(engine);
    CHECK(!engine.ping_succeeded());
    CHECK(engine.time_ms() == 3000);
    CHECK(engine.arp_table(1).empty());
    CHECK(contains(engine, "no frame can leave"));
    CHECK(contains(engine, "ARP resolution timed out"));
    CHECK(count(engine, {TraceKind::FrameDropped, 1, Reason::NoCarrier}) == 3);
    CHECK(count(engine, {TraceKind::ArpTimeout, 1}) == 3);
    CHECK(count(engine, {TraceKind::PingFailed, 1, Reason::ArpUnresolved}) == 1);
    CHECK(count(engine, {TraceKind::FrameSent}) == 0);
    CHECK(std::none_of(engine.events().begin(), engine.events().end(), [](const auto& event) { return event.protocol == "ICMP"; }));
    CHECK(engine.set_link_enabled(1, true));
    CHECK(engine.set_device_enabled(2, false));
    CHECK(engine.start_ping(1, "192.168.1.20"));
    drain(engine);
    CHECK(!engine.ping_succeeded());
    CHECK(contains(engine, "the device is disabled"));
    CHECK(count(engine, {TraceKind::FrameDropped, 2, Reason::DeviceDown}) == 3);
    CHECK(engine.set_device_enabled(2, true));
    CHECK(engine.start_ping(1, "192.168.1.99"));
    drain(engine);
    CHECK(!engine.ping_succeeded());
    CHECK(contains(engine, "ARP resolution timed out"));
}
void topology_validation() {
    netsim::Engine engine;
    const int a = engine.add_device(netsim::DeviceKind::Switch, 0, 0);
    const int b = engine.add_device(netsim::DeviceKind::Switch, 0, 0);
    const int c = engine.add_device(netsim::DeviceKind::Switch, 0, 0);
    CHECK(engine.connect(a, b));
    CHECK(engine.connect(b, c));
    CHECK(!engine.connect(c, a));
    CHECK(engine.last_error().find("STP") != std::string::npos);
    CHECK(engine.set_link_enabled(1, false));
    CHECK(!engine.connect(c, a));
    CHECK(!engine.connect(a, a));
    CHECK(!engine.connect(1000, a));

    engine.clear();
    const int pc = engine.add_device(netsim::DeviceKind::Host, 0, 0);
    const int pc2 = engine.add_device(netsim::DeviceKind::Host, 0, 0);
    const int pc3 = engine.add_device(netsim::DeviceKind::Host, 0, 0);
    CHECK(engine.connect(pc, pc2));
    CHECK(!engine.connect(pc, pc3));
    CHECK(engine.start_ping(pc, engine.device(pc2)->ip.to_string()));
    drain(engine);
    CHECK(engine.ping_succeeded());

    engine.clear();
    const int sw = engine.add_device(netsim::DeviceKind::Switch, 0, 0);
    for (int i = 0; i < 8; ++i) CHECK(engine.connect(sw, engine.add_device(netsim::DeviceKind::Host, 0, 0)));
    CHECK(!engine.connect(sw, engine.add_device(netsim::DeviceKind::Host, 0, 0)));
}
void configuration_and_edits() {
    netsim::Engine engine;
    engine.seed_demo();
    CHECK(!engine.configure_host(1, "PC", "01.2.3.4", 24));
    CHECK(!engine.configure_host(1, "PC", "192.168.1.0", 24));
    CHECK(!engine.configure_host(1, "PC", "192.168.1.255", 24));
    CHECK(!engine.configure_host(1, "PC", "224.0.0.1", 24));
    CHECK(!engine.configure_host(1, "PC", "127.0.0.1", 24));
    CHECK(!engine.configure_host(1, "PC", "192.168.1.10", 0));
    CHECK(!engine.configure_host(1, "PC", "192.168.1.10", 32));
    CHECK(!engine.configure_host(1, "\n", "192.168.1.10", 24));
    CHECK(!engine.configure_host(3, "Switch", "192.168.1.10", 24));
    CHECK(engine.device(1)->ip.to_string() == "192.168.1.10");
    CHECK(!engine.start_ping(3, "192.168.1.20"));
    CHECK(!engine.start_ping(1, "192.168.1.255"));
    CHECK(engine.start_ping(1, "192.168.1.20"));
    CHECK(engine.move_device(1, 42, 64));
    CHECK(engine.pending());
    CHECK(!engine.configure_host(1, "PC", "bad", 24));
    CHECK(engine.pending());
    CHECK(engine.remove_device(2));
    CHECK(engine.links().size() == 1);
    CHECK(!engine.pending());
    CHECK(engine.events().empty());
    CHECK(engine.trace().records().empty() && engine.trace().pdus().empty());
    CHECK(engine.arp_table(1).empty());
    CHECK(!engine.remove_device(2));
    CHECK(engine.remove_link(1));
    CHECK(engine.links().empty());

    engine.clear();
    const int solo = engine.add_device(netsim::DeviceKind::Host, 0, 0);
    CHECK(engine.start_ping(solo, engine.device(solo)->ip.to_string()));
    drain(engine);
    CHECK(engine.ping_succeeded());
    CHECK(engine.events().size() == 1);
    CHECK(engine.events()[0].link_id == -1);
    CHECK(engine.trace().records().size() == 1);
    CHECK(engine.trace().records()[0].kind == TraceKind::PingSucceeded && !engine.trace().records()[0].pdu);
}
void deterministic_reset() {
    netsim::Engine a, b;
    a.seed_demo(); b.seed_demo();
    CHECK(a.start_ping(1, "192.168.1.20"));
    CHECK(b.start_ping(1, "192.168.1.20"));
    drain(a); drain(b);
    CHECK(a.events().size() == b.events().size());
    for (std::size_t i = 0; i < a.events().size(); ++i) {
        CHECK(a.events()[i].summary == b.events()[i].summary);
        CHECK(a.events()[i].detail == b.events()[i].detail);
        CHECK(a.events()[i].time_ms == b.events()[i].time_ms);
    }
    CHECK(!a.trace().records().empty());
    CHECK(a.trace().records() == b.trace().records());
    CHECK(a.trace().pdus() == b.trace().pdus());
    a.reset_simulation();
    CHECK(a.events().empty());
    CHECK(a.mac_table(3).empty());
    CHECK(a.arp_table(1).empty());
    CHECK(a.devices().size() == 3 && a.links().size() == 2);
    CHECK(a.trace().records().empty() && a.now() == 0ns);
    CHECK(a.start_ping(1, "192.168.1.20"));
    drain(a);
    CHECK(a.events().size() == b.events().size());
    CHECK(a.trace().records() == b.trace().records());
    CHECK(a.ping_succeeded());
}
} // namespace

int main() {
    basic_ping_and_cache();
    transmissions_and_run_until();
    broadcast_and_unicast();
    route_failures();
    links_and_timeouts();
    topology_validation();
    configuration_and_edits();
    deterministic_reset();
    std::cout << "Network tests passed: " << test::checks << " checks.\n";
}

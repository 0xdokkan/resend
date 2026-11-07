// Milestone M5: routers and static routing. The routing table and the
// longest prefix match, hosts with a default gateway, forwarding with the
// TTL decremented, ICMP errors with their quote and the rules about when
// none is sent, traceroute, and routing loops that end when the TTL runs out.
#include "check.hpp"
#include "core/rng.hpp"
#include "explain/explain.hpp"
#include "routed_networks.hpp"
#include "sim/routing.hpp"
#include "sim_support.hpp"
#include "wire/wire.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using netsim::DeviceConfig;
using netsim::DeviceId;
using netsim::DeviceKind;
using netsim::EchoState;
using netsim::Ipv4Prefix;
using netsim::PingState;
using netsim::PortRef;
using netsim::Problem;
using netsim::Reason;
using netsim::Scenario;
using netsim::Simulation;
using netsim::TraceKind;
using namespace simtest;

Ipv4Prefix prefix(const char* text) { return *Ipv4Prefix::parse(text); }

// The frames sent on a cable, in order, as a short line each: "ARP 1",
// "ARP 2", or "ICMP 8/0 ttl 64": what the comparison with Linux looks at.
std::vector<std::string> on_link(const Simulation& simulation, std::uint32_t link) {
    std::vector<std::string> lines;
    for (const auto& sent : simulation.trace().transmissions()) {
        if (sent.link.value != link) continue;
        const auto& frame = simulation.trace().pdu(sent.pdu);
        if (const auto* arp = frame.arp()) {
            lines.push_back("ARP " + std::to_string(static_cast<int>(arp->op)));
        } else {
            const auto& packet = *frame.ipv4();
            lines.push_back("ICMP " + std::to_string(packet.icmp.type) + "/" + std::to_string(packet.icmp.code) + " ttl " +
                            std::to_string(packet.ttl));
        }
    }
    return lines;
}

const netsim::EthernetFrame* first_frame(const Simulation& simulation, netsim::PduKind kind) {
    for (const auto& frame : simulation.trace().pdus())
        if (netsim::kind_of(frame) == kind) return &frame;
    return nullptr;
}

netsim::PingId start(Simulation& simulation, DeviceId source, const char* destination, const netsim::PingOptions& options = {}) {
    return ping(simulation, source, ip(destination), options);
}

void table_and_longest_prefix() {
    DeviceConfig router{DeviceId{1}, DeviceKind::Router, "R", true, netsim::default_ports(DeviceKind::Router, DeviceId{1})};
    router.ports[0].ipv4 = prefix("10.1.0.1/24");
    router.ports[2].ipv4 = prefix("172.16.0.1/30");
    router.ports[3].ipv4 = prefix("172.16.0.5/30");
    router.routes = {
        {prefix("0.0.0.0/0"), ip("172.16.0.2")},
        {prefix("10.0.0.0/8"), ip("172.16.0.6")},
        {prefix("10.20.0.0/16"), ip("172.16.0.2")},
        {prefix("10.20.30.0/24"), ip("172.16.0.6")},
        {prefix("10.1.0.0/24"), ip("172.16.0.2")},   // a connected network has this destination
        {prefix("192.168.0.0/16"), ip("192.0.2.1")}, // the next hop is on none of the router's networks
        {prefix("192.0.2.0/24"), ip("10.1.0.1")},    // the next hop is the router itself
    };
    const auto table = netsim::routing_table(router);
    CHECK(table.size() == 10);
    // Connected networks first, in port order; ports without an address have none.
    CHECK(table[0].connected && table[0].destination == prefix("10.1.0.0/24") && table[0].port == 0);
    CHECK(table[1].connected && table[1].destination == prefix("172.16.0.0/30") && table[1].port == 2);
    CHECK(table[2].connected && table[2].port == 3);
    CHECK(!table[3].connected && table[3].next_hop == ip("172.16.0.2") && table[3].port == 2);
    CHECK(table[4].port == 3 && table[5].port == 2 && table[6].port == 3);
    CHECK(!table[7].active() && !table[8].active() && !table[9].active());

    const auto via = [&](const char* destination) {
        const auto route = netsim::lookup(table, ip(destination));
        return route ? route->destination.to_string() : std::string("none");
    };
    // /8, /16 and /24 overlap: the longest prefix that matches wins (RFC 1812 §5.2.4.3).
    CHECK(via("10.20.30.40") == "10.20.30.0/24");
    CHECK(via("10.20.31.40") == "10.20.0.0/16");
    CHECK(via("10.21.0.1") == "10.0.0.0/8");
    CHECK(via("11.0.0.1") == "0.0.0.0/0");
    CHECK(via("10.1.0.77") == "10.1.0.0/24");
    CHECK(netsim::lookup(table, ip("10.1.0.77"))->connected);
    CHECK(via("192.168.1.1") == "0.0.0.0/0");  // the inactive /16 is not used
    CHECK(netsim::next_hop(*netsim::lookup(table, ip("10.1.0.77")), ip("10.1.0.77")) == ip("10.1.0.77"));
    CHECK(netsim::next_hop(*netsim::lookup(table, ip("10.20.30.40")), ip("10.20.30.40")) == ip("172.16.0.6"));
    // Without a default route some destinations have no route at all.
    router.routes.erase(router.routes.begin());
    CHECK(!netsim::lookup(netsim::routing_table(router), ip("11.0.0.1")));
    // The next hop resolves through the longest connected network.
    router.ports[1].ipv4 = prefix("172.16.0.0/16");
    router.routes = {{prefix("0.0.0.0/0"), ip("172.16.0.6")}};
    CHECK(netsim::routing_table(router).back().port == 3);
}

void two_lans_and_a_router() {
    const auto scenario = routed::two_lans();
    const DeviceId a{1}, router{3}, b{5};
    Simulation simulation(scenario);
    const auto id = start(simulation, a, "10.2.0.20");
    drain(simulation);
    const auto& status = *simulation.ping(id);
    CHECK(status.state == PingState::Succeeded && status.echoes[0].responder == ip("10.2.0.20"));
    // RFC 1122 §3.3.1: PC-A asks for its gateway's MAC, not PC-B's.
    const auto* request = first_frame(simulation, netsim::PduKind::ArpRequest);
    CHECK(request && request->arp()->target_ip == ip("10.1.0.1") && request->arp()->sender_ip == ip("10.1.0.10"));
    // Each side of the router: ARP, then the echo with TTL 64 on PC-A's
    // side and 63 on PC-B's (RFC 1812 §5.3.1), and the reply the other way.
    CHECK((on_link(simulation, 1) == std::vector<std::string>{"ARP 1", "ARP 2", "ICMP 8/0 ttl 64", "ICMP 0/0 ttl 63"}));
    CHECK((on_link(simulation, 4) == std::vector<std::string>{"ARP 1", "ARP 2", "ICMP 8/0 ttl 63", "ICMP 0/0 ttl 64"}));
    // The router forwarded both datagrams by its connected networks,
    // keeping their identification, and keeps an ARP cache on each port.
    CHECK(count(simulation, TraceKind::DatagramRouted, router) == 2);
    for (const auto& record : simulation.trace().records())
        if (record.kind == TraceKind::DatagramRouted)
            CHECK(record.route == prefix("10.2.0.0/24") || record.route == prefix("10.1.0.0/24"));
    std::vector<std::uint16_t> requests;
    for (const auto& frame : simulation.trace().pdus())
        if (netsim::kind_of(frame) == netsim::PduKind::EchoRequest) requests.push_back(frame.ipv4()->identification);
    CHECK(requests.size() == 2 && requests[0] == requests[1]);
    const auto cache = simulation.arp_cache(router);
    CHECK(cache.size() == 2 && cache[0].ip == ip("10.1.0.10") && cache[0].port == 0 && cache[1].ip == ip("10.2.0.20") && cache[1].port == 1);
    CHECK(simulation.arp_cache(a).size() == 1 && simulation.arp_cache(a)[0].ip == ip("10.1.0.1"));
    // PC-B learned the router from its ARP request and needed no ARP of its own.
    CHECK(arp_requests(simulation, b) == 0);

    // A second ping finds every cache warm: four frames on each side.
    start(simulation, a, "10.2.0.20");
    drain(simulation);
    CHECK(on_link(simulation, 1).size() == 6 && on_link(simulation, 4).size() == 6);

    // The router answers pings to any of its addresses, from that address.
    const auto far_side = start(simulation, a, "10.2.0.1");
    drain(simulation);
    CHECK(simulation.ping(far_side)->state == PingState::Succeeded);
    const auto& replies = simulation.trace().pdus();
    const auto last_reply = std::find_if(replies.rbegin(), replies.rend(),
                                         [](const auto& frame) { return netsim::kind_of(frame) == netsim::PduKind::EchoReply; });
    CHECK(last_reply->ipv4()->src == ip("10.2.0.1") && last_reply->ipv4()->ttl == 64);
}

void gateways() {
    // No gateway: other subnets are unreachable, and nothing is sent.
    {
        auto scenario = routed::two_lans();
        scenario.device(DeviceId{1})->gateway.reset();
        Simulation simulation(scenario);
        const auto id = start(simulation, DeviceId{1}, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::NoRoute && simulation.trace().transmissions().empty());
    }
    // A gateway outside the subnet cannot be reached.
    {
        auto scenario = routed::two_lans();
        scenario.device(DeviceId{1})->gateway = ip("10.2.0.1");
        Simulation simulation(scenario);
        const auto id = start(simulation, DeviceId{1}, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::GatewayOffLink && simulation.trace().transmissions().empty());
    }
    // The wrong gateway: an address of the subnet nobody has. ARP gives up.
    {
        auto scenario = routed::two_lans();
        scenario.device(DeviceId{1})->gateway = ip("10.1.0.254");
        Simulation simulation(scenario);
        const auto id = start(simulation, DeviceId{1}, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::ArpUnresolved && arp_requests(simulation, DeviceId{1}) == 3);
        // Hosts of the subnet are still reachable without the gateway.
        CHECK(simulation.ping(start(simulation, DeviceId{1}, "10.1.0.1"))->state == PingState::Running);
        drain(simulation);
        CHECK(simulation.pings().back().state == PingState::Succeeded);
    }
    // The gateway is another PC: it is not a router, and drops the datagram.
    {
        routed::Network net;
        const auto a = net.pc("PC-A", "10.1.0.10/24", "10.1.0.11");
        const auto other = net.pc("PC-C", "10.1.0.11/24");
        net.cable(a, 1, other, 1);
        Simulation simulation(net.done());
        const auto id = start(simulation, a, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::EchoTimeout);
        CHECK(count(simulation, TraceKind::FrameDropped, other, Reason::Ipv4NotForUs) == 1);
    }
    // PC-B without a gateway receives the request but cannot answer it.
    {
        auto scenario = routed::two_lans();
        scenario.device(DeviceId{5})->gateway.reset();
        Simulation simulation(scenario);
        const auto id = start(simulation, DeviceId{1}, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::EchoTimeout);
        CHECK(count(simulation, TraceKind::FrameDropped, DeviceId{5}, Reason::NoRoute) == 1);
    }
}

void icmp_errors() {
    const DeviceId a{1}, r1{2}, r2{3}, b{4};
    // No route at R1: a Destination Unreachable (network) comes back at once.
    {
        Simulation simulation(routed::chain());
        const auto id = start(simulation, a, "10.9.9.9", {.timeout = 5s});
        drain(simulation);
        const auto& status = *simulation.ping(id);
        CHECK(status.reason == Reason::NetUnreachable && status.echoes[0].responder == ip("10.1.0.1"));
        CHECK(status.finished == 40ms);  // ARP there and back, the request, the error
        CHECK(count(simulation, TraceKind::DatagramDropped, r1, Reason::NoRoute) == 1);
        const auto* error = first_frame(simulation, netsim::PduKind::Unreachable);
        CHECK(error && error->ipv4()->icmp.code == netsim::icmp::net_unreachable && error->ipv4()->src == ip("10.1.0.1"));
        // The quote is the request as it arrived: header and whole ICMP message.
        const auto& original = *error->ipv4()->icmp.original;
        CHECK(original.src == ip("10.1.0.10") && original.dst == ip("10.9.9.9") && original.ttl == 64);
        CHECK(original.type == netsim::icmp::echo_request && original.identifier == id.value && original.sequence == 1);
        CHECK(original.data_size == netsim::ping_data_size);
        const auto bytes = netsim::wire::encode(*error);
        CHECK(bytes.size() == 14 + 20 + 8 + 84);
        CHECK(netsim::wire::decode(bytes) == *error);
    }
    // Nobody has the address at R2's side: after three ARP requests R2
    // sends a Destination Unreachable (host), quoting the request with the
    // TTL it had after R1 and R2.
    {
        Simulation simulation(routed::chain());
        const auto id = start(simulation, a, "10.2.0.99", {.timeout = 5s});
        drain(simulation);
        const auto& status = *simulation.ping(id);
        CHECK(status.reason == Reason::HostUnreachable && status.echoes[0].responder == ip("10.12.0.2"));
        CHECK(count(simulation, TraceKind::DatagramDropped, r2, Reason::ArpUnresolved) == 1);
        const auto* error = first_frame(simulation, netsim::PduKind::Unreachable);
        CHECK(error && error->ipv4()->icmp.code == netsim::icmp::host_unreachable && error->ipv4()->icmp.original->ttl == 62);
        // With ping's default timeout of a second the request has already
        // timed out when the error comes back, two seconds later.
        Simulation impatient(routed::chain());
        const auto late = start(impatient, a, "10.2.0.99");
        drain(impatient);
        CHECK(impatient.ping(late)->reason == Reason::EchoTimeout);
        CHECK(count(impatient, TraceKind::FrameDropped, a, Reason::UnmatchedError) == 1);
    }
    // The missing return route: the request arrives, the reply does not.
    // R2 sends its error about the reply to PC-B, which is not waiting for one.
    {
        Simulation simulation(routed::chain(false));
        const auto id = start(simulation, a, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::EchoTimeout);
        CHECK(count(simulation, TraceKind::DatagramDropped, r2, Reason::NoRoute) == 1);
        CHECK(count(simulation, TraceKind::FrameDropped, b, Reason::UnmatchedError) == 1);
        // The path there is fine: a traceroute stops at hop 3, which is PC-B,
        // whose replies never come back.
        CHECK(count(simulation, TraceKind::DatagramRouted, r1) == 1);
    }
    // A TTL of 1 dies at the first router, 2 reaches the far side.
    {
        Simulation simulation(routed::chain());
        const auto short_lived = start(simulation, a, "10.2.0.20", {.ttl = 1});
        drain(simulation);
        CHECK(simulation.ping(short_lived)->reason == Reason::TtlExceeded);
        CHECK(simulation.ping(short_lived)->echoes[0].responder == ip("10.1.0.1"));
        const auto* error = first_frame(simulation, netsim::PduKind::TimeExceeded);
        CHECK(error && error->ipv4()->icmp.original->ttl == 1 && error->ipv4()->ttl == 64);
        const auto enough = start(simulation, a, "10.2.0.20", {.ttl = 3});
        drain(simulation);
        CHECK(simulation.ping(enough)->state == PingState::Succeeded);
        const auto two = start(simulation, a, "10.2.0.20", {.ttl = 2});
        drain(simulation);
        CHECK(simulation.ping(two)->reason == Reason::TtlExceeded && simulation.ping(two)->echoes[0].responder == ip("10.12.0.2"));
    }
    // A directed broadcast is not forwarded, and no error comes back.
    {
        Simulation simulation(routed::chain());
        const auto id = start(simulation, a, "10.2.0.255");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::EchoTimeout);
        CHECK(count(simulation, TraceKind::DatagramDropped, r2, Reason::DirectedBroadcast) == 1);
        CHECK(!first_frame(simulation, netsim::PduKind::Unreachable));
    }
    // No error about an error (RFC 1122 §3.2.2): R2 has no route for the
    // destination, and its route back to PC-A goes to R3, which sends
    // everything back to R2. The error goes round until its own TTL runs
    // out, and no error is sent about it.
    {
        auto scenario = routed::chain();
        routed::Network net{scenario};
        const auto r3 = net.router("R3", {"10.23.0.2/30", ""});
        auto& second = *net.scenario.device(r2);
        second.ports = netsim::edit::resized_ports(second, 3);
        second.ports[2].ipv4 = prefix("10.23.0.1/30");
        second.routes = {{prefix("10.1.0.0/24"), ip("10.23.0.2")}};
        net.cable(r2, 3, r3, 1);
        net.route(r3, "0.0.0.0/0", "10.23.0.1");
        net.route(r1, "0.0.0.0/0", "10.12.0.2");
        Simulation simulation(net.done());
        const auto id = start(simulation, a, "10.9.9.9");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::EchoTimeout);
        CHECK(count(simulation, TraceKind::DatagramDropped, r2, Reason::NoRoute) == 1);
        int errors = 0, quiet = 0;
        for (const auto& record : simulation.trace().records()) {
            if (record.kind != TraceKind::DatagramDropped || record.reason != Reason::TtlExceeded) continue;
            ++quiet;
            CHECK(netsim::icmp::is_error(simulation.trace().pdu(*record.pdu).ipv4()->icmp.type));
        }
        for (const auto& frame : simulation.trace().pdus()) errors += netsim::is_icmp_error(netsim::kind_of(frame));
        CHECK(quiet == 1);
        // The one error, forwarded hop after hop: R2 sent it with TTL 64,
        // and it died when its TTL reached 1. Nothing else was an error.
        std::vector<int> ttls;
        for (const auto& frame : simulation.trace().pdus())
            if (netsim::is_icmp_error(netsim::kind_of(frame))) ttls.push_back(frame.ipv4()->ttl);
        CHECK(ttls.size() == 64 && ttls.front() == 64 && ttls.back() == 1);
        CHECK(errors == 64 && count(simulation, TraceKind::SimulatorGuard) == 0);
    }
}

void routing_loop_and_traceroute() {
    const DeviceId a{1}, r1{2}, r2{3};
    // A routing loop ends when the TTL runs out. TTL 64 leaves PC-A: R1
    // receives it with 64 and forwards it with 63, R2 receives 63, R1 62,
    // ... R2 receives it with 1 and drops it: 63 forwards. R2's Time
    // Exceeded goes back through R1, one forward more. No storm guard:
    // every hop is a new frame.
    {
        Simulation simulation(routed::routing_loop());
        const auto id = start(simulation, a, "10.9.9.9");
        drain(simulation);
        const auto& status = *simulation.ping(id);
        CHECK(status.reason == Reason::TtlExceeded);
        CHECK(count(simulation, TraceKind::DatagramRouted) == 64);
        CHECK(count(simulation, TraceKind::DatagramRouted, r1) == 33 && count(simulation, TraceKind::DatagramRouted, r2) == 31);
        CHECK(count(simulation, TraceKind::DatagramDropped, r2, Reason::TtlExceeded) == 1);
        CHECK(count(simulation, TraceKind::SimulatorGuard) == 0);
        CHECK(status.echoes[0].responder == ip("10.12.0.2"));
        CHECK(status.finished < 1s);
    }
    // Traceroute: one request at a time with TTL 1, 2, 3; each router
    // answers with a Time Exceeded, the destination with its reply.
    {
        Simulation simulation(routed::chain());
        const auto id = start(simulation, a, "10.2.0.20", {.count = netsim::default_max_hops, .traceroute = true});
        drain(simulation);
        const auto& status = *simulation.ping(id);
        CHECK(status.state == PingState::Succeeded && status.echoes.size() == 3);
        CHECK(status.echoes[0].ttl == 1 && status.echoes[1].ttl == 2 && status.echoes[2].ttl == 3);
        CHECK(status.echoes[0].responder == ip("10.1.0.1") && status.echoes[0].reason == Reason::TtlExceeded);
        CHECK(status.echoes[1].responder == ip("10.12.0.2") && status.echoes[1].reason == Reason::TtlExceeded);
        CHECK(status.echoes[2].responder == ip("10.2.0.20") && status.echoes[2].state == EchoState::Replied);
        // The next request leaves when the last answer arrives.
        CHECK(status.echoes[1].sent == status.echoes[0].finished && status.echoes[2].sent == status.echoes[1].finished);
        CHECK(count(simulation, TraceKind::PingStarted) == 1);
    }
    // In a loop, traceroute shows the two routers taking turns until it
    // gives up after its maximum number of hops.
    {
        Simulation simulation(routed::routing_loop());
        const auto id = start(simulation, a, "10.9.9.9", {.count = 8, .traceroute = true});
        drain(simulation);
        const auto& status = *simulation.ping(id);
        CHECK(status.state == PingState::Failed && status.reason == Reason::TtlExceeded && status.echoes.size() == 8);
        // Each router answers from the port its error leaves by, towards
        // PC-A (RFC 1812 §4.3.2.4): R1 always from 10.1.0.1.
        for (std::size_t hop = 0; hop < 8; ++hop) CHECK(status.echoes[hop].responder == (hop % 2 ? ip("10.12.0.2") : ip("10.1.0.1")));
    }
    // A Destination Unreachable stops it; so does a hop that cannot leave.
    {
        Simulation simulation(routed::chain());
        const auto unreachable = start(simulation, a, "10.9.9.9", {.count = 30, .traceroute = true});
        drain(simulation);
        CHECK(simulation.ping(unreachable)->reason == Reason::NetUnreachable && simulation.ping(unreachable)->echoes.size() == 1);
        auto scenario = routed::chain();
        scenario.device(a)->gateway.reset();
        Simulation lonely(scenario);
        const auto nowhere = start(lonely, a, "10.2.0.20", {.count = 30, .traceroute = true});
        drain(lonely);
        CHECK(lonely.ping(nowhere)->reason == Reason::NoRoute && lonely.ping(nowhere)->echoes.size() == 1);
    }
    // PC-B is off. With time to wait, R2's Destination Unreachable (host)
    // ends the traceroute at hop 3; with ping's one second, hop 3 shows
    // no answer, and so does hop 4, the last one allowed here.
    {
        auto scenario = routed::chain();
        scenario.device(DeviceId{4})->powered = false;
        Simulation patient(scenario);
        const auto id = start(patient, a, "10.2.0.20", {.count = 4, .timeout = 5s, .traceroute = true});
        drain(patient);
        const auto& status = *patient.ping(id);
        CHECK(status.echoes.size() == 3 && status.reason == Reason::HostUnreachable);
        CHECK(status.echoes[2].responder == ip("10.12.0.2"));
        Simulation hasty(scenario);
        const auto quick = start(hasty, a, "10.2.0.20", {.count = 4, .traceroute = true});
        drain(hasty);
        const auto& stars = *hasty.ping(quick);
        CHECK(stars.echoes.size() == 4 && stars.reason == Reason::EchoTimeout && stars.state == PingState::Failed);
        CHECK(stars.echoes[2].responder == netsim::Ipv4Address{} && stars.echoes[3].responder == netsim::Ipv4Address{});
        CHECK(stars.echoes[3].sent == stars.echoes[2].finished);
    }
    // Options: a traceroute has at most 255 hops; a TTL is at least 1.
    {
        Simulation simulation(routed::chain());
        Problem problem{};
        CHECK(!simulation.start_ping(a, ip("10.2.0.20"), problem, {.count = 256, .traceroute = true}));
        CHECK(problem == Problem::InvalidPingOptions);
        CHECK(!simulation.start_ping(a, ip("10.2.0.20"), problem, {.ttl = 0}));
        CHECK(!simulation.start_ping(r2, ip("10.2.0.20"), problem));
        CHECK(problem == Problem::SourceNotHost);
    }
}

void router_ports() {
    const DeviceId a{1}, r1{2};
    // A port without an address takes no part in IPv4.
    {
        routed::Network net;
        const auto pc = net.pc("PC", "10.5.0.10/24", "10.5.0.1");
        const auto router = net.router("R", {"10.1.0.1/24", ""});
        net.cable(pc, 1, router, 2);
        Simulation simulation(net.done());
        const auto id = start(simulation, pc, "10.1.0.1");
        drain(simulation);
        CHECK(simulation.ping(id)->reason == Reason::ArpUnresolved);
        CHECK(count(simulation, TraceKind::FrameDropped, router, Reason::NoAddress) == 3);
    }
    // Frames for another MAC are ignored, as by any interface.
    {
        Simulation simulation(routed::two_lans());
        start(simulation, DeviceId{1}, "10.1.0.99");
        drain(simulation);
        CHECK(count(simulation, TraceKind::FrameDropped, DeviceId{3}, Reason::ArpForOtherHost) == 3);
    }
    // Live: the route to PC-B goes away while the request travels, then
    // comes back; the address of R1's port goes away while R1 resolves.
    {
        Simulation simulation(routed::chain());
        start(simulation, a, "10.2.0.20", {.timeout = 5s});
        simulation.run_until(5ms);
        CHECK(simulation.apply(netsim::SetRoutes{r1, {}}) == Problem::None);
        drain(simulation);
        CHECK(simulation.pings().back().reason == Reason::NetUnreachable);
        CHECK(simulation.apply(netsim::SetRoutes{r1, {{prefix("10.2.0.0/24"), ip("10.12.0.2")}}}) == Problem::None);
        start(simulation, a, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.pings().back().state == PingState::Succeeded);
    }
    {
        Simulation simulation(routed::chain());
        start(simulation, a, "10.12.0.2", {.timeout = 5s});
        simulation.run_until(35ms);  // R1 has asked for R2's MAC
        CHECK(simulation.arp_cache(r1).size() == 2 && simulation.arp_cache(r1)[1].state == netsim::ArpState::Incomplete);
        CHECK(simulation.apply(netsim::SetPortUp{PortRef{DeviceId{3}, 0}, false}) == Problem::None);
        CHECK(simulation.apply(netsim::SetAddress{PortRef{r1, 1}, std::nullopt}) == Problem::None);
        drain(simulation);
        CHECK(count(simulation, TraceKind::DatagramDropped, r1, Reason::NoAddress) == 1);
        CHECK(simulation.pings().back().reason == Reason::EchoTimeout);
    }
    // Powering a router off empties its caches; more ports work like the first.
    {
        Simulation simulation(routed::chain());
        start(simulation, a, "10.2.0.20");
        drain(simulation);
        CHECK(!simulation.arp_cache(r1).empty());
        CHECK(simulation.apply(netsim::SetPowered{r1, false}) == Problem::None);
        CHECK(simulation.apply(netsim::SetPowered{r1, true}) == Problem::None);
        CHECK(simulation.arp_cache(r1).empty());
        const auto* router = simulation.scenario().device(r1);
        CHECK(simulation.apply(netsim::SetPorts{r1, netsim::edit::resized_ports(*router, 8)}) == Problem::None);
        CHECK(simulation.apply(netsim::SetGateway{a, std::nullopt}) == Problem::None);
        start(simulation, a, "10.2.0.20");
        drain(simulation);
        CHECK(simulation.pings().back().reason == Reason::NoRoute);
        CHECK(simulation.edits().size() == 4);
    }
}

void explanations() {
    // Every event of the routed scenarios has words, and the key decisions
    // are named: the route, the TTL, the error and why one was not sent.
    std::string all;
    const auto story = [&](Scenario scenario, DeviceId source, const char* destination, netsim::PingOptions options = {}) {
        Simulation simulation(std::move(scenario));
        netsim::explain::Explainer explainer;
        start(simulation, source, destination, options);
        for (int guard = 0; simulation.pending() && guard < 100'000; ++guard) {
            simulation.step();
            explainer.update(simulation);
        }
        std::string text;
        for (const auto& event : explainer.events()) {
            CHECK(!event.summary.empty() && !event.detail.empty());
            text += event.summary + "\n" + event.detail + "\n";
        }
        all += text;
        return text;
    };
    const auto has = [](const std::string& text, const std::string& part) {
        const bool found = text.find(part) != std::string::npos;
        if (!found) std::cout << "  missing: " << part << '\n';
        return found;
    };
    const auto lan = story(routed::two_lans(), DeviceId{1}, "10.2.0.20");
    CHECK(has(lan, "the datagram goes to the default gateway"));
    CHECK(has(lan, "Route: 10.2.0.20 is on the connected network 10.2.0.0/24, port 2."));
    CHECK(has(lan, "TTL 64 → 63."));
    CHECK(has(lan, "Forwarded from port 1 to 02:00:00:00:00:01."));
    const auto prefix_story = story(routed::longest_prefix(), DeviceId{1}, "10.20.30.10");
    CHECK(has(prefix_story, "Route: 10.20.30.0/24 via 172.16.3.2, port 4. It is the longest prefix that matches: 10.0.0.0/8, 10.20.0.0/16 match too, but are shorter."));
    const auto loop = story(routed::routing_loop(), DeviceId{1}, "10.9.9.9");
    CHECK(has(loop, "TTL 1: forwarding would take it to 0, so the router discards the datagram (RFC 1812 §5.3.1)."));
    CHECK(has(loop, "Time Exceeded sent back to 10.1.0.10 from port 1."));
    CHECK(has(loop, "Ping failed · TTL exceeded"));
    const auto unreachable = story(routed::chain(), DeviceId{1}, "10.9.9.9");
    CHECK(has(unreachable, "No route matches 10.9.9.9, and there is no default route"));
    CHECK(has(unreachable, "10.1.0.1 has no route to 10.9.9.9 and sent back a Destination Unreachable (network)."));
    const auto trace = story(routed::chain(), DeviceId{1}, "10.2.0.20", {.count = 30, .traceroute = true});
    CHECK(has(trace, "Hop 1 · 10.1.0.1"));
    CHECK(has(trace, "Hop 3 · 10.2.0.20 reached"));
    CHECK(has(trace, "1  10.1.0.1  "));
    const auto host = story(routed::chain(), DeviceId{1}, "10.2.0.99", {.timeout = 5s});
    CHECK(has(host, "Three ARP requests for the next hop went unanswered"));
    CHECK(has(host, "A Destination Unreachable (host) goes back to 10.1.0.10."));
    CHECK(has(host, "Ping failed · host unreachable"));
    CHECK(all.find("not implemented") == std::string::npos && all.find("no routers") == std::string::npos);
}

// Random networks of routers and PCs, with random routes, gateways, pings
// and traceroutes. Every run ends, every ping ends, no frame needs the
// storm guard, every frame sent arrives, every frame survives the wire
// format, and the same seed gives the same trace on every run and platform.
std::uint64_t random_routed_run(std::uint64_t seed, int& pings, int& errors) {
    netsim::Rng rng(seed);
    routed::Network net;
    const auto routers = static_cast<int>(rng.between(1, 4));
    std::vector<DeviceId> ids;
    for (int r = 0; r < routers; ++r) {
        const auto id = net.router("R" + std::to_string(r + 1), {"", "", "", ""});
        auto& device = *net.scenario.device(id);
        // Port 1: this router's LAN, 10.<r+1>.0.1/24; ports 2-4 for links.
        device.ports[0].ipv4 = Ipv4Prefix(netsim::Ipv4Address(0x0a000001U | static_cast<std::uint32_t>(r + 1) << 16), 24);
        ids.push_back(id);
    }
    // A chain of routers, sometimes closed into a ring.
    for (int r = 0; r + 1 < routers; ++r) {
        const auto link = static_cast<std::uint32_t>(0xac100000U | static_cast<std::uint32_t>(r) << 8);  // 172.16.r.0/30
        net.scenario.device(ids[static_cast<std::size_t>(r)])->ports[1].ipv4 = Ipv4Prefix(netsim::Ipv4Address(link | 1), 30);
        net.scenario.device(ids[static_cast<std::size_t>(r + 1)])->ports[2].ipv4 = Ipv4Prefix(netsim::Ipv4Address(link | 2), 30);
        net.cable(ids[static_cast<std::size_t>(r)], 2, ids[static_cast<std::size_t>(r + 1)], 3);
    }
    std::vector<DeviceId> hosts;
    for (int r = 0; r < routers; ++r) {
        const auto lan = 0x0a000000U | static_cast<std::uint32_t>(r + 1) << 16;
        const auto pc = net.pc("PC" + std::to_string(r + 1), "10.0.0.10/24");
        auto& device = *net.scenario.device(pc);
        device.ports[0].ipv4 = Ipv4Prefix(netsim::Ipv4Address(lan | 10), 24);
        if (!rng.chance(1, 6)) device.gateway = netsim::Ipv4Address(lan | (rng.chance(1, 8) ? 254U : 1U));
        net.cable(pc, 1, ids[static_cast<std::size_t>(r)], 1);
        hosts.push_back(pc);
    }
    // Random routes to the LANs and random defaults, loops included.
    for (int r = 0; r < routers; ++r) {
        auto& device = *net.scenario.device(ids[static_cast<std::size_t>(r)]);
        for (int target = 0; target < routers; ++target) {
            if (target == r || rng.chance(1, 4)) continue;
            const auto lan = netsim::Ipv4Address(0x0a000000U | static_cast<std::uint32_t>(target + 1) << 16);
            const bool forward = target > r ? !rng.chance(1, 5) : rng.chance(1, 5);
            const auto neighbour = forward ? r : r - 1;
            if (neighbour < 0 || neighbour + 1 >= routers) continue;
            const auto link = 0xac100000U | static_cast<std::uint32_t>(neighbour) << 8;
            device.routes.push_back({Ipv4Prefix(lan, 16 + 8 * static_cast<int>(rng.below(2))).canonical(),
                                     netsim::Ipv4Address(link | (forward ? 2U : 1U))});
        }
        if (rng.chance(1, 2) && routers > 1) {
            const auto neighbour = r + 1 < routers ? r : r - 1;
            const auto link = 0xac100000U | static_cast<std::uint32_t>(neighbour) << 8;
            device.routes.push_back({prefix("0.0.0.0/0"), netsim::Ipv4Address(link | (r + 1 < routers ? 2U : 1U))});
        }
        std::sort(device.routes.begin(), device.routes.end(), [](const auto& x, const auto& y) { return x.destination < y.destination; });
        device.routes.erase(std::unique(device.routes.begin(), device.routes.end(),
                                        [](const auto& x, const auto& y) { return x.destination == y.destination; }),
                            device.routes.end());
    }
    Simulation simulation(net.done());
    for (int count = 0; count < 4; ++count) {
        const auto source = hosts[rng.below(hosts.size())];
        const auto target = netsim::Ipv4Address(0x0a000000U | static_cast<std::uint32_t>(rng.between(1, 5)) << 16 |
                                                (rng.chance(1, 2) ? 10U : 1U));
        netsim::PingOptions options{.count = static_cast<std::uint16_t>(rng.between(1, 2)), .timeout = 3s};
        if (rng.chance(1, 3)) options = {.count = 12, .timeout = 3s, .traceroute = true};
        Problem problem{};
        if (simulation.start_ping(source, target, problem, options)) ++pings;
        simulation.run_until(simulation.now() + std::chrono::milliseconds(rng.between(0, 30)));
    }
    drain(simulation);
    for (const auto& status : simulation.pings()) CHECK(status.state != PingState::Running);
    CHECK(count(simulation, TraceKind::SimulatorGuard) == 0);
    CHECK(simulation.frames_in_flight() == 0);
    CHECK(simulation.trace().transmissions().size() == static_cast<std::size_t>(count(simulation, TraceKind::FrameReceived)));
    for (const auto& frame : simulation.trace().pdus()) {
        errors += netsim::is_icmp_error(netsim::kind_of(frame));
        CHECK(netsim::wire::decode(netsim::wire::encode(frame)) == frame);
    }
    Hash hash;
    add_trace(hash, simulation);
    return hash.value;
}

void random_routed_networks() {
    // The same on every platform and compiler; it changes only with the model.
    constexpr std::uint64_t expected = 0xee71025ff99c2ec6;
    int pings = 0, errors = 0, again_pings = 0, again_errors = 0;
    std::uint64_t combined = 0;
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        const auto first = random_routed_run(seed, pings, errors);
        CHECK(random_routed_run(seed, again_pings, again_errors) == first);
        combined ^= first + seed;
    }
    std::cout << "  200 random routed networks: " << pings << " pings and traceroutes, " << errors << " ICMP errors; hash 0x"
              << std::hex << combined << std::dec << '\n';
    CHECK(pings > 500 && errors > 100);
    CHECK(combined == expected);
}
} // namespace

int main() {
    table_and_longest_prefix();
    two_lans_and_a_router();
    gateways();
    icmp_errors();
    routing_loop_and_traceroute();
    router_ports();
    explanations();
    random_routed_networks();
    std::cout << "Routing tests passed: " << test::checks << " checks.\n";
}

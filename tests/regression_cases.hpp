#pragma once

// Regression cases. The first fourteen are the prototype's behaviour: each
// network and its pings were checked against the prototype's engine, record
// by record and text by text, before that engine was removed. The others
// cover what came after, with scripts that change the network as it runs,
// and from 24 on, routers.
#include "check.hpp"
#include "explain/explain.hpp"
#include "routed_networks.hpp"
#include "scenario/edit.hpp"
#include "sim/simulation.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace regression {
using namespace std::chrono_literals;

// Runs a simulation for a script, explaining after every step as the
// interface does.
struct Driver {
    netsim::Simulation& simulation;
    netsim::explain::Explainer& explainer;

    netsim::PingId ping(std::uint32_t source, const char* destination, netsim::PingOptions options = {}) {
        netsim::Problem problem{};
        const auto id = simulation.start_ping(netsim::DeviceId{source}, *netsim::Ipv4Address::parse(destination), problem, options);
        CHECK(id.has_value());
        return id.value_or(netsim::PingId{});
    }
    // Every event due up to `time`, then the clock moves there.
    void until(netsim::SimTime time) {
        for (int count = 0; simulation.next_event_time() && *simulation.next_event_time() <= time; ++count) {
            CHECK(simulation.step());
            explainer.update(simulation);
            CHECK(count < 100'000);
        }
        simulation.run_until(time);
    }
    void apply(const netsim::Edit& edit) {
        CHECK(simulation.apply(edit) == netsim::Problem::None);
        explainer.update(simulation);
    }
    void finish() {
        for (int count = 0; simulation.pending(); ++count) {
            CHECK(simulation.step());
            explainer.update(simulation);
            CHECK(count < 100'000);
        }
    }
};

struct Ping {
    std::uint32_t source;
    std::string destination;
};
struct Case {
    const char* file;  // golden file name
    const char* name;
    std::function<netsim::Scenario()> build;
    std::vector<Ping> pings;  // one after the other, each run to the end
    std::function<void(Driver&)> script = {};  // runs first, then to the end
};

inline netsim::DeviceId add(netsim::Scenario& scenario, netsim::DeviceKind kind) {
    netsim::DeviceId id{};
    CHECK(netsim::edit::add_device(scenario, kind, 0, 0, &id) == netsim::Problem::None);
    return id;
}
inline void connect(netsim::Scenario& scenario, std::uint32_t a, std::uint32_t b) {
    CHECK(netsim::edit::connect(scenario, netsim::DeviceId{a}, netsim::DeviceId{b}) == netsim::Problem::None);
}
inline netsim::Scenario sample() { return netsim::sample_lan(); }
// A switch (id 1) with `hosts` PCs (ids 2, 3, ...) on its first ports.
inline netsim::Scenario star(int hosts) {
    netsim::Scenario scenario;
    add(scenario, netsim::DeviceKind::Switch);
    for (int i = 0; i < hosts; ++i) connect(scenario, 1, add(scenario, netsim::DeviceKind::Host).value);
    return scenario;
}

inline std::vector<Case> cases() {
    using netsim::DeviceKind;
    using netsim::Problem;
    namespace edit = netsim::edit;
    return {
        {"01-warm-caches", "ping, then ping again with warm caches", sample,
         {{1, "192.168.1.20"}, {1, "192.168.1.20"}, {2, "192.168.1.10"}}},
        {"02-third-host", "a third host sees only the broadcast",
         [] {
             auto s = sample();
             add(s, DeviceKind::Host);
             connect(s, 4, 3);
             return s;
         },
         {{1, "192.168.1.20"}, {4, "192.168.1.10"}, {2, "192.168.1.13"}}},
        {"03-no-route", "no route", sample, {{1, "10.0.0.1"}}},
        {"04-no-return-route", "no return route, then a reply timeout",
         [] {
             auto s = sample();
             CHECK(edit::configure_host(s, netsim::DeviceId{1}, "Wide subnet", "192.168.1.10", 16) == Problem::None);
             CHECK(edit::configure_host(s, netsim::DeviceId{2}, "Narrow subnet", "192.168.2.20", 24) == Problem::None);
             return s;
         },
         {{1, "192.168.2.20"}, {1, "192.168.2.20"}}},
        {"05-cable-down-at-source", "cable down at the source",
         [] {
             auto s = sample();
             CHECK(edit::set_link_up(s, netsim::LinkId{1}, false) == Problem::None);
             return s;
         },
         {{1, "192.168.1.20"}}},
        {"06-cable-down-after-switch", "cable down after the switch",
         [] {
             auto s = sample();
             CHECK(edit::set_link_up(s, netsim::LinkId{2}, false) == Problem::None);
             return s;
         },
         {{1, "192.168.1.20"}}},
        {"07-destination-off", "destination powered off",
         [] {
             auto s = sample();
             CHECK(edit::set_powered(s, netsim::DeviceId{2}, false) == Problem::None);
             return s;
         },
         {{1, "192.168.1.20"}}},
        {"08-switch-off", "switch powered off",
         [] {
             auto s = sample();
             CHECK(edit::set_powered(s, netsim::DeviceId{3}, false) == Problem::None);
             return s;
         },
         {{1, "192.168.1.20"}}},
        {"09-nobody-then-success", "nobody has the address, then a successful ping", sample,
         {{1, "192.168.1.99"}, {1, "192.168.1.20"}}},
        {"10-local-ping", "local ping",
         [] {
             netsim::Scenario s;
             add(s, DeviceKind::Host);
             return s;
         },
         {{1, "192.168.1.10"}}},
        {"11-two-pcs-one-cable", "two PCs on one cable",
         [] {
             netsim::Scenario s;
             add(s, DeviceKind::Host);
             add(s, DeviceKind::Host);
             connect(s, 1, 2);
             return s;
         },
         {{1, "192.168.1.11"}, {2, "192.168.1.10"}}},
        {"12-two-switches", "two switches in a row",
         [] {
             netsim::Scenario s;
             add(s, DeviceKind::Host);
             add(s, DeviceKind::Switch);
             add(s, DeviceKind::Switch);
             add(s, DeviceKind::Host);
             add(s, DeviceKind::Host);
             connect(s, 1, 2);
             connect(s, 2, 3);
             connect(s, 3, 4);
             connect(s, 3, 5);
             return s;
         },
         {{1, "192.168.1.13"}, {5, "192.168.1.10"}, {4, "192.168.1.14"}, {1, "192.168.1.14"}}},
        {"13-duplicate-address", "two PCs with the same address",
         [] {
             auto s = sample();
             const auto twin = add(s, DeviceKind::Host);
             connect(s, twin.value, 3);
             CHECK(edit::configure_host(s, twin, "Twin", "192.168.1.20", 24) == Problem::None);
             return s;
         },
         {{1, "192.168.1.20"}, {1, "192.168.1.20"}}},
        {"14-full-switch", "a full switch",
         [] {
             netsim::Scenario s;
             add(s, DeviceKind::Switch);
             for (std::uint32_t i = 0; i < 8; ++i) connect(s, 1, add(s, DeviceKind::Host).value);
             return s;
         },
         {{2, "192.168.1.18"}, {9, "192.168.1.11"}, {5, "192.168.1.16"}, {3, "192.168.1.16"}}},

        {"15-cable-down-in-flight", "a cable disabled while a frame is on it, enabled again", sample, {},
         [](Driver& run) {
             run.ping(1, "192.168.1.20");
             run.until(15ms);
             run.apply(netsim::SetLinkUp{netsim::LinkId{2}, false});
             run.until(2500ms);
             run.apply(netsim::SetLinkUp{netsim::LinkId{2}, true});
         }},
        {"16-power-off-mid-ping", "the source powered off during a ping of three requests", sample, {},
         [](Driver& run) {
             run.ping(1, "192.168.1.20", {.count = 3});
             run.until(1020ms);
             run.apply(netsim::SetPowered{netsim::DeviceId{1}, false});
             run.until(1500ms);
             run.apply(netsim::SetPowered{netsim::DeviceId{1}, true});
         }},
        {"17-stale-arp", "an address moves to another PC: the stale ARP entry, then its expiry",
         [] { return star(3); }, {},
         [](Driver& run) {
             // PC-2 (.11) learns PC-3 (.12); then .12 moves to PC-4.
             run.ping(2, "192.168.1.12");
             run.until(1s);
             run.apply(netsim::SetAddress{{netsim::DeviceId{3}, 0}, *netsim::Ipv4Prefix::parse("192.168.1.99/24")});
             run.apply(netsim::SetAddress{{netsim::DeviceId{4}, 0}, *netsim::Ipv4Prefix::parse("192.168.1.12/24")});
             run.ping(2, "192.168.1.12");
             run.until(61s);
             run.ping(2, "192.168.1.12");
         }},
        {"18-cable-moved", "a cable moved to another switch port: the stale MAC entry, then its ageing",
         [] {
             auto s = star(2);
             s.devices[0].mac_ageing = 10s;
             return s;
         },
         {},
         [](Driver& run) {
             // PC-3's cable moves from switch port 2 to port 5.
             run.ping(2, "192.168.1.12");
             run.until(1s);
             run.apply(netsim::Replug{netsim::LinkId{2}, {netsim::DeviceId{1}, 1}, {netsim::DeviceId{1}, 4}});
             run.ping(2, "192.168.1.12");
             run.until(15s);
             run.ping(2, "192.168.1.12");
         }},
        {"19-switch-loop", "three switches in a ring: the storm guard",
         [] {
             netsim::Scenario s;
             for (int i = 0; i < 3; ++i) add(s, DeviceKind::Switch);
             connect(s, 1, 2);
             connect(s, 2, 3);
             CHECK(edit::connect(s, netsim::PortRef{netsim::DeviceId{3}, 1}, netsim::PortRef{netsim::DeviceId{1}, 1}) == Problem::None);
             connect(s, 1, add(s, DeviceKind::Host).value);
             connect(s, 2, add(s, DeviceKind::Host).value);
             return s;
         },
         {{4, "192.168.1.14"}}},
        {"20-ping-count", "four requests, one lost on a cable disabled for a moment", sample, {},
         [](Driver& run) {
             run.ping(1, "192.168.1.20", {.count = 4});
             run.until(1015ms);
             run.apply(netsim::SetLinkUp{netsim::LinkId{2}, false});
             run.until(1500ms);
             run.apply(netsim::SetLinkUp{netsim::LinkId{2}, true});
         }},
        {"21-arp-queue-full", "requests for an address nobody has, faster than ARP gives up", sample, {},
         [](Driver& run) { run.ping(1, "192.168.1.99", {.count = 5, .interval = 200ms}); }},
        {"22-reply-resolves-sender", "a rebooted PC resolves the sender before replying", sample, {},
         [](Driver& run) {
             run.ping(1, "192.168.1.20");
             run.until(1s);
             run.apply(netsim::SetPowered{netsim::DeviceId{2}, false});
             run.apply(netsim::SetPowered{netsim::DeviceId{2}, true});
             run.ping(1, "192.168.1.20");
         }},
        {"23-remove-device-in-flight", "the destination removed while the request travels", sample, {},
         [](Driver& run) {
             run.ping(1, "192.168.1.20");
             run.until(15ms);
             run.apply(netsim::RemoveDevice{netsim::DeviceId{2}});
         }},
        {"24-two-lans", "two LANs and a router, each PC with the router as its gateway", routed::two_lans,
         {{1, "10.2.0.20"}, {1, "10.2.0.20"}, {5, "10.1.0.10"}, {1, "10.2.0.1"}}},
        {"25-wrong-gateway", "a gateway address nobody has, then one outside the subnet",
         [] {
             auto s = routed::two_lans();
             s.device(netsim::DeviceId{1})->gateway = netsim::Ipv4Address::parse("10.1.0.254");
             return s;
         },
         {{1, "10.2.0.20"}},
         [](Driver& run) {
             run.ping(1, "10.2.0.20");
             run.finish();
             run.apply(netsim::SetGateway{netsim::DeviceId{1}, netsim::Ipv4Address::parse("10.2.0.1")});
         }},
        {"26-missing-return-route", "the far router has no route back: the request arrives, the reply does not",
         [] { return routed::chain(false); }, {{1, "10.2.0.20"}}},
        {"27-routing-loop", "two routers whose default routes point at each other: the TTL ends the loop",
         routed::routing_loop, {{1, "10.9.9.9"}}},
        {"28-longest-prefix", "overlapping /8, /16 and /24 routes: the longest prefix decides",
         routed::longest_prefix, {{1, "10.9.0.10"}, {1, "10.20.9.10"}, {1, "10.20.30.10"}}},
        {"29-traceroute", "traceroute across two routers, then into a routing loop", routed::routing_loop, {},
         [](Driver& run) {
             run.ping(1, "10.2.0.20", {.count = 30, .traceroute = true});
             run.finish();
             run.ping(1, "10.9.9.9", {.count = 5, .traceroute = true});
         }},
        {"30-unreachable", "no route at the first router, nobody at the address behind the second",
         [] { return routed::chain(); }, {},
         [](Driver& run) {
             run.ping(1, "10.9.9.9", {.timeout = 5s});
             run.finish();
             run.ping(1, "10.2.0.99", {.timeout = 5s});
         }},
    };
}

} // namespace regression

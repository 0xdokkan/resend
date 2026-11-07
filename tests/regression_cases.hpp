#pragma once

// The prototype's behaviour, kept as regression cases: each network and
// its pings were checked against the prototype's engine, record by record
// and text by text, before that engine was removed.
#include "check.hpp"
#include "scenario/edit.hpp"

#include <functional>
#include <string>
#include <vector>

namespace regression {

struct Ping {
    std::uint32_t source;
    std::string destination;
};
struct Case {
    const char* file;  // golden file name
    const char* name;
    std::function<netsim::Scenario()> build;
    std::vector<Ping> pings;  // one after the other, each run to the end
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
    };
}

} // namespace regression

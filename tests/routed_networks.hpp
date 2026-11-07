#pragma once

// Networks with routers, shared by the routing, regression and comparison
// tests. Addresses follow one plan: LAN n is 10.n.0.0/24 with its router at
// .1, and the link between routers m and n is 10.mn.0.0/30.
#include "check.hpp"
#include "scenario/edit.hpp"

#include <initializer_list>
#include <string>

namespace routed {

struct Network {
    netsim::Scenario scenario;

    netsim::DeviceId add(netsim::DeviceKind kind, const std::string& name) {
        netsim::DeviceId id{};
        CHECK(netsim::edit::add_device(scenario, kind, 0, 0, &id) == netsim::Problem::None);
        scenario.device(id)->name = name;
        return id;
    }
    netsim::DeviceId pc(const std::string& name, const char* address, const char* gateway = nullptr) {
        const auto id = add(netsim::DeviceKind::Host, name);
        auto& device = *scenario.device(id);
        device.ports[0].ipv4 = netsim::Ipv4Prefix::parse(address);
        if (gateway) device.gateway = netsim::Ipv4Address::parse(gateway);
        return id;
    }
    // A router with one port per address, "" for a port without one.
    netsim::DeviceId router(const std::string& name, std::initializer_list<const char*> addresses) {
        const auto id = add(netsim::DeviceKind::Router, name);
        auto& device = *scenario.device(id);
        device.ports = netsim::edit::resized_ports(device, addresses.size());
        std::size_t port = 0;
        for (const auto* address : addresses) {
            if (*address) device.ports[port].ipv4 = netsim::Ipv4Prefix::parse(address);
            ++port;
        }
        return id;
    }
    netsim::DeviceId switch_(const std::string& name) { return add(netsim::DeviceKind::Switch, name); }
    void route(netsim::DeviceId router, const char* destination, const char* via) {
        scenario.device(router)->routes.push_back({*netsim::Ipv4Prefix::parse(destination), *netsim::Ipv4Address::parse(via)});
    }
    // Ports count from 1, as on the interface.
    void cable(netsim::DeviceId a, int a_port, netsim::DeviceId b, int b_port) {
        const netsim::PortRef from{a, static_cast<std::uint8_t>(a_port - 1)}, to{b, static_cast<std::uint8_t>(b_port - 1)};
        CHECK(netsim::edit::connect(scenario, from, to) == netsim::Problem::None);
    }
    netsim::Scenario done() const {
        CHECK(netsim::validate(scenario).empty());
        return scenario;
    }
};

// PC-A (1) on Switch-1 (2), Router (3), Switch-2 (4) and PC-B (5):
// 10.1.0.10 and 10.2.0.20, each with the router as its gateway.
inline netsim::Scenario two_lans() {
    Network net;
    const auto a = net.pc("PC-A", "10.1.0.10/24", "10.1.0.1");
    const auto left = net.switch_("Switch-1");
    const auto router = net.router("Router", {"10.1.0.1/24", "10.2.0.1/24"});
    const auto right = net.switch_("Switch-2");
    const auto b = net.pc("PC-B", "10.2.0.20/24", "10.2.0.1");
    net.cable(a, 1, left, 1);
    net.cable(left, 2, router, 1);
    net.cable(router, 2, right, 1);
    net.cable(right, 2, b, 1);
    return net.done();
}

// PC-A (1) — R1 (2) — R2 (3) — PC-B (4): 10.1.0.10, 10.12.0.1 and .2,
// 10.2.0.20. R1 knows the way to 10.2.0.0/24; R2 knows the way back to
// 10.1.0.0/24 only with `return_route`.
inline netsim::Scenario chain(bool return_route = true) {
    Network net;
    const auto a = net.pc("PC-A", "10.1.0.10/24", "10.1.0.1");
    const auto r1 = net.router("R1", {"10.1.0.1/24", "10.12.0.1/30"});
    const auto r2 = net.router("R2", {"10.12.0.2/30", "10.2.0.1/24"});
    const auto b = net.pc("PC-B", "10.2.0.20/24", "10.2.0.1");
    net.cable(a, 1, r1, 1);
    net.cable(r1, 2, r2, 1);
    net.cable(r2, 2, b, 1);
    net.route(r1, "10.2.0.0/24", "10.12.0.2");
    if (return_route) net.route(r2, "10.1.0.0/24", "10.12.0.1");
    return net.done();
}

// The chain, where each router's default route points at the other: a
// datagram for an address neither knows goes round until its TTL runs out.
inline netsim::Scenario routing_loop() {
    auto scenario = chain();
    scenario.device(netsim::DeviceId{2})->routes.push_back({*netsim::Ipv4Prefix::parse("0.0.0.0/0"), *netsim::Ipv4Address::parse("10.12.0.2")});
    scenario.device(netsim::DeviceId{3})->routes.push_back({*netsim::Ipv4Prefix::parse("0.0.0.0/0"), *netsim::Ipv4Address::parse("10.12.0.1")});
    CHECK(netsim::validate(scenario).empty());
    return scenario;
}

// A core router (2) with PC-A (1) on 10.1.0.0/24 and three routers behind
// it: Wide (3) for 10.0.0.0/8, Middle (4) for 10.20.0.0/16 and Narrow (5)
// for 10.20.30.0/24, each with a PC: 10.9.0.10 (6), 10.20.9.10 (7) and
// 10.20.30.10 (8). The three routes overlap; the longest prefix decides.
inline netsim::Scenario longest_prefix() {
    Network net;
    const auto a = net.pc("PC-A", "10.1.0.10/24", "10.1.0.1");
    const auto core = net.router("Core", {"10.1.0.1/24", "172.16.1.1/30", "172.16.2.1/30", "172.16.3.1/30"});
    const auto wide = net.router("Wide", {"172.16.1.2/30", "10.9.0.1/24"});
    const auto middle = net.router("Middle", {"172.16.2.2/30", "10.20.9.1/24"});
    const auto narrow = net.router("Narrow", {"172.16.3.2/30", "10.20.30.1/24"});
    const auto far = net.pc("PC-8", "10.9.0.10/24", "10.9.0.1");
    const auto mid = net.pc("PC-16", "10.20.9.10/24", "10.20.9.1");
    const auto near = net.pc("PC-24", "10.20.30.10/24", "10.20.30.1");
    net.cable(a, 1, core, 1);
    net.cable(core, 2, wide, 1);
    net.cable(core, 3, middle, 1);
    net.cable(core, 4, narrow, 1);
    net.cable(wide, 2, far, 1);
    net.cable(middle, 2, mid, 1);
    net.cable(narrow, 2, near, 1);
    net.route(core, "10.0.0.0/8", "172.16.1.2");
    net.route(core, "10.20.0.0/16", "172.16.2.2");
    net.route(core, "10.20.30.0/24", "172.16.3.2");
    net.route(wide, "0.0.0.0/0", "172.16.1.1");
    net.route(middle, "0.0.0.0/0", "172.16.2.1");
    net.route(narrow, "0.0.0.0/0", "172.16.3.1");
    return net.done();
}

} // namespace routed

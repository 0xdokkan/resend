#pragma once

#include "core/address.hpp"
#include "core/ids.hpp"
#include "core/problem.hpp"
#include "core/sim_time.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace netsim {

// What the user builds, saves and shares: devices, their ports and
// configuration, the cables, and where the cards sit on the canvas.
// The simulation is built from a scenario and never changes it.

enum class DeviceKind : std::uint8_t { Host, Switch, Router };

struct PortRef {
    DeviceId device{};
    std::uint8_t port = 0;  // zero-based; files and the interface count from 1
    auto operator<=>(const PortRef&) const = default;
};

struct PortConfig {
    MacAddress mac;
    bool admin_up = true;
    // The interface address: required on hosts, optional on routers, where a
    // port without one takes no part in IPv4.
    std::optional<Ipv4Prefix> ipv4;
    bool operator==(const PortConfig&) const = default;
};

// A static route of a router: datagrams for `destination` go to
// `next_hop`. 0.0.0.0/0 is the default route. The route is used only while
// the next hop is on the subnet of one of the router's interfaces, as a
// router installs a static route only when it can reach its next hop.
struct Route {
    Ipv4Prefix destination;  // a network: no host bits set
    Ipv4Address next_hop;
    bool operator==(const Route&) const = default;
};

// How long a switch keeps a MAC address it has not seen again: IEEE 802.1Q's
// default and range.
constexpr SimTime default_mac_ageing = std::chrono::seconds(300);
constexpr SimTime min_mac_ageing = std::chrono::seconds(10);
constexpr SimTime max_mac_ageing = std::chrono::seconds(1'000'000);

struct DeviceConfig {
    DeviceId id{};
    DeviceKind kind = DeviceKind::Host;
    std::string name;
    bool powered = true;
    std::vector<PortConfig> ports;
    int x = 0, y = 0;  // top-left corner of the card, in canvas units
    SimTime mac_ageing = default_mac_ageing;  // switches only
    // Hosts only: where datagrams for other subnets go (RFC 1122 §3.3.1).
    std::optional<Ipv4Address> gateway{};
    std::vector<Route> routes{};  // routers only, in the order configured
    bool operator==(const DeviceConfig&) const = default;
};

struct LinkConfig {
    LinkId id{};
    PortRef a, b;
    SimTime delay = std::chrono::milliseconds(10);
    bool up = true;
    bool operator==(const LinkConfig&) const = default;
};

struct Scenario {
    std::string title;
    std::string notes;
    std::uint32_t seed = 1;
    std::vector<DeviceConfig> devices;
    std::vector<LinkConfig> links;

    const DeviceConfig* device(DeviceId id) const;
    DeviceConfig* device(DeviceId id);
    const DeviceConfig* device_named(std::string_view name) const;
    const LinkConfig* link(LinkId id) const;
    LinkConfig* link(LinkId id);
    // The cable plugged into a port, if any.
    const LinkConfig* link_at(PortRef port) const;
    // The port at the other end of the cable plugged into `port`.
    std::optional<PortRef> peer(PortRef port) const;
    DeviceId next_device_id() const;
    LinkId next_link_id() const;
    bool operator==(const Scenario&) const = default;
};

constexpr int host_ports = 1;
constexpr int switch_ports = 8;
// A router has 2 to 8 ports; a new one has 4.
constexpr int min_router_ports = 2;
constexpr int max_router_ports = 8;
constexpr int router_ports = 4;
constexpr std::size_t max_routes = 32;
constexpr std::size_t max_devices = 100;
constexpr SimTime min_delay = std::chrono::microseconds(1);
constexpr SimTime max_delay = std::chrono::seconds(10);
// Card positions, as in scenario files.
constexpr int max_coordinate = 1'000'000;

// The ports a new device has.
int port_count(DeviceKind kind);
bool valid_port_count(DeviceKind kind, std::size_t count);
// The MAC a port gets by default: 02:00:<id> for host ports, so hosts keep
// the addresses of the prototype, 02:1p:<id> for switch port p and
// 02:2p:<id> for router port p.
MacAddress default_mac(DeviceKind kind, DeviceId id, int port);
std::vector<PortConfig> default_ports(DeviceKind kind, DeviceId id);
// A host's default gateway, or nullopt if it is not a unicast address.
Problem check_gateway(const std::optional<Ipv4Address>& gateway);
// A router's routes: each a network with a unicast next hop, no two to the
// same network, at most max_routes.
Problem check_routes(const std::vector<Route>& routes);
// 1 to 48 characters with at least one visible one, no control characters.
bool valid_name(std::string_view name);

struct Issue {
    Problem problem = Problem::None;
    std::string subject;  // the device or cable concerned, as named in the scenario
    bool operator==(const Issue&) const = default;
};
// Everything that makes a scenario impossible to simulate. Valid but
// questionable networks (duplicate addresses, inconsistent masks, Ethernet
// loops) pass: they are worth trying.
std::vector<Issue> validate(const Scenario& scenario);

// The network the app starts with: PC-A and PC-B on Switch-01.
Scenario sample_lan();

} // namespace netsim

#pragma once

#include "scenario/scenario.hpp"

#include <optional>
#include <vector>

namespace netsim {

// One route of a router's table: a connected network, which datagrams
// reach directly, or a static route through a next hop.
struct RouteEntry {
    Ipv4Prefix destination;  // a network: no host bits set
    bool connected = false;
    Ipv4Address next_hop{};  // static routes
    // The port datagrams leave from. Nothing for a static route the router
    // does not use: its next hop is on none of the router's networks, or a
    // connected network has the same destination.
    std::optional<std::uint8_t> port{};
    bool active() const { return port.has_value(); }
    bool operator==(const RouteEntry&) const = default;
};

// A router's routing table, from its configuration: a connected network for
// each port with an address, in port order, then the static routes in the
// order configured. A static route goes through the port whose network
// holds its next hop (the longest match when several do), as routers
// install a static route only when its next hop is directly reachable.
std::vector<RouteEntry> routing_table(const DeviceConfig& router);

// The route for a destination (RFC 1812 §5.2.4.3): of the active routes
// that match it, the one with the longest prefix (RFC 4632); between equal
// prefixes, the first in the table. Nothing if no route matches: without a
// default route (0.0.0.0/0) there may be none.
std::optional<RouteEntry> lookup(const std::vector<RouteEntry>& table, Ipv4Address destination);

// Where a datagram for `destination` goes next from the port of `route`:
// the destination itself on a connected network, the next hop otherwise.
inline Ipv4Address next_hop(const RouteEntry& route, Ipv4Address destination) {
    return route.connected ? destination : route.next_hop;
}

} // namespace netsim

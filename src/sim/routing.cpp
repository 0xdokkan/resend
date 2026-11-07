#include "sim/routing.hpp"

#include <algorithm>
#include <cstddef>

namespace netsim {

std::vector<RouteEntry> routing_table(const DeviceConfig& router) {
    std::vector<RouteEntry> table;
    for (std::size_t port = 0; port < router.ports.size(); ++port)
        if (const auto& address = router.ports[port].ipv4)
            table.push_back({address->canonical(), true, {}, static_cast<std::uint8_t>(port)});
    const auto connected = table.size();
    for (const auto& route : router.routes) {
        RouteEntry entry{route.destination, false, route.next_hop, std::nullopt};
        const auto shadowed = std::any_of(table.begin(), table.begin() + static_cast<std::ptrdiff_t>(connected),
                                          [&](const auto& network) { return network.destination == route.destination; });
        const auto own = std::any_of(router.ports.begin(), router.ports.end(),
                                     [&](const auto& port) { return port.ipv4 && port.ipv4->address() == route.next_hop; });
        if (!shadowed && !own) {
            const RouteEntry* via = nullptr;
            for (std::size_t i = 0; i < connected; ++i)
                if (table[i].destination.contains(route.next_hop) && (!via || table[i].destination.length() > via->destination.length()))
                    via = &table[i];
            if (via) entry.port = via->port;
        }
        table.push_back(entry);
    }
    return table;
}

std::optional<RouteEntry> lookup(const std::vector<RouteEntry>& table, Ipv4Address destination) {
    const RouteEntry* best = nullptr;
    for (const auto& route : table)
        if (route.active() && route.destination.contains(destination) && (!best || route.destination.length() > best->destination.length()))
            best = &route;
    return best ? std::optional(*best) : std::nullopt;
}

} // namespace netsim

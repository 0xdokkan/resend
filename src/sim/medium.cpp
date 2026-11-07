#include "sim/medium.hpp"

#include <utility>

namespace netsim {

Medium::Medium(const Scenario& scenario) {
    const auto enabled = [&](PortRef port) {
        const auto* device = scenario.device(port.device);
        return device && port.port < device->ports.size() && device->ports[port.port].admin_up;
    };
    for (const auto& cable : scenario.links) {
        up_[cable.id] = cable.up;
        const bool ports = enabled(cable.a) && enabled(cable.b);
        for (const auto& [from, to] : {std::pair{cable.a, cable.b}, std::pair{cable.b, cable.a}}) {
            const auto* sender = scenario.device(from.device);
            ends_[from] = {{cable.id, to, cable.delay}, cable.up && ports && sender && sender->powered};
        }
    }
}

std::optional<Medium::Path> Medium::path(PortRef port) const {
    const auto it = ends_.find(port);
    if (it == ends_.end() || !it->second.carrier) return std::nullopt;
    return it->second.path;
}

bool Medium::up(LinkId link) const {
    const auto it = up_.find(link);
    return it != up_.end() && it->second;
}

} // namespace netsim

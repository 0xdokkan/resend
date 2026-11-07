#include "sim/medium.hpp"

#include <utility>

namespace netsim {

void Medium::update(const Scenario& scenario) {
    const auto enabled = [&](PortRef port) {
        const auto* device = scenario.device(port.device);
        return device && port.port < device->ports.size() && device->ports[port.port].admin_up;
    };
    std::map<LinkId, Cable> cables;
    ends_.clear();
    for (const auto& link : scenario.links) {
        Cable cable{link.a, link.b, link.up && enabled(link.a) && enabled(link.b), 0};
        const auto old = cables_.find(link.id);
        // The same cable, still carrying frames or only starting to.
        const bool same = old != cables_.end() && old->second.a == cable.a && old->second.b == cable.b &&
            (!old->second.usable || cable.usable);
        cable.serial = same ? old->second.serial : ++last_serial_;
        cables.emplace(link.id, cable);
        for (const auto& [from, to] : {std::pair{link.a, link.b}, std::pair{link.b, link.a}}) {
            const auto* sender = scenario.device(from.device);
            ends_[from] = {{link.id, to, link.delay, cable.serial}, cable.usable && sender && sender->powered};
        }
    }
    cables_ = std::move(cables);
}

std::optional<Medium::Path> Medium::path(PortRef port) const {
    const auto it = ends_.find(port);
    if (it == ends_.end() || !it->second.carrier) return std::nullopt;
    return it->second.path;
}

bool Medium::intact(LinkId link, std::uint64_t serial) const {
    const auto it = cables_.find(link);
    return it != cables_.end() && it->second.usable && it->second.serial == serial;
}

} // namespace netsim

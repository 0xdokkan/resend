#include "scenario/edit.hpp"

#include <algorithm>
#include <set>
#include <vector>

namespace netsim::edit {

std::optional<std::uint8_t> free_port(const Scenario& scenario, DeviceId device) {
    const auto* item = scenario.device(device);
    if (!item) return std::nullopt;
    for (std::size_t port = 0; port < item->ports.size(); ++port) {
        const PortRef ref{device, static_cast<std::uint8_t>(port)};
        if (!scenario.link_at(ref)) return ref.port;
    }
    return std::nullopt;
}

bool joined(const Scenario& scenario, DeviceId a, DeviceId b) {
    std::vector<DeviceId> visit{a};
    std::set<DeviceId> seen{a};
    for (std::size_t i = 0; i < visit.size(); ++i) {
        if (visit[i] == b) return true;
        for (const auto& cable : scenario.links) {
            const auto next = cable.a.device == visit[i] ? std::optional(cable.b.device)
                : cable.b.device == visit[i] ? std::optional(cable.a.device) : std::nullopt;
            if (next && seen.insert(*next).second) visit.push_back(*next);
        }
    }
    return false;
}

Problem add_device(Scenario& scenario, DeviceKind kind, int x, int y, DeviceId* created) {
    if (scenario.devices.size() >= max_devices) return Problem::TooManyDevices;
    const auto id = scenario.next_device_id();
    DeviceConfig device{id, kind, (kind == DeviceKind::Host ? "PC-" : "Switch-") + std::to_string(id.value), true,
                        default_ports(kind, id), x, y};
    if (kind == DeviceKind::Host) {
        // 192.168.<third>.<fourth>, counting from 192.168.1.10.
        const auto third = 1 + ((id.value - 1) / 240) % 254;
        const auto fourth = 10 + (id.value - 1) % 240;
        device.ports[0].ipv4 = Ipv4Prefix(Ipv4Address(0xc0a80000U | third << 8 | fourth), 24);
    }
    scenario.devices.push_back(std::move(device));
    if (created) *created = id;
    return Problem::None;
}

Problem connect(Scenario& scenario, PortRef a, PortRef b, LinkId* created) {
    const auto* first = scenario.device(a.device);
    const auto* second = scenario.device(b.device);
    if (!first || !second) return Problem::NoSuchDevice;
    if (a.device == b.device) return Problem::SameDevice;
    if (a.port >= first->ports.size() || b.port >= second->ports.size()) return Problem::NoSuchPort;
    if (scenario.link_at(a) || scenario.link_at(b)) return Problem::PortInUse;
    if (joined(scenario, a.device, b.device)) return Problem::LoopNotSupported;
    const auto id = scenario.next_link_id();
    scenario.links.push_back({id, a, b});
    if (created) *created = id;
    return Problem::None;
}

Problem connect(Scenario& scenario, DeviceId a, DeviceId b, LinkId* created) {
    if (!scenario.device(a) || !scenario.device(b)) return Problem::NoSuchDevice;
    if (a == b) return Problem::SameDevice;
    if (joined(scenario, a, b)) return Problem::LoopNotSupported;
    const auto pa = free_port(scenario, a), pb = free_port(scenario, b);
    if (!pa || !pb) return Problem::NoFreePort;
    return connect(scenario, PortRef{a, *pa}, PortRef{b, *pb}, created);
}

Problem remove_device(Scenario& scenario, DeviceId id) {
    if (!scenario.device(id)) return Problem::NoSuchDevice;
    std::erase_if(scenario.devices, [id](const auto& item) { return item.id == id; });
    std::erase_if(scenario.links, [id](const auto& cable) { return cable.a.device == id || cable.b.device == id; });
    return Problem::None;
}

Problem remove_link(Scenario& scenario, LinkId id) {
    if (!scenario.link(id)) return Problem::NoSuchLink;
    std::erase_if(scenario.links, [id](const auto& cable) { return cable.id == id; });
    return Problem::None;
}

Problem configure_host(Scenario& scenario, DeviceId id, std::string_view name, std::string_view address, int prefix) {
    auto* device = scenario.device(id);
    if (!device || device->kind != DeviceKind::Host) return Problem::NotAHost;
    if (!valid_name(name)) return Problem::InvalidName;
    if (prefix < 1 || prefix > 32) return Problem::InvalidPrefix;
    const auto parsed = Ipv4Address::parse(address);
    if (!parsed || !Ipv4Prefix(*parsed, prefix).usable_host()) return Problem::InvalidHostAddress;
    device->name = name;
    device->ports[0].ipv4 = Ipv4Prefix(*parsed, prefix);
    return Problem::None;
}

Problem rename(Scenario& scenario, DeviceId id, std::string_view name) {
    auto* device = scenario.device(id);
    if (!device) return Problem::NoSuchDevice;
    if (!valid_name(name)) return Problem::InvalidName;
    device->name = name;
    return Problem::None;
}

Problem set_link_up(Scenario& scenario, LinkId id, bool up) {
    auto* cable = scenario.link(id);
    if (!cable) return Problem::NoSuchLink;
    cable->up = up;
    return Problem::None;
}

Problem set_powered(Scenario& scenario, DeviceId id, bool powered) {
    auto* device = scenario.device(id);
    if (!device) return Problem::NoSuchDevice;
    device->powered = powered;
    return Problem::None;
}

Problem move(Scenario& scenario, DeviceId id, int x, int y) {
    auto* device = scenario.device(id);
    if (!device) return Problem::NoSuchDevice;
    device->x = x;
    device->y = y;
    return Problem::None;
}

} // namespace netsim::edit

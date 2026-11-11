#include "scenario/edit.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace netsim {

bool affects_network(const Edit& edit) {
    return !std::holds_alternative<Rename>(edit) && !std::holds_alternative<MoveDevice>(edit);
}

} // namespace netsim

namespace netsim::edit {
namespace {
template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

bool valid_port(const Scenario& scenario, PortRef port) {
    const auto* device = scenario.device(port.device);
    return device && port.port < device->ports.size();
}

Problem check_delay(SimTime delay) { return delay < min_delay || delay > max_delay ? Problem::InvalidDelay : Problem::None; }

Problem apply_one(Scenario& scenario, const SetLinkUp& edit) {
    auto* cable = scenario.link(edit.link);
    if (!cable) return Problem::NoSuchLink;
    cable->up = edit.up;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetDelay& edit) {
    auto* cable = scenario.link(edit.link);
    if (!cable) return Problem::NoSuchLink;
    if (const auto problem = check_delay(edit.delay); !ok(problem)) return problem;
    cable->delay = edit.delay;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetPortUp& edit) {
    auto* device = scenario.device(edit.port.device);
    if (!device) return Problem::NoSuchDevice;
    if (edit.port.port >= device->ports.size()) return Problem::NoSuchPort;
    device->ports[edit.port.port].admin_up = edit.up;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetPowered& edit) {
    auto* device = scenario.device(edit.device);
    if (!device) return Problem::NoSuchDevice;
    device->powered = edit.powered;
    return Problem::None;
}

Problem check_address(const std::optional<Ipv4Prefix>& address) {
    if (!address) return Problem::None;
    if (address->length() < 1) return Problem::InvalidPrefix;
    return address->usable_host() ? Problem::None : Problem::InvalidHostAddress;
}

Problem apply_one(Scenario& scenario, const SetAddress& edit) {
    auto* device = scenario.device(edit.port.device);
    if (!device) return Problem::NotAHost;
    if (device->kind == DeviceKind::Switch) return Problem::AddressOnSwitch;
    if (edit.port.port >= device->ports.size()) return Problem::NoSuchPort;
    if (const auto problem = check_address(edit.address); !ok(problem)) return problem;
    device->ports[edit.port.port].ipv4 = edit.address;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetGateway& edit) {
    auto* device = scenario.device(edit.device);
    if (!device || device->kind != DeviceKind::Host) return Problem::NotAHost;
    if (const auto problem = check_gateway(edit.gateway); !ok(problem)) return problem;
    device->gateway = edit.gateway;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetRoutes& edit) {
    auto* device = scenario.device(edit.device);
    if (!device || device->kind != DeviceKind::Router) return Problem::NotARouter;
    if (const auto problem = check_routes(edit.routes); !ok(problem)) return problem;
    device->routes = edit.routes;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetPorts& edit) {
    auto* device = scenario.device(edit.device);
    if (!device || device->kind != DeviceKind::Router) return Problem::NotARouter;
    if (!valid_port_count(device->kind, edit.ports.size())) return Problem::WrongPortCount;
    for (auto port = edit.ports.size(); port < device->ports.size(); ++port)
        if (scenario.link_at({edit.device, static_cast<std::uint8_t>(port)})) return Problem::PortInUse;
    for (const auto& port : edit.ports)
        if (const auto problem = check_address(port.ipv4); !ok(problem)) return problem;
    device->ports = edit.ports;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const SetAgeing& edit) {
    auto* device = scenario.device(edit.device);
    if (!device || device->kind != DeviceKind::Switch) return Problem::NotASwitch;
    if (edit.ageing < min_mac_ageing || edit.ageing > max_mac_ageing) return Problem::InvalidAgeing;
    device->mac_ageing = edit.ageing;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const Rename& edit) {
    auto* device = scenario.device(edit.device);
    if (!device) return Problem::NoSuchDevice;
    if (!valid_name(edit.name)) return Problem::InvalidName;
    device->name = edit.name;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const MoveDevice& edit) {
    auto* device = scenario.device(edit.device);
    if (!device) return Problem::NoSuchDevice;
    device->x = std::clamp(edit.x, -max_coordinate, max_coordinate);
    device->y = std::clamp(edit.y, -max_coordinate, max_coordinate);
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const Connect& edit) {
    if (!scenario.device(edit.a.device) || !scenario.device(edit.b.device)) return Problem::NoSuchDevice;
    if (edit.a.device == edit.b.device) return Problem::SameDevice;
    if (!valid_port(scenario, edit.a) || !valid_port(scenario, edit.b)) return Problem::NoSuchPort;
    if (scenario.link_at(edit.a) || scenario.link_at(edit.b)) return Problem::PortInUse;
    if (const auto problem = check_delay(edit.delay); !ok(problem)) return problem;
    const auto id = edit.id.value_or(scenario.next_link_id());
    if (id.value == 0) return Problem::InvalidLinkId;
    if (scenario.link(id)) return Problem::DuplicateLinkId;
    scenario.links.push_back({id, edit.a, edit.b, edit.delay});
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const Disconnect& edit) {
    if (!scenario.link(edit.link)) return Problem::NoSuchLink;
    std::erase_if(scenario.links, [&](const auto& cable) { return cable.id == edit.link; });
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const Replug& edit) {
    auto* cable = scenario.link(edit.link);
    if (!cable) return Problem::NoSuchLink;
    if (cable->a != edit.from && cable->b != edit.from) return Problem::NoSuchPort;
    if (!scenario.device(edit.to.device)) return Problem::NoSuchDevice;
    if (!valid_port(scenario, edit.to)) return Problem::NoSuchPort;
    if (edit.to == edit.from) return Problem::None;
    if (scenario.link_at(edit.to)) return Problem::PortInUse;
    auto& moved = cable->a == edit.from ? cable->a : cable->b;
    const auto& other = cable->a == edit.from ? cable->b : cable->a;
    if (other.device == edit.to.device) return Problem::SameDevice;
    moved = edit.to;
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const AddDevice& edit) {
    if (scenario.devices.size() >= max_devices) return Problem::TooManyDevices;
    const auto id = edit.id.value_or(scenario.next_device_id());
    if (id.value == 0) return Problem::InvalidDeviceId;
    if (scenario.device(id)) return Problem::DuplicateDeviceId;
    const char* prefix = edit.kind == DeviceKind::Host ? "PC-" : edit.kind == DeviceKind::Switch ? "Switch-" : "Router-";
    DeviceConfig device{id, edit.kind, prefix + std::to_string(id.value), true,
                        default_ports(edit.kind, id), std::clamp(edit.x, -max_coordinate, max_coordinate),
                        std::clamp(edit.y, -max_coordinate, max_coordinate)};
    if (edit.kind == DeviceKind::Host) {
        // 192.168.<third>.<fourth>, counting from 192.168.1.10.
        const auto third = 1 + ((id.value - 1) / 240) % 254;
        const auto fourth = 10 + (id.value - 1) % 240;
        device.ports[0].ipv4 = Ipv4Prefix(Ipv4Address(0xc0a80000U | third << 8 | fourth), 24);
    }
    scenario.devices.push_back(std::move(device));
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const RemoveDevice& edit) {
    if (!scenario.device(edit.device)) return Problem::NoSuchDevice;
    std::erase_if(scenario.devices, [&](const auto& item) { return item.id == edit.device; });
    std::erase_if(scenario.links, [&](const auto& cable) { return cable.a.device == edit.device || cable.b.device == edit.device; });
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const RestoreLink& edit) {
    const auto& cable = edit.link;
    if (!scenario.device(cable.a.device) || !scenario.device(cable.b.device)) return Problem::NoSuchDevice;
    if (cable.a.device == cable.b.device) return Problem::SameDevice;
    if (!valid_port(scenario, cable.a) || !valid_port(scenario, cable.b)) return Problem::NoSuchPort;
    if (scenario.link_at(cable.a) || scenario.link_at(cable.b)) return Problem::PortInUse;
    if (const auto problem = check_delay(cable.delay); !ok(problem)) return problem;
    if (cable.id.value == 0) return Problem::InvalidLinkId;
    if (scenario.link(cable.id)) return Problem::DuplicateLinkId;
    const auto index = std::min(edit.index, scenario.links.size());
    scenario.links.insert(scenario.links.begin() + static_cast<std::ptrdiff_t>(index), cable);
    return Problem::None;
}

Problem apply_one(Scenario& scenario, const RestoreDevice& edit) {
    if (scenario.devices.size() >= max_devices) return Problem::TooManyDevices;
    if (edit.device.id.value == 0) return Problem::InvalidDeviceId;
    if (scenario.device(edit.device.id)) return Problem::DuplicateDeviceId;
    // The device and its cables come back together or not at all.
    Scenario next = scenario;
    auto device = edit.device;
    device.x = std::clamp(device.x, -max_coordinate, max_coordinate);
    device.y = std::clamp(device.y, -max_coordinate, max_coordinate);
    const auto index = std::min(edit.index, next.devices.size());
    next.devices.insert(next.devices.begin() + static_cast<std::ptrdiff_t>(index), std::move(device));
    if (const auto issues = validate(next); !issues.empty()) return issues.front().problem;
    for (const auto& cable : edit.links) {
        if (cable.link.a.device != edit.device.id && cable.link.b.device != edit.device.id) return Problem::NoSuchLink;
        if (const auto problem = apply_one(next, cable); !ok(problem)) return problem;
    }
    scenario = std::move(next);
    return Problem::None;
}

template<class T>
std::size_t index_of(const std::vector<T>& items, const T* item) {
    return static_cast<std::size_t>(item - items.data());
}

} // namespace

Problem apply(Scenario& scenario, const Edit& edit) {
    return std::visit([&](const auto& item) { return apply_one(scenario, item); }, edit);
}

std::optional<Edit> inverse(const Scenario& before, const Edit& edit) {
    Scenario trial = before;
    if (!ok(apply(trial, edit))) return std::nullopt;
    const auto device = [&](DeviceId id) { return before.device(id); };
    return std::visit(Overloaded{
        [&](const SetLinkUp& item) -> std::optional<Edit> { return SetLinkUp{item.link, before.link(item.link)->up}; },
        [&](const SetDelay& item) -> std::optional<Edit> { return SetDelay{item.link, before.link(item.link)->delay}; },
        [&](const SetPortUp& item) -> std::optional<Edit> {
            return SetPortUp{item.port, device(item.port.device)->ports[item.port.port].admin_up};
        },
        [&](const SetPowered& item) -> std::optional<Edit> { return SetPowered{item.device, device(item.device)->powered}; },
        [&](const SetAddress& item) -> std::optional<Edit> {
            return SetAddress{item.port, device(item.port.device)->ports[item.port.port].ipv4};
        },
        [&](const SetAgeing& item) -> std::optional<Edit> { return SetAgeing{item.device, device(item.device)->mac_ageing}; },
        [&](const Rename& item) -> std::optional<Edit> { return Rename{item.device, device(item.device)->name}; },
        [&](const MoveDevice& item) -> std::optional<Edit> {
            const auto* old = device(item.device);
            return MoveDevice{item.device, old->x, old->y};
        },
        [&](const Connect& item) -> std::optional<Edit> {
            if (!item.id) return std::nullopt;
            return Disconnect{*item.id};
        },
        [&](const Disconnect& item) -> std::optional<Edit> {
            const auto* cable = before.link(item.link);
            return RestoreLink{*cable, index_of(before.links, cable)};
        },
        [&](const Replug& item) -> std::optional<Edit> { return Replug{item.link, item.to, item.from}; },
        [&](const AddDevice& item) -> std::optional<Edit> {
            if (!item.id) return std::nullopt;
            return RemoveDevice{*item.id};
        },
        [&](const RemoveDevice& item) -> std::optional<Edit> {
            const auto* old = device(item.device);
            RestoreDevice restore{*old, index_of(before.devices, old)};
            for (std::size_t index = 0; index < before.links.size(); ++index) {
                const auto& cable = before.links[index];
                if (cable.a.device == item.device || cable.b.device == item.device) restore.links.push_back({cable, index});
            }
            return restore;
        },
        [&](const RestoreLink& item) -> std::optional<Edit> { return Disconnect{item.link.id}; },
        [&](const RestoreDevice& item) -> std::optional<Edit> { return RemoveDevice{item.device.id}; },
        [&](const SetGateway& item) -> std::optional<Edit> { return SetGateway{item.device, device(item.device)->gateway}; },
        [&](const SetRoutes& item) -> std::optional<Edit> { return SetRoutes{item.device, device(item.device)->routes}; },
        [&](const SetPorts& item) -> std::optional<Edit> { return SetPorts{item.device, device(item.device)->ports}; },
    }, edit);
}

std::vector<PortConfig> resized_ports(const DeviceConfig& device, std::size_t count) {
    auto ports = device.ports;
    ports.resize(std::min(ports.size(), count));
    while (ports.size() < count) ports.push_back({default_mac(device.kind, device.id, static_cast<int>(ports.size())), true, std::nullopt});
    return ports;
}

std::optional<std::uint8_t> free_port(const Scenario& scenario, DeviceId device) {
    const auto* item = scenario.device(device);
    if (!item) return std::nullopt;
    for (std::size_t port = 0; port < item->ports.size(); ++port) {
        const PortRef ref{device, static_cast<std::uint8_t>(port)};
        if (!scenario.link_at(ref)) return ref.port;
    }
    return std::nullopt;
}

Problem add_device(Scenario& scenario, DeviceKind kind, int x, int y, DeviceId* created) {
    const auto problem = apply(scenario, AddDevice{kind, x, y});
    if (ok(problem) && created) *created = scenario.devices.back().id;
    return problem;
}

Problem connect(Scenario& scenario, PortRef a, PortRef b, LinkId* created) {
    const auto problem = apply(scenario, Connect{a, b});
    if (ok(problem) && created) *created = scenario.links.back().id;
    return problem;
}

std::optional<Connect> first_free_ports(const Scenario& scenario, DeviceId a, DeviceId b, Problem& problem) {
    const auto pa = free_port(scenario, a), pb = free_port(scenario, b);
    problem = !scenario.device(a) || !scenario.device(b) ? Problem::NoSuchDevice
        : a == b ? Problem::SameDevice
        : !pa || !pb ? Problem::NoFreePort
        : Problem::None;
    if (!ok(problem)) return std::nullopt;
    return Connect{PortRef{a, *pa}, PortRef{b, *pb}};
}

Problem connect(Scenario& scenario, DeviceId a, DeviceId b, LinkId* created) {
    Problem problem = Problem::None;
    const auto cable = first_free_ports(scenario, a, b, problem);
    return cable ? connect(scenario, cable->a, cable->b, created) : problem;
}

Problem remove_device(Scenario& scenario, DeviceId id) { return apply(scenario, RemoveDevice{id}); }
Problem remove_link(Scenario& scenario, LinkId id) { return apply(scenario, Disconnect{id}); }

std::optional<SetAddress> host_address(const Scenario& scenario, DeviceId id, std::string_view address, int prefix, Problem& problem) {
    const auto* device = scenario.device(id);
    const auto parsed = Ipv4Address::parse(address);
    problem = !device || device->kind != DeviceKind::Host ? Problem::NotAHost : Problem::None;
    if (ok(problem) && address.empty()) return SetAddress{PortRef{id, 0}, std::nullopt};  // no address at all
    problem = !ok(problem) ? problem
        : prefix < 1 || prefix > 32 ? Problem::InvalidPrefix
        : !parsed || !Ipv4Prefix(*parsed, prefix).usable_host() ? Problem::InvalidHostAddress
        : Problem::None;
    if (!ok(problem)) return std::nullopt;
    return SetAddress{PortRef{id, 0}, Ipv4Prefix(*parsed, prefix)};
}

Problem configure_host(Scenario& scenario, DeviceId id, std::string_view name, std::string_view address, int prefix) {
    const auto* device = scenario.device(id);
    if (!device || device->kind != DeviceKind::Host) return Problem::NotAHost;
    if (!valid_name(name)) return Problem::InvalidName;
    Problem problem = Problem::None;
    const auto change = host_address(scenario, id, address, prefix, problem);
    if (!change) return problem;
    apply(scenario, Rename{id, std::string(name)});
    return apply(scenario, *change);
}

Problem rename(Scenario& scenario, DeviceId id, std::string_view name) { return apply(scenario, Rename{id, std::string(name)}); }
Problem set_link_up(Scenario& scenario, LinkId id, bool up) { return apply(scenario, SetLinkUp{id, up}); }
Problem set_powered(Scenario& scenario, DeviceId id, bool powered) { return apply(scenario, SetPowered{id, powered}); }
Problem move(Scenario& scenario, DeviceId id, int x, int y) { return apply(scenario, MoveDevice{id, x, y}); }

} // namespace netsim::edit

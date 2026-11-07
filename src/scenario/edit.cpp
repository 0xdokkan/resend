#include "scenario/edit.hpp"

#include <algorithm>
#include <vector>

namespace netsim {

bool affects_network(const Edit& edit) {
    return !std::holds_alternative<Rename>(edit) && !std::holds_alternative<MoveDevice>(edit);
}

} // namespace netsim

namespace netsim::edit {
namespace {

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

Problem apply_one(Scenario& scenario, const SetAddress& edit) {
    auto* device = scenario.device(edit.port.device);
    if (!device || device->kind != DeviceKind::Host) return Problem::NotAHost;
    if (edit.port.port >= device->ports.size()) return Problem::NoSuchPort;
    if (edit.address.length() < 1) return Problem::InvalidPrefix;
    if (!edit.address.usable_host()) return Problem::InvalidHostAddress;
    device->ports[edit.port.port].ipv4 = edit.address;
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
    DeviceConfig device{id, edit.kind, (edit.kind == DeviceKind::Host ? "PC-" : "Switch-") + std::to_string(id.value), true,
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

} // namespace

Problem apply(Scenario& scenario, const Edit& edit) {
    return std::visit([&](const auto& item) { return apply_one(scenario, item); }, edit);
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
    problem = !device || device->kind != DeviceKind::Host ? Problem::NotAHost
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

#include "scenario/scenario.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace netsim {
namespace {
template<class Items, class Key>
auto find_by_id(Items& items, Key id) -> decltype(&items.front()) {
    auto it = std::find_if(items.begin(), items.end(), [id](const auto& item) { return item.id == id; });
    return it == items.end() ? nullptr : &*it;
}

std::string port_text(const Scenario& scenario, PortRef port) {
    const auto* device = scenario.device(port.device);
    return (device ? device->name : "device " + std::to_string(port.device.value)) + " port " + std::to_string(port.port + 1);
}
} // namespace

const DeviceConfig* Scenario::device(DeviceId id) const { return find_by_id(devices, id); }
DeviceConfig* Scenario::device(DeviceId id) { return find_by_id(devices, id); }
const LinkConfig* Scenario::link(LinkId id) const { return find_by_id(links, id); }
LinkConfig* Scenario::link(LinkId id) { return find_by_id(links, id); }

const DeviceConfig* Scenario::device_named(std::string_view name) const {
    auto it = std::find_if(devices.begin(), devices.end(), [name](const auto& item) { return item.name == name; });
    return it == devices.end() ? nullptr : &*it;
}

const LinkConfig* Scenario::link_at(PortRef port) const {
    auto it = std::find_if(links.begin(), links.end(), [port](const auto& item) { return item.a == port || item.b == port; });
    return it == links.end() ? nullptr : &*it;
}

std::optional<PortRef> Scenario::peer(PortRef port) const {
    const auto* cable = link_at(port);
    if (!cable) return std::nullopt;
    return cable->a == port ? cable->b : cable->a;
}

DeviceId Scenario::next_device_id() const {
    std::uint32_t last = 0;
    for (const auto& item : devices) last = std::max(last, item.id.value);
    return DeviceId{last + 1};
}

LinkId Scenario::next_link_id() const {
    std::uint32_t last = 0;
    for (const auto& item : links) last = std::max(last, item.id.value);
    return LinkId{last + 1};
}

int port_count(DeviceKind kind) { return kind == DeviceKind::Host ? host_ports : switch_ports; }

MacAddress default_mac(DeviceKind kind, DeviceId id, int port) {
    if (kind == DeviceKind::Host) return MacAddress::local(id.value);
    const auto v = id.value;
    return MacAddress(MacAddress::Bytes{0x02, static_cast<std::uint8_t>(0x10 + port), static_cast<std::uint8_t>(v >> 24),
                                       static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
                                       static_cast<std::uint8_t>(v)});
}

std::vector<PortConfig> default_ports(DeviceKind kind, DeviceId id) {
    std::vector<PortConfig> ports;
    for (int port = 0; port < port_count(kind); ++port) ports.push_back({default_mac(kind, id, port), true, std::nullopt});
    return ports;
}

bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 48 &&
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c > 32; }) &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::vector<Issue> validate(const Scenario& scenario) {
    std::vector<Issue> issues;
    const auto report = [&](Problem problem, std::string subject) { issues.push_back({problem, std::move(subject)}); };

    if (scenario.devices.size() > max_devices) report(Problem::TooManyDevices, std::to_string(scenario.devices.size()));
    std::set<DeviceId> device_ids;
    for (const auto& device : scenario.devices) {
        const auto& who = device.name;
        if (device.id.value == 0) report(Problem::InvalidDeviceId, who);
        else if (!device_ids.insert(device.id).second) report(Problem::DuplicateDeviceId, who);
        if (!valid_name(device.name)) report(Problem::InvalidName, "device " + std::to_string(device.id.value));
        if (static_cast<int>(device.ports.size()) != port_count(device.kind)) {
            report(Problem::WrongPortCount, who);
            continue;
        }
        for (const auto& port : device.ports) {
            if (device.kind == DeviceKind::Switch && port.ipv4) report(Problem::AddressOnSwitch, who);
            if (device.kind == DeviceKind::Host && !port.ipv4) report(Problem::MissingAddress, who);
            if (device.kind == DeviceKind::Host && port.ipv4 && !port.ipv4->usable_host())
                report(Problem::InvalidHostAddress, who + " " + port.ipv4->to_string());
        }
    }

    std::set<LinkId> link_ids;
    std::set<PortRef> used;
    for (const auto& cable : scenario.links) {
        const auto who = port_text(scenario, cable.a) + " — " + port_text(scenario, cable.b);
        if (cable.id.value == 0) report(Problem::InvalidLinkId, who);
        else if (!link_ids.insert(cable.id).second) report(Problem::DuplicateLinkId, who);
        if (cable.delay < min_delay || cable.delay > max_delay) report(Problem::InvalidDelay, who);
        bool ends_valid = true;
        for (const auto& end : {cable.a, cable.b}) {
            const auto* device = scenario.device(end.device);
            if (!device) {
                report(Problem::NoSuchDevice, who);
                ends_valid = false;
            } else if (end.port >= device->ports.size()) {
                report(Problem::NoSuchPort, who);
                ends_valid = false;
            } else if (!used.insert(end).second) {
                report(Problem::PortInUse, port_text(scenario, end));
                ends_valid = false;
            }
        }
        if (ends_valid && cable.a.device == cable.b.device) report(Problem::SameDevice, who);
    }

    // Until storm protection exists, every cable must join two separate
    // groups of devices: a union-find over the cables in file order.
    if (issues.empty()) {
        std::map<DeviceId, DeviceId> parent;
        const auto root = [&](DeviceId id) {
            while (parent.contains(id) && parent[id] != id) id = parent[id];
            return id;
        };
        for (const auto& device : scenario.devices) parent[device.id] = device.id;
        for (const auto& cable : scenario.links) {
            const auto a = root(cable.a.device), b = root(cable.b.device);
            if (a == b) report(Problem::LoopNotSupported, port_text(scenario, cable.a) + " — " + port_text(scenario, cable.b));
            else parent[a] = b;
        }
    }
    return issues;
}

Scenario sample_lan() {
    Scenario scenario;
    scenario.title = "Sample LAN";
    scenario.notes = "Two PCs on one switch. Ping PC-B from PC-A: ARP finds PC-B's MAC address first, "
                     "then the ICMP echo crosses the switch and comes back.";
    const auto host = [&](std::uint32_t id, const char* name, const char* address, int x, int y) {
        DeviceConfig device{DeviceId{id}, DeviceKind::Host, name, true, default_ports(DeviceKind::Host, DeviceId{id}), x, y};
        device.ports[0].ipv4 = Ipv4Prefix::parse(address);
        scenario.devices.push_back(std::move(device));
    };
    host(1, "PC-A", "192.168.1.10/24", 100, 130);
    host(2, "PC-B", "192.168.1.20/24", 520, 130);
    scenario.devices.push_back({DeviceId{3}, DeviceKind::Switch, "Switch-01", true, default_ports(DeviceKind::Switch, DeviceId{3}), 310, 280});
    scenario.links.push_back({LinkId{1}, {DeviceId{1}, 0}, {DeviceId{3}, 0}});
    scenario.links.push_back({LinkId{2}, {DeviceId{2}, 0}, {DeviceId{3}, 1}});
    return scenario;
}

} // namespace netsim

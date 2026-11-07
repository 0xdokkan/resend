#pragma once

#include "scenario/scenario.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace netsim {

// Changes to a network, as values. edit::apply() makes them on a scenario;
// Simulation::apply() makes them on a running network at the current time
// and records them.
struct SetLinkUp {
    LinkId link{};
    bool up = true;
    bool operator==(const SetLinkUp&) const = default;
};
struct SetDelay {
    LinkId link{};
    SimTime delay{};
    bool operator==(const SetDelay&) const = default;
};
struct SetPortUp {
    PortRef port;
    bool up = true;
    bool operator==(const SetPortUp&) const = default;
};
struct SetPowered {
    DeviceId device{};
    bool powered = true;
    bool operator==(const SetPowered&) const = default;
};
struct SetAddress {
    PortRef port;
    Ipv4Prefix address;
    bool operator==(const SetAddress&) const = default;
};
struct SetAgeing {
    DeviceId device{};
    SimTime ageing{};
    bool operator==(const SetAgeing&) const = default;
};
struct Rename {
    DeviceId device{};
    std::string name;
    bool operator==(const Rename&) const = default;
};
// Positions are clamped to the canvas range.
struct MoveDevice {
    DeviceId device{};
    int x = 0, y = 0;
    bool operator==(const MoveDevice&) const = default;
};
// A new cable. Without an id it gets the next free one; a running
// simulation never gives out the id of a cable it removed.
struct Connect {
    PortRef a, b;
    SimTime delay = std::chrono::milliseconds(10);
    std::optional<LinkId> id{};
    bool operator==(const Connect&) const = default;
};
struct Disconnect {
    LinkId link{};
    bool operator==(const Disconnect&) const = default;
};
// Moves one end of a cable from port `from` to port `to`.
struct Replug {
    LinkId link{};
    PortRef from, to;
    bool operator==(const Replug&) const = default;
};
// A device with its defaults: a host gets 192.168.x.y/24 derived from its
// id (192.168.1.10 for id 1), names are "PC-<id>" and "Switch-<id>".
// Without an id it gets the next free one; a running simulation never gives
// out the id of a device it removed, so its records keep their meaning.
struct AddDevice {
    DeviceKind kind = DeviceKind::Host;
    int x = 0, y = 0;
    std::optional<DeviceId> id{};
    bool operator==(const AddDevice&) const = default;
};
// Removes the device and its cables.
struct RemoveDevice {
    DeviceId device{};
    bool operator==(const RemoveDevice&) const = default;
};
// Puts back a cable as it was, at `index` in the scenario's list: the
// inverse of Disconnect, which undo uses.
struct RestoreLink {
    LinkConfig link;
    std::size_t index = 0;
    bool operator==(const RestoreLink&) const = default;
};
// Puts back a device as it was, at `index` in the scenario's list, with the
// cables that were plugged into it, in increasing index order: the inverse
// of RemoveDevice.
struct RestoreDevice {
    DeviceConfig device;
    std::size_t index = 0;
    std::vector<RestoreLink> links{};
    bool operator==(const RestoreDevice&) const = default;
};

using Edit = std::variant<SetLinkUp, SetDelay, SetPortUp, SetPowered, SetAddress, SetAgeing, Rename, MoveDevice, Connect,
                          Disconnect, Replug, AddDevice, RemoveDevice, RestoreLink, RestoreDevice>;

// Whether the edit changes how the network behaves. Names and positions do
// not: a running simulation takes them without recording anything.
bool affects_network(const Edit& edit);

} // namespace netsim

// Editing operations with the interface's rules. Each returns
// Problem::None on success and leaves the scenario unchanged otherwise.
namespace netsim::edit {

Problem apply(Scenario& scenario, const Edit& edit);

// The edit that takes the scenario back to `before` once `edit` is made on
// it, exactly, down to the order of devices and cables; nothing if `edit`
// would be refused. New devices and cables must carry their ids, as
// Simulation::resolve() gives them.
std::optional<Edit> inverse(const Scenario& before, const Edit& edit);

// Shorthands for the edits above, as the interface and the tests use them.
Problem add_device(Scenario& scenario, DeviceKind kind, int x, int y, DeviceId* created = nullptr);
// On the first free port of each device.
Problem connect(Scenario& scenario, DeviceId a, DeviceId b, LinkId* created = nullptr);
Problem connect(Scenario& scenario, PortRef a, PortRef b, LinkId* created = nullptr);
Problem remove_device(Scenario& scenario, DeviceId id);
Problem remove_link(Scenario& scenario, LinkId id);
// Name and address together: both are checked before either changes.
Problem configure_host(Scenario& scenario, DeviceId id, std::string_view name, std::string_view address, int prefix);
Problem rename(Scenario& scenario, DeviceId id, std::string_view name);
Problem set_link_up(Scenario& scenario, LinkId id, bool up);
Problem set_powered(Scenario& scenario, DeviceId id, bool powered);
Problem move(Scenario& scenario, DeviceId id, int x, int y);

// The address change configure_host() makes, after the same checks.
std::optional<SetAddress> host_address(const Scenario& scenario, DeviceId id, std::string_view address, int prefix, Problem& problem);
// The cable connect() plugs between two devices, on their first free ports.
std::optional<Connect> first_free_ports(const Scenario& scenario, DeviceId a, DeviceId b, Problem& problem);
// The first port of `device` without a cable.
std::optional<std::uint8_t> free_port(const Scenario& scenario, DeviceId device);

} // namespace netsim::edit

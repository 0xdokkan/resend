#pragma once

#include "scenario/scenario.hpp"

#include <optional>
#include <string_view>

// Editing operations with the interface's rules. Each returns
// Problem::None on success and leaves the scenario unchanged otherwise.
namespace netsim::edit {

// A host gets 192.168.x.y/24 derived from its id (192.168.1.10 for id 1),
// a switch eight ports; names are "PC-<id>" and "Switch-<id>".
Problem add_device(Scenario& scenario, DeviceKind kind, int x, int y, DeviceId* created = nullptr);
// On the first free port of each device.
Problem connect(Scenario& scenario, DeviceId a, DeviceId b, LinkId* created = nullptr);
Problem connect(Scenario& scenario, PortRef a, PortRef b, LinkId* created = nullptr);
Problem remove_device(Scenario& scenario, DeviceId id);
Problem remove_link(Scenario& scenario, LinkId id);
Problem configure_host(Scenario& scenario, DeviceId id, std::string_view name, std::string_view address, int prefix);
Problem rename(Scenario& scenario, DeviceId id, std::string_view name);
Problem set_link_up(Scenario& scenario, LinkId id, bool up);
Problem set_powered(Scenario& scenario, DeviceId id, bool powered);
Problem move(Scenario& scenario, DeviceId id, int x, int y);

// The first port of `device` without a cable.
std::optional<std::uint8_t> free_port(const Scenario& scenario, DeviceId device);
// Whether a path of cables, up or down, joins the two devices.
bool joined(const Scenario& scenario, DeviceId a, DeviceId b);

} // namespace netsim::edit

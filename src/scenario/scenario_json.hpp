#pragma once

#include "scenario/scenario.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace netsim {

// Scenario files: JSON with "format": "resend-scenario" and a version.
// Ports are numbered from 1, as in the interface; delays are durations
// such as "10ms". docs/scenario-format.md describes every member.
// Version 2 added routers, default gateways and routes; version 3 hosts
// without an address. A file is written with the lowest version that holds
// its network, so older versions of resend still read what they can.
constexpr int scenario_format_version = 3;

namespace json {
class Value;
}

std::string scenario_to_json(const Scenario& scenario);
// Reads a file, or explains the first problem: the line and column of a
// syntax error, or the path of the member that is wrong. The network itself
// is checked separately by validate().
std::optional<Scenario> scenario_from_json(std::string_view text, std::string& error);

// The same members inside another file, such as the networks of a lab: an
// object with "seed", "devices" and "links" (and "title" and "notes" if
// wanted), read as a scenario file of `version` would be. Errors name the
// members from `path`, e.g. "network.devices[0].name".
std::optional<Scenario> scenario_from_value(const json::Value& object, int version, const std::string& path, std::string& error);
// Without "format" and "version" unless `with_header`.
json::Value scenario_to_value(const Scenario& scenario, bool with_header = false);

} // namespace netsim

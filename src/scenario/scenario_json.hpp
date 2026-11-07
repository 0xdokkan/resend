#pragma once

#include "scenario/scenario.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace netsim {

// Scenario files: JSON with "format": "resend-scenario" and a version.
// Ports are numbered from 1, as in the interface; delays are durations
// such as "10ms". docs/scenario-format.md describes every member.
constexpr int scenario_format_version = 1;

std::string scenario_to_json(const Scenario& scenario);
// Reads a file, or explains the first problem: the line and column of a
// syntax error, or the path of the member that is wrong. The network itself
// is checked separately by validate().
std::optional<Scenario> scenario_from_json(std::string_view text, std::string& error);

} // namespace netsim

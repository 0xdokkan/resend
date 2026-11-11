#pragma once

#include "scenario/edit.hpp"
#include "scenario/scenario.hpp"
#include "sim/simulation.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The device terminal (M8): a subset of the Linux tools resend is compared
// with, `ip`, `bridge`, `ping` and `traceroute`, with resend's port names
// (p1, p2, ...). What a learner types here works on a Linux machine whose
// interfaces have those names. A command that changes the network gives the
// same edits as the inspector; queries print the state at the instant shown;
// a command or an option outside the subset is refused with a message, never
// ignored. No Slint here: the app keeps the sessions and shows them.
namespace netsim::term {

// An application a command starts: a ping or a traceroute from the host.
struct Start {
    Ipv4Address destination;
    PingOptions options;
    // ping without -c: Linux pings until stopped, resend sends this many.
    bool default_count = false;
    bool operator==(const Start&) const = default;
};

struct Result {
    // Printed at once: a query's answer, an error, help. Lines end in '\n'.
    std::string output;
    bool error = false;
    // A change, as the inspector would make it, and its label for undo.
    std::vector<Edit> edits;
    std::string label;
    std::optional<Start> start;
    // `clear`: the session's screen is emptied.
    bool clear = false;
    bool operator==(const Result&) const = default;
};

// Runs one command line typed on `device`. `network` is the network at the
// instant shown and `simulation` its run, for the tables; without one the
// tables are empty.
Result run(std::string_view line, DeviceId device, const Scenario& network, const Simulation* simulation);

// The words of a command line, as the shell splits them, with "sudo" in
// front dropped: whoever has a terminal here is root.
std::vector<std::string> words(std::string_view line);

// The same command spelled in full, for comparing what two learners typed:
// "ip a" and "sudo ip address show" are both "ip address show". Nothing if
// the line is not a command resend knows.
std::optional<std::string> canonical(std::string_view line);

// What ping and traceroute print for `ping` so far: a line per request or
// hop as the simulation plays, then the statistics once it is over.
std::string application_output(const Simulation& simulation, const PingStatus& ping, bool default_count = false);

// Tab completion: the line with the common start of the candidates for its
// last word, and the candidates when there is more than one.
struct Completion {
    std::string line;
    std::vector<std::string> candidates;
    bool operator==(const Completion&) const = default;
};
Completion complete(std::string_view line, DeviceId device, const Scenario& network);

// The menus of the terminal: a family of commands (ip, ping, bridge) and
// what can be run of it on this device. A command with nothing to fill in
// runs when chosen; one with a placeholder in capitals (ADDRESS, PORT) is
// written on the line for the learner to finish, the cursor on the first.
struct MenuEntry {
    std::string command;
    std::string text;  // what it does, in a line
    bool complete = true;
    bool operator==(const MenuEntry&) const = default;
};
struct Menu {
    std::string family;
    std::vector<MenuEntry> entries;
    bool operator==(const Menu&) const = default;
};
std::vector<Menu> menus(DeviceId device, const Scenario& network);

// help, or help COMMAND.
std::string help(std::string_view command = {});

// The prompt: root@NAME:~#
std::string prompt(const DeviceConfig& device);

// The commands that make the configuration of `before`'s devices that of
// `after`'s, matched by name, device by device in `after`'s order: the
// addresses, the ports enabled or disabled, a host's default gateway and a
// router's static routes. Cables, power and names are not the terminal's.
struct DeviceCommands {
    std::string device;
    std::vector<std::string> commands;
    bool operator==(const DeviceCommands&) const = default;
};
std::vector<DeviceCommands> commands_between(const Scenario& before, const Scenario& after);

// "p1" for port 0.
std::string port_name(std::uint8_t port);
std::optional<std::uint8_t> port_index(std::string_view name, const DeviceConfig& device);

} // namespace netsim::term

#pragma once

#include "core/address.hpp"
#include "core/problem.hpp"
#include "scenario/edit.hpp"
#include "scenario/scenario.hpp"
#include "sim/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Guided labs: a network with a task. A lab says what to build or repair
// and which goals must hold; the goals are checked on the simulation, never
// scripted. No text and no Slint here: explain/ turns verdicts into words,
// the app shows them. docs/lab-format.md describes the files.
namespace netsim::lab {

// Lab files: JSON with "format": "resend-lab" and a version.
constexpr int lab_format_version = 1;

// After the way a first course teaches: steps to follow; a goal and an empty
// or half-built network; a broken network and a goal, nothing else.
enum class Kind : std::uint8_t { Guided, Challenge, Troubleshooting };

// What of a device a lock protects, or a `kept` goal compares: flags.
enum Setting : std::uint8_t {
    Name = 1,
    Power = 2,
    Addresses = 4,   // the IPv4 address of each port
    Gateway = 8,
    Routes = 16,
    Ports = 32,      // how many ports, and whether each is enabled
    Cables = 64,     // the cables plugged into it, and their state and delay
};
using Settings = std::uint8_t;
constexpr Settings all_settings = Name | Power | Addresses | Gateway | Routes | Ports | Cables;
// In the order files and texts list them.
constexpr Setting setting_list[] = {Name, Power, Addresses, Gateway, Routes, Ports, Cables};

// A device of the lab's network, by name, and what of it is meant.
struct DeviceSettings {
    std::string device;
    Settings settings = all_settings;
    bool operator==(const DeviceSettings&) const = default;
};

enum class GoalKind : std::uint8_t { Reach, Path, Config, Observed, Answer, Kept, Command };

// The predicates of a `config` goal.
enum class ConfigCheck : std::uint8_t {
    AddressIn,  // a port's address is inside `prefix`, with its length
    Gateway,    // the host's default gateway is `address`
    Route,      // the router has a static route to `prefix`, via `address` if given
    PortUp,     // port `port` is enabled (or disabled, with `up` false)
    Cable,      // a cable joins `device` and `other`
};

// The records an `observed` goal looks for in the learner's own run.
enum class Observation : std::uint8_t {
    ArpRequest,  // `device` sent an ARP request, for `address` if given
    ArpReply,    // `device` sent an ARP reply
    Routed,      // the router `device` routed a datagram
    EchoReply,   // a ping from `device` got an Echo Reply
    MacLearned,  // the switch `device` learned a MAC address
};

// What an `answer` goal reads from a probe's trace.
enum class Quantity : std::uint8_t {
    ReplyTtl,     // the TTL of the Echo Reply when it reaches `from`
    Hops,         // how many hops a traceroute from `from` to `to` counts, the destination included
    FirstHopMac,  // the destination MAC of the Echo Request as it leaves `from`
};

struct Goal {
    std::string id;    // unique in the lab; answers refer to it
    GoalKind kind = GoalKind::Reach;
    std::string text;  // what the learner reads: the goal in a sentence
    // reach, path, answer: a probe from the host `from` to `to`.
    std::string from;
    Ipv4Address to;
    // reach: nothing for a reply, or the reason the ping must fail for.
    std::optional<Reason> expect;
    // path: the addresses that answer, hop by hop, the destination last.
    std::vector<Ipv4Address> hops;
    // config, observed: the device concerned.
    std::string device;
    ConfigCheck check = ConfigCheck::AddressIn;
    Ipv4Prefix prefix;
    std::optional<Ipv4Address> address;
    std::uint8_t port = 0;  // zero-based
    bool up = true;
    std::string other;
    Observation observation = Observation::ArpRequest;
    Quantity quantity = Quantity::ReplyTtl;
    // kept: the devices and what of them stays as the lab gave it.
    std::vector<DeviceSettings> kept;
    // command: what `device`'s terminal must run, e.g. "ip neigh"; the
    // same command typed in any of its Linux spellings, or with more after
    // it, meets the goal (src/term/terminal.hpp, canonical()).
    std::string command;
    bool operator==(const Goal&) const = default;
};

// A command the solution runs on its network, so that `observed` goals see
// their records: a ping, or a traceroute, from a host.
struct Command {
    std::string from;
    Ipv4Address to;
    bool traceroute = false;
    bool operator==(const Command&) const = default;
};

struct Solution {
    Scenario network;
    std::vector<Command> commands;
    std::map<std::string, std::string> answers;  // goal id to answer
    bool operator==(const Solution&) const = default;
};

// How the devices a learner adds start: as the lab's devices did, possibly
// as they come out of their boxes.
struct NewDevices {
    bool host_address = true;     // false: new hosts have no address
    bool router_ports_up = true;  // false: new routers' ports are disabled
    bool operator==(const NewDevices&) const = default;
};

struct Lab {
    std::string id;      // the file name without .json
    Kind kind = Kind::Guided;
    std::string topic;   // the library's section, e.g. "First networks"
    std::string title, brief;
    Scenario network;    // to start from; may be empty
    // The names goals use for devices the learner has to add.
    std::vector<std::string> names;
    NewDevices new_devices;
    std::vector<DeviceSettings> locks;
    std::vector<Goal> goals;
    std::vector<std::string> steps, hints;
    Solution solution;
    bool operator==(const Lab&) const = default;
};

// Reads a lab file, or explains the first problem as scenario files do,
// with the path of the member. Then checked by validate().
std::optional<Lab> lab_from_json(std::string_view text, std::string& error);
std::string lab_to_json(const Lab& lab);

// What makes a lab unusable: its networks invalid, a goal or a lock naming
// a device that neither the network nor `names` has, duplicate goal ids,
// answers to goals that ask none, commands from devices that are not hosts.
// Each issue is a short English sentence for authors, not for learners.
std::vector<std::string> validate(const Lab& lab);

// Why a goal is met or not.
enum class Finding : std::uint8_t {
    Met,
    NoDevice,      // the network has no device named `subject`
    NotAHost,      // `subject` is not a host, so it cannot ping
    CannotStart,   // the probe could not start: `problem`
    NoReply,       // reach, path: the ping failed for `reason`
    Replied,       // reach: a reply came, though the goal expects a failure
    OtherFailure,  // reach: it failed, but for `reason`, not the one expected
    WrongPath,     // path: other hops answered (`hops`)
    Different,     // config: `actual` instead of `expected`
    NotSeen,       // observed: no such record in the run
    NoAnswer,      // answer: nothing answered yet
    WrongAnswer,   // answer: `actual` is not the value read
    Unreadable,    // answer: the probe could not read it, the ping failing for `reason`
    Changed,       // kept: `setting` of `subject` is not as the lab gave it
    NotRun,        // command: `subject`'s terminal has not run it
};

struct Verdict {
    std::size_t goal = 0;  // index in the lab's goals
    bool met = false;
    Finding finding = Finding::Met;
    Reason reason = Reason::None;
    Problem problem = Problem::None;
    std::string subject;  // a device's name
    Setting setting = Name;
    std::string expected, actual;
    std::vector<Ipv4Address> hops;
    // The records that decided it: from the probe for reach, path and
    // answer (the ping's loss or reply and any datagram or frame stopped
    // for a problem), from the learner's run for observed.
    // Their devices are those of the network checked.
    std::vector<TraceRecord> evidence;
    bool operator==(const Verdict&) const = default;
};

// The learner's answers, by goal id; for a command goal, the line typed in
// the device's terminal that met it.
using Answers = std::map<std::string, std::string>;

// Whether `typed` is the command a goal asks for, or that command with more
// after it: "ip n show dev p1" is "ip neigh".
bool command_matches(std::string_view goal, std::string_view typed);

// Checks every goal on `network`, the learner's current one. Probes start a
// new simulation of it, from time zero with empty tables: the same network
// always gets the same verdict. `run`, the learner's own simulation, is read
// by `observed` goals only; without one they are not met.
std::vector<Verdict> check(const Lab& lab, const Scenario& network, const Simulation* run, const Answers& answers);
Verdict check(const Lab& lab, std::size_t goal, const Scenario& network, const Simulation* run, const Answers& answers);
bool all_met(const std::vector<Verdict>& verdicts);

// Runs the solution's commands on a simulation of its network to the end.
Simulation solution_run(const Lab& lab);
// The solution's network configured as the learner starts: the lab's own
// devices with their addresses, ports, gateway and routes as the lab gives
// them, the devices the learner adds as they come out of their boxes. What
// is left between it and the solution is the terminal's to do (M8.5).
Scenario starting_configuration(const Lab& lab);
// A lab proves itself: valid, at least one goal unmet as given, every goal
// met with its solution, which keeps the locks. Problems as validate()'s.
std::vector<std::string> verify(const Lab& lab);

// The locks that stop `edit` on `before`, the network it would change:
// nothing if it may go ahead. Positions are never locked.
std::optional<DeviceSettings> locked(const Lab& lab, const Scenario& before, const Edit& edit);

// Whether `device` in `network` has `settings` as the lab's network has them,
// and the first that differs.
std::optional<Setting> changed(const Lab& lab, const Scenario& network, const DeviceSettings& device);

// The edits that make a device a learner just added start as the lab says:
// a host without its address, a router with its ports disabled.
std::vector<Edit> new_device_edits(const Lab& lab, const DeviceConfig& device);

// Names for files and texts: "guided", "addresses", "reply-ttl"...
std::string_view name(Kind kind);
std::string_view name(Setting setting);
std::optional<Reason> reason_named(std::string_view text);

} // namespace netsim::lab

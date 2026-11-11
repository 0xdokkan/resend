// The words for a lab's verdicts and locks: why a goal is met or not, said
// from the records that decided it.
#include "core/text.hpp"
#include "explain/explain.hpp"
#include "lab/lab.hpp"

#include <algorithm>

namespace netsim::explain {
namespace {
using lab::Finding;
using lab::Goal;
using lab::GoalKind;
using text::tr;

std::string name_of(const Scenario& network, DeviceId id) {
    const auto* device = network.device(id);
    return device ? device->name : tr("device {0}", {std::to_string(id.value)});
}

// Why a probe's ping failed, from the first record where something stopped
// for a reason worth telling, or else from the ping's own reason.
std::string failure(const Scenario& network, const lab::Verdict& verdict, const std::string& from, Ipv4Address to) {
    for (const auto& record : verdict.evidence) {
        const auto at = name_of(network, record.device);
        const auto* device = network.device(record.device);
        const bool host = device && device->kind == DeviceKind::Host;
        const auto address = Ipv4Address(record.value).to_string();
        // A host that cannot answer notes the request it received as dropped.
        const bool reply_stuck = record.kind == TraceKind::FrameDropped && host &&
                                 (record.reason == Reason::NoRoute || record.reason == Reason::GatewayOffLink);
        if (record.kind == TraceKind::DatagramDropped || reply_stuck) {
            switch (record.reason) {
            case Reason::NoRoute:
                return host ? tr("the request arrives, but {0} cannot send its reply back: {0} has no default gateway.", {at})
                            : tr("{0} has no route to {1}.", {at, address});
            case Reason::GatewayOffLink:
                return tr("the request arrives, but {0} cannot send its reply back: its default gateway is not on its subnet.", {at});
            case Reason::ArpUnresolved:
                return host ? tr("the request arrives, but {0} gets no ARP answer for the next hop of its reply.", {at})
                            : tr("{0} finds no device with {1} on its network: no answer to its ARP requests.", {at, address});
            case Reason::TtlExceeded: return tr("the datagram's TTL runs out at {0}: the routers send it round a loop.", {at});
            case Reason::NoAddress: return tr("{0} has a port without an IPv4 address on the way.", {at});
            case Reason::DirectedBroadcast: return tr("{0} does not forward to a network's broadcast address.", {at});
            default: return tr("the datagram stops at {0}: {1}.", {at, describe(record.reason)});
            }
        }
        if (record.kind == TraceKind::FrameDropped) {
            switch (record.reason) {
            case Reason::NoCarrier:
                return tr("{0} cannot send on port {1}: no cable, or the cable or a port at either end is disabled.",
                          {at, std::to_string(record.port.value_or(0) + 1)});
            case Reason::LinkDown: return tr("a cable of {0} stopped carrying while the frame was on it.", {at});
            case Reason::DeviceDown: return tr("{0} is off.", {at});
            case Reason::NoAddress: return tr("{0} receives the frame on a port without an IPv4 address.", {at});
            default: break;
            }
        }
        if (record.kind == TraceKind::SimulatorGuard)
            return tr("frames go round a loop of switches, and the simulator stopped them at {0}.", {at});
    }
    const auto* source = network.device_named(from);
    switch (verdict.reason) {
    case Reason::NoRoute: return tr("{0}'s request has no route: {0} has no default gateway.", {from});
    case Reason::GatewayOffLink:
        return tr("{0}'s default gateway {1} is not on its subnet.", {from, source && source->gateway ? source->gateway->to_string() : "?"});
    case Reason::NoAddress: return tr("{0} has no IPv4 address.", {from});
    case Reason::ArpUnresolved: {
        const auto& address = source && !source->ports.empty() ? source->ports.front().ipv4 : std::nullopt;
        const bool local = address && address->contains(to);
        const auto next_hop = local || !source || !source->gateway ? to.to_string() : source->gateway->to_string();
        return tr("{0} gets no answer to its ARP requests for {1}: nobody on its subnet has that address, or a cable, a port or the "
                  "device is down.", {from, next_hop});
    }
    case Reason::EchoTimeout: return tr("{0} does not answer: the request or the reply is lost on the way.", {to.to_string()});
    case Reason::DeviceDown: return tr("{0} is off.", {from});
    default: return describe(verdict.reason) + ".";
    }
}

std::string observation_text(const Goal& goal) {
    switch (goal.observation) {
    case lab::Observation::ArpRequest:
        return goal.address ? tr("{0} has not asked for {1} with ARP yet: make it send something there.", {goal.device, goal.address->to_string()})
                            : tr("{0} has not sent an ARP request yet: start a ping from it.", {goal.device});
    case lab::Observation::ArpReply: return tr("{0} has not answered an ARP request yet.", {goal.device});
    case lab::Observation::Routed: return tr("{0} has not routed a datagram yet: start a ping that crosses it.", {goal.device});
    case lab::Observation::EchoReply: return tr("No ping from {0} has had a reply yet.", {goal.device});
    case lab::Observation::MacLearned: return tr("{0} has not learned a MAC address yet: send a frame through it.", {goal.device});
    }
    return {};
}

std::string different(const Goal& goal, const lab::Verdict& verdict) {
    switch (goal.check) {
    case lab::ConfigCheck::AddressIn:
        return verdict.actual.empty() ? tr("{0} has no IPv4 address in {1} yet.", {goal.device, verdict.expected})
                                      : tr("{0} has {1}, which is not an address in {2}.", {goal.device, verdict.actual, verdict.expected});
    case lab::ConfigCheck::Gateway:
        return verdict.actual.empty() ? tr("{0} has no default gateway; it should be {1}.", {goal.device, verdict.expected})
                                      : tr("{0}'s default gateway is {1}; it should be {2}.", {goal.device, verdict.actual, verdict.expected});
    case lab::ConfigCheck::Route:
        return verdict.actual.empty() ? tr("{0} has no static route to {1}.", {goal.device, goal.prefix.to_string()})
                                      : tr("{0}'s route is {1}, not {2}.", {goal.device, verdict.actual, verdict.expected});
    case lab::ConfigCheck::PortUp:
        return goal.up ? tr("Port {0} of {1} is disabled.", {std::to_string(goal.port + 1), goal.device})
                       : tr("Port {0} of {1} is still enabled.", {std::to_string(goal.port + 1), goal.device});
    case lab::ConfigCheck::Cable: return tr("No cable joins {0} and {1}.", {goal.device, goal.other});
    }
    return {};
}
} // namespace

std::string setting_text(lab::Setting setting) {
    switch (setting) {
    case lab::Name: return tr("name");
    case lab::Power: return tr("power");
    case lab::Addresses: return tr("addresses");
    case lab::Gateway: return tr("default gateway");
    case lab::Routes: return tr("routes");
    case lab::Ports: return tr("ports");
    case lab::Cables: return tr("cables");
    }
    return {};
}

std::string settings_text(lab::Settings settings) {
    std::vector<std::string> names;
    for (const auto setting : lab::setting_list)
        if (settings & setting) names.push_back(setting_text(setting));
    if (settings == lab::all_settings || names.empty()) return tr("everything");
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i)
        out += (i == 0 ? "" : i + 1 == names.size() ? tr(" and ") : ", ") + names[i];
    return out;
}

std::string lock_text(const lab::DeviceSettings& lock) {
    if (lock.settings == lab::all_settings) return tr("The lab keeps {0} as it is: it cannot be changed.", {lock.device});
    return tr("The lab keeps {0} as it is: its {1} cannot be changed.", {lock.device, settings_text(lock.settings)});
}

std::string kind_text(lab::Kind kind) {
    switch (kind) {
    case lab::Kind::Guided: return tr("Guided");
    case lab::Kind::Challenge: return tr("Challenge");
    case lab::Kind::Troubleshooting: return tr("Troubleshooting");
    }
    return {};
}

std::string verdict_text(const lab::Lab& lab, const lab::Verdict& verdict, const Scenario& network) {
    const auto& goal = lab.goals.at(verdict.goal);
    const auto to = goal.to.to_string();
    switch (verdict.finding) {
    case Finding::Met:
        switch (goal.kind) {
        case GoalKind::Reach:
            return goal.expect ? tr("{0}'s ping to {1} fails, as it should: {2}", {goal.from, to, failure(network, verdict, goal.from, goal.to)})
                               : tr("{0} reaches {1}.", {goal.from, to});
        case GoalKind::Path: return tr("The traceroute from {0} goes through {1}.", {goal.from, verdict.actual});
        case GoalKind::Answer: return tr("Right.");
        case GoalKind::Command: return tr("{0} ran {1}.", {goal.device, goal.command});
        default: return tr("Done.");
        }
    case Finding::NoDevice: return tr("There is no device named {0} yet.", {verdict.subject});
    case Finding::NotAHost: return tr("{0} is not a PC, so it cannot ping.", {verdict.subject});
    case Finding::CannotStart: return tr("The ping from {0} cannot start: {1}", {goal.from, describe(verdict.problem)});
    case Finding::NoReply:
        return tr("{0} does not reach {1}: {2}", {goal.from, to, failure(network, verdict, goal.from, goal.to)});
    case Finding::Replied: return tr("{0} still reaches {1}: the goal is to find what stops it.", {goal.from, to});
    case Finding::OtherFailure:
        return tr("{0}'s ping to {1} fails, but not as the goal asks: {2}", {goal.from, to, failure(network, verdict, goal.from, goal.to)});
    case Finding::WrongPath: return tr("The traceroute from {0} goes through {1}, not {2}.", {goal.from, verdict.actual, verdict.expected});
    case Finding::Different: return different(goal, verdict);
    case Finding::NotSeen: return observation_text(goal);
    case Finding::NoAnswer: return tr("Write your answer.");
    case Finding::WrongAnswer: return tr("{0} is not right.", {verdict.actual});
    case Finding::Unreadable:
        return tr("This cannot be read yet: the ping from {0} to {1} fails: {2}", {goal.from, to, failure(network, verdict, goal.from, goal.to)});
    case Finding::Changed:
        return tr("{0}'s {1} is not as the lab gave it.", {verdict.subject, setting_text(verdict.setting)});
    case Finding::NotRun: return tr("Run {0} in {1}'s terminal (Ctrl+T).", {goal.command, goal.device});
    }
    return {};
}

} // namespace netsim::explain

// Goals checked on the simulation: probes on a new simulation of the
// learner's network, the learner's own run for what was observed, and the
// locks and kept settings against the network the lab gave.
#include "lab/lab.hpp"

#include "term/terminal.hpp"

#include <algorithm>
#include <cctype>
#include <tuple>
#include <variant>

namespace netsim::lab {
namespace {

// A probe is one ping or traceroute and the network's answer to it; this
// bounds a network that never stops sending (a loop of switches).
constexpr std::size_t probe_events = 200'000;
// After the ping ends, the network goes on for as long as a router keeps
// asking with ARP, so that the records say where its requests stopped.
constexpr SimTime probe_tail = arp_retry_interval * (arp_attempts + 1);
// The solution's commands and what follows them, likewise.
constexpr std::size_t run_events = 1'000'000;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

struct Probe {
    Simulation simulation;
    std::optional<PingId> id;
    Problem problem = Problem::None;
    const PingStatus* status() const { return id ? simulation.ping(*id) : nullptr; }
};

Probe probe(const Scenario& network, DeviceId from, Ipv4Address to, bool traceroute) {
    Probe result{Simulation(network), std::nullopt, Problem::None};
    PingOptions options;
    if (traceroute) {
        options.traceroute = true;
        options.count = default_max_hops;
    }
    result.id = result.simulation.start_ping(from, to, result.problem, options);
    if (!result.id) return result;
    auto& simulation = result.simulation;
    std::size_t events = 0;
    for (; events < probe_events && result.status()->state == PingState::Running; ++events)
        if (!simulation.step()) break;
    const auto end = simulation.now() + probe_tail;
    for (; events < probe_events && simulation.next_event_time() && *simulation.next_event_time() <= end; ++events) simulation.step();
    return result;
}

// Frames and datagrams that stopped for something worth telling: not those
// that were simply for someone else.
bool telling(Reason reason) {
    return reason != Reason::None && reason != Reason::NotForUs && reason != Reason::ArpForOtherHost && reason != Reason::Ipv4NotForUs;
}

// What decided a probe: where datagrams stopped, then how the ping ended.
// The requests of a traceroute die at each router on purpose: those are not
// evidence of anything wrong.
std::vector<TraceRecord> evidence_of(const Probe& probe) {
    std::vector<TraceRecord> evidence;
    const bool traceroute = probe.status() && probe.status()->options.traceroute;
    for (const auto& record : probe.simulation.trace().records()) {
        const bool stopped = (record.kind == TraceKind::DatagramDropped || record.kind == TraceKind::FrameDropped) && telling(record.reason) &&
                             !(traceroute && record.reason == Reason::TtlExceeded);
        const bool outcome = record.kind == TraceKind::PingLost || record.kind == TraceKind::PingFailed ||
                             record.kind == TraceKind::PingSucceeded || record.kind == TraceKind::SimulatorGuard;
        if (stopped || outcome) evidence.push_back(record);
    }
    return evidence;
}

std::vector<Ipv4Address> hops_of(const PingStatus& status) {
    std::vector<Ipv4Address> hops;
    for (const auto& echo : status.echoes) hops.push_back(echo.responder);
    return hops;
}

// The host a goal starts from, or why there is none.
const DeviceConfig* source(const Scenario& network, const std::string& name, Verdict& verdict) {
    const auto* device = network.device_named(name);
    verdict.subject = name;
    if (!device) verdict.finding = Finding::NoDevice;
    else if (device->kind != DeviceKind::Host) verdict.finding = Finding::NotAHost;
    else return device;
    return nullptr;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Answers compare as what they mean: a MAC in any case or with dashes, a
// number without leading zeros.
bool same_answer(const std::string& given, const std::string& expected) {
    auto a = lowered(given), b = lowered(expected);
    if (a == b) return true;
    std::replace(a.begin(), a.end(), '-', ':');
    const auto mac_a = MacAddress::parse(a), mac_b = MacAddress::parse(b);
    if (mac_a && mac_b) return *mac_a == *mac_b;
    const auto digits = [](const std::string& text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    if (digits(a) && digits(b)) {
        a.erase(0, std::min(a.find_first_not_of('0'), a.size() - 1));
        b.erase(0, std::min(b.find_first_not_of('0'), b.size() - 1));
        return a == b;
    }
    return false;
}

bool sends_from(const DeviceConfig& device, const EthernetFrame& frame) {
    return std::any_of(device.ports.begin(), device.ports.end(), [&](const PortConfig& port) { return port.mac == frame.src; });
}

void check_reach(const Goal& goal, const Scenario& network, Verdict& verdict) {
    const auto* host = source(network, goal.from, verdict);
    if (!host) return;
    const auto result = probe(network, host->id, goal.to, false);
    if (!result.id) {
        verdict.finding = Finding::CannotStart;
        verdict.problem = result.problem;
        return;
    }
    verdict.evidence = evidence_of(result);
    const auto& status = *result.status();
    const bool replied = status.state == PingState::Succeeded;
    verdict.reason = replied ? Reason::None : status.state == PingState::Running ? Reason::EchoTimeout : status.reason;
    if (!goal.expect) verdict.finding = replied ? Finding::Met : Finding::NoReply;
    else if (replied) verdict.finding = Finding::Replied;
    else verdict.finding = verdict.reason == *goal.expect ? Finding::Met : Finding::OtherFailure;
}

void check_path(const Goal& goal, const Scenario& network, Verdict& verdict) {
    const auto* host = source(network, goal.from, verdict);
    if (!host) return;
    const auto result = probe(network, host->id, goal.to, true);
    if (!result.id) {
        verdict.finding = Finding::CannotStart;
        verdict.problem = result.problem;
        return;
    }
    verdict.evidence = evidence_of(result);
    const auto& status = *result.status();
    verdict.hops = hops_of(status);
    if (status.state != PingState::Succeeded) {
        verdict.finding = Finding::NoReply;
        verdict.reason = status.state == PingState::Running ? Reason::EchoTimeout : status.reason;
    } else {
        verdict.finding = verdict.hops == goal.hops ? Finding::Met : Finding::WrongPath;
    }
    for (const auto hop : goal.hops) verdict.expected += (verdict.expected.empty() ? "" : " ") + hop.to_string();
    for (const auto hop : verdict.hops) verdict.actual += (verdict.actual.empty() ? "" : " ") + (hop == Ipv4Address{} ? "*" : hop.to_string());
}

void check_config(const Goal& goal, const Scenario& network, Verdict& verdict) {
    const auto* device = network.device_named(goal.device);
    verdict.subject = goal.device;
    if (!device) {
        verdict.finding = Finding::NoDevice;
        return;
    }
    bool met = false;
    switch (goal.check) {
    case ConfigCheck::AddressIn: {
        verdict.expected = goal.prefix.to_string();
        for (std::size_t index = 0; index < device->ports.size(); ++index) {
            if (goal.port != 0xff && index != goal.port) continue;
            const auto& address = device->ports[index].ipv4;
            if (!address) continue;
            verdict.actual += (verdict.actual.empty() ? "" : ", ") + address->to_string();
            met = met || (goal.prefix.contains(address->address()) && address->length() == goal.prefix.length());
        }
        break;
    }
    case ConfigCheck::Gateway:
        verdict.expected = goal.address->to_string();
        verdict.actual = device->gateway ? device->gateway->to_string() : "";
        met = device->gateway == goal.address;
        break;
    case ConfigCheck::Route: {
        verdict.expected = goal.prefix.to_string() + (goal.address ? " via " + goal.address->to_string() : "");
        for (const auto& route : device->routes) {
            const bool same = route.destination.network() == goal.prefix.network() && route.destination.length() == goal.prefix.length();
            if (same) verdict.actual = route.destination.to_string() + " via " + route.next_hop.to_string();
            met = met || (same && (!goal.address || route.next_hop == *goal.address));
        }
        break;
    }
    case ConfigCheck::PortUp:
        verdict.expected = goal.up ? "up" : "down";
        if (goal.port < device->ports.size()) {
            verdict.actual = device->ports[goal.port].admin_up ? "up" : "down";
            met = device->ports[goal.port].admin_up == goal.up;
        }
        break;
    case ConfigCheck::Cable: {
        const auto* other = network.device_named(goal.other);
        verdict.expected = goal.other;
        if (!other) {
            verdict.finding = Finding::NoDevice;
            verdict.subject = goal.other;
            return;
        }
        met = std::any_of(network.links.begin(), network.links.end(), [&](const LinkConfig& link) {
            return (link.a.device == device->id && link.b.device == other->id) || (link.b.device == device->id && link.a.device == other->id);
        });
        break;
    }
    }
    verdict.finding = met ? Finding::Met : Finding::Different;
}

void check_observed(const Goal& goal, const Scenario& network, const Simulation* run, Verdict& verdict) {
    verdict.subject = goal.device;
    const auto* device = network.device_named(goal.device);
    if (!device) {
        verdict.finding = Finding::NoDevice;
        return;
    }
    verdict.finding = Finding::NotSeen;
    if (!run) return;
    for (const auto& record : run->trace().records()) {
        if (record.device != device->id) continue;
        bool seen = false;
        switch (goal.observation) {
        case Observation::ArpRequest:
        case Observation::ArpReply: {
            if (record.kind != TraceKind::FrameSent || !record.pdu) break;
            const auto& frame = run->trace().pdu(*record.pdu);
            const auto* arp = frame.arp();
            const auto op = goal.observation == Observation::ArpRequest ? ArpPacket::Op::Request : ArpPacket::Op::Reply;
            seen = arp && arp->op == op && sends_from(*device, frame) && (!goal.address || arp->target_ip == *goal.address);
            break;
        }
        case Observation::Routed: seen = record.kind == TraceKind::DatagramRouted; break;
        case Observation::EchoReply: seen = record.kind == TraceKind::PingReply; break;
        case Observation::MacLearned: seen = record.kind == TraceKind::MacLearned; break;
        }
        if (seen) {
            verdict.finding = Finding::Met;
            verdict.evidence.push_back(record);
            return;
        }
    }
}

void check_answer(const Goal& goal, const Scenario& network, const Answers& answers, Verdict& verdict) {
    const auto given = answers.find(goal.id);
    const auto* host = source(network, goal.from, verdict);
    if (!host) return;
    const auto result = probe(network, host->id, goal.to, goal.quantity == Quantity::Hops);
    if (!result.id) {
        verdict.finding = Finding::CannotStart;
        verdict.problem = result.problem;
        return;
    }
    verdict.evidence = evidence_of(result);
    const auto& status = *result.status();
    const auto& trace = result.simulation.trace();
    std::optional<std::string> value;
    switch (goal.quantity) {
    case Quantity::ReplyTtl:
        for (const auto& echo : status.echoes) {
            if (echo.state != EchoState::Replied) continue;
            const auto& record = trace.records().at(echo.record);
            if (record.pdu && trace.pdu(*record.pdu).ipv4()) value = std::to_string(trace.pdu(*record.pdu).ipv4()->ttl);
            verdict.evidence = {record};
            break;
        }
        break;
    case Quantity::Hops:
        if (status.state == PingState::Succeeded) value = std::to_string(status.echoes.size());
        break;
    case Quantity::FirstHopMac:
        for (const auto& record : trace.records()) {
            if (record.kind != TraceKind::FrameSent || record.device != host->id || !record.pdu) continue;
            const auto& frame = trace.pdu(*record.pdu);
            if (frame.ipv4() && frame.ipv4()->icmp.type == icmp::echo_request) {
                value = frame.dst.to_string();
                verdict.evidence = {record};
                break;
            }
        }
        break;
    }
    if (!value) {
        verdict.finding = Finding::Unreadable;
        verdict.reason = status.state == PingState::Running ? Reason::EchoTimeout : status.reason;
        return;
    }
    verdict.expected = *value;
    if (given == answers.end() || lowered(given->second).empty()) {
        verdict.finding = Finding::NoAnswer;
        return;
    }
    verdict.actual = given->second;
    verdict.finding = same_answer(given->second, *value) ? Finding::Met : Finding::WrongAnswer;
}

void check_kept(const Lab& lab, const Goal& goal, const Scenario& network, Verdict& verdict) {
    for (const auto& entry : goal.kept) {
        const auto* given = lab.network.device_named(entry.device);
        verdict.subject = entry.device;
        if (!given || !network.device(given->id)) {
            verdict.finding = Finding::NoDevice;
            return;
        }
        if (const auto setting = changed(lab, network, entry)) {
            verdict.finding = Finding::Changed;
            verdict.setting = *setting;
            return;
        }
    }
    verdict.subject.clear();
    verdict.finding = Finding::Met;
}

// A cable as seen from one of its devices, whatever its id.
using CableEnd = std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, SimTime, bool>;
std::vector<CableEnd> cables_of(const Scenario& network, DeviceId device) {
    std::vector<CableEnd> ends;
    for (const auto& link : network.links) {
        if (link.a.device == device) ends.emplace_back(link.a.port, link.b.device.value, link.b.port, link.delay, link.up);
        if (link.b.device == device) ends.emplace_back(link.b.port, link.a.device.value, link.a.port, link.delay, link.up);
    }
    std::sort(ends.begin(), ends.end());
    return ends;
}

// The devices and settings an edit changes, on the network before it.
std::vector<std::pair<DeviceId, Settings>> touched(const Scenario& before, const Edit& edit) {
    std::vector<std::pair<DeviceId, Settings>> out;
    const auto cable = [&](LinkId id) {
        if (const auto* link = before.link(id)) {
            out.emplace_back(link->a.device, Cables);
            out.emplace_back(link->b.device, Cables);
        }
    };
    const auto whole = [&](DeviceId device) {
        out.emplace_back(device, all_settings);
        for (const auto& link : before.links) {
            if (link.a.device == device) out.emplace_back(link.b.device, Cables);
            if (link.b.device == device) out.emplace_back(link.a.device, Cables);
        }
    };
    std::visit(Overloaded{
                   [&](const SetLinkUp& item) { cable(item.link); },
                   [&](const SetDelay& item) { cable(item.link); },
                   [&](const SetPortUp& item) { out.emplace_back(item.port.device, Ports); },
                   [&](const SetPowered& item) { out.emplace_back(item.device, Power); },
                   [&](const SetAddress& item) { out.emplace_back(item.port.device, Addresses); },
                   [&](const SetAgeing& item) { out.emplace_back(item.device, Ports); },
                   [&](const Rename& item) { out.emplace_back(item.device, Name); },
                   [&](const MoveDevice&) {},
                   [&](const Connect& item) {
                       out.emplace_back(item.a.device, Cables);
                       out.emplace_back(item.b.device, Cables);
                   },
                   [&](const Disconnect& item) { cable(item.link); },
                   [&](const Replug& item) {
                       cable(item.link);
                       out.emplace_back(item.to.device, Cables);
                   },
                   [&](const AddDevice&) {},
                   [&](const RemoveDevice& item) { whole(item.device); },
                   [&](const RestoreLink& item) {
                       out.emplace_back(item.link.a.device, Cables);
                       out.emplace_back(item.link.b.device, Cables);
                   },
                   [&](const RestoreDevice& item) {
                       out.emplace_back(item.device.id, all_settings);
                       for (const auto& link : item.links) {
                           out.emplace_back(link.link.a.device, Cables);
                           out.emplace_back(link.link.b.device, Cables);
                       }
                   },
                   [&](const SetGateway& item) { out.emplace_back(item.device, Gateway); },
                   [&](const SetRoutes& item) { out.emplace_back(item.device, Routes); },
                   [&](const SetPorts& item) { out.emplace_back(item.device, Ports | Addresses); },
               },
               edit);
    return out;
}

} // namespace

Verdict check(const Lab& lab, std::size_t index, const Scenario& network, const Simulation* run, const Answers& answers) {
    Verdict verdict;
    verdict.goal = index;
    const auto& goal = lab.goals.at(index);
    switch (goal.kind) {
    case GoalKind::Reach: check_reach(goal, network, verdict); break;
    case GoalKind::Path: check_path(goal, network, verdict); break;
    case GoalKind::Config: check_config(goal, network, verdict); break;
    case GoalKind::Observed: check_observed(goal, network, run, verdict); break;
    case GoalKind::Answer: check_answer(goal, network, answers, verdict); break;
    case GoalKind::Kept: check_kept(lab, goal, network, verdict); break;
    case GoalKind::Command: {
        verdict.subject = goal.device;
        if (!network.device_named(goal.device)) {
            verdict.finding = Finding::NoDevice;
            break;
        }
        const auto typed = answers.find(goal.id);
        verdict.finding = typed != answers.end() && command_matches(goal.command, typed->second) ? Finding::Met : Finding::NotRun;
        if (typed != answers.end()) verdict.actual = typed->second;
        verdict.expected = goal.command;
        break;
    }
    }
    verdict.met = verdict.finding == Finding::Met;
    return verdict;
}

std::vector<Verdict> check(const Lab& lab, const Scenario& network, const Simulation* run, const Answers& answers) {
    std::vector<Verdict> verdicts;
    for (std::size_t index = 0; index < lab.goals.size(); ++index) verdicts.push_back(check(lab, index, network, run, answers));
    return verdicts;
}

bool all_met(const std::vector<Verdict>& verdicts) {
    return std::all_of(verdicts.begin(), verdicts.end(), [](const Verdict& verdict) { return verdict.met; });
}

Simulation solution_run(const Lab& lab) {
    Simulation run(lab.solution.network);
    std::size_t events = 0;
    for (const auto& command : lab.solution.commands) {
        const auto* host = lab.solution.network.device_named(command.from);
        if (!host) continue;
        PingOptions options;
        if (command.traceroute) {
            options.traceroute = true;
            options.count = default_max_hops;
        }
        Problem problem = Problem::None;
        const auto id = run.start_ping(host->id, command.to, problem, options);
        while (id && run.ping(*id)->state == PingState::Running && events++ < run_events && run.step()) {}
    }
    while (events++ < run_events && run.step()) {}
    return run;
}

std::vector<std::string> verify(const Lab& lab) {
    auto issues = validate(lab);
    if (!issues.empty()) return issues;
    const Simulation given(lab.network);
    if (all_met(check(lab, lab.network, &given, {})))
        issues.push_back("every goal is already met in the network the lab starts from");
    const auto run = solution_run(lab);
    const auto verdicts = check(lab, lab.solution.network, &run, lab.solution.answers);
    for (const auto& verdict : verdicts)
        if (!verdict.met) issues.push_back("the solution does not meet goal \"" + lab.goals[verdict.goal].id + "\"");
    for (const auto& lock : lab.locks) {
        const auto* device = lab.network.device_named(lock.device);
        if (!device || !lab.solution.network.device(device->id))
            issues.push_back("the solution removes the locked device \"" + lock.device + "\"");
        else if (const auto setting = changed(lab, lab.solution.network, lock))
            issues.push_back("the solution changes the " + std::string(name(*setting)) + " of the locked device \"" + lock.device + "\"");
    }
    return issues;
}

std::optional<DeviceSettings> locked(const Lab& lab, const Scenario& before, const Edit& edit) {
    if (lab.locks.empty()) return std::nullopt;
    for (const auto& [device, settings] : touched(before, edit)) {
        for (const auto& lock : lab.locks) {
            const auto* given = lab.network.device_named(lock.device);
            if (given && given->id == device && (lock.settings & settings)) return DeviceSettings{lock.device, static_cast<Settings>(lock.settings & settings)};
        }
    }
    return std::nullopt;
}

std::optional<Setting> changed(const Lab& lab, const Scenario& network, const DeviceSettings& entry) {
    const auto* given = lab.network.device_named(entry.device);
    if (!given) return std::nullopt;
    const auto* now = network.device(given->id);
    if (!now) return Name;
    const auto addresses = [](const DeviceConfig& device) {
        std::vector<std::optional<Ipv4Prefix>> out;
        for (const auto& port : device.ports) out.push_back(port.ipv4);
        return out;
    };
    const auto ports = [](const DeviceConfig& device) {
        std::vector<bool> out;
        for (const auto& port : device.ports) out.push_back(port.admin_up);
        return std::pair{out, device.mac_ageing};
    };
    for (const auto setting : setting_list) {
        if (!(entry.settings & setting)) continue;
        bool same = true;
        switch (setting) {
        case Name: same = now->name == given->name; break;
        case Power: same = now->powered == given->powered; break;
        case Addresses: same = addresses(*now) == addresses(*given); break;
        case Gateway: same = now->gateway == given->gateway; break;
        case Routes: same = now->routes == given->routes; break;
        case Ports: same = ports(*now) == ports(*given); break;
        case Cables: same = cables_of(network, now->id) == cables_of(lab.network, given->id); break;
        }
        if (!same) return setting;
    }
    return std::nullopt;
}

bool command_matches(std::string_view goal, std::string_view typed) {
    const auto wanted = term::canonical(goal), given = term::canonical(typed);
    if (!wanted || !given) return false;
    const auto a = term::words(*wanted), b = term::words(*given);
    return b.size() >= a.size() && std::equal(a.begin(), a.end(), b.begin());
}

Scenario starting_configuration(const Lab& lab) {
    auto network = lab.solution.network;
    for (auto& device : network.devices) {
        const auto* given = lab.network.device_named(device.name);
        if (given && given->kind == device.kind && given->ports.size() == device.ports.size()) {
            for (std::size_t port = 0; port < device.ports.size(); ++port) {
                device.ports[port].ipv4 = given->ports[port].ipv4;
                device.ports[port].admin_up = given->ports[port].admin_up;
            }
            device.gateway = given->gateway;
            device.routes = given->routes;
            continue;
        }
        // As the learner adds it, then as the lab says new devices start.
        Scenario fresh;
        edit::apply(fresh, AddDevice{device.kind, 0, 0, device.id});
        const auto& boxed = fresh.devices.front();
        for (std::size_t port = 0; port < device.ports.size(); ++port) {
            device.ports[port].ipv4 = port < boxed.ports.size() ? boxed.ports[port].ipv4 : std::nullopt;
            device.ports[port].admin_up = true;
        }
        device.gateway.reset();
        device.routes.clear();
        for (const auto& change : new_device_edits(lab, device)) edit::apply(network, change);
    }
    return network;
}

std::vector<Edit> new_device_edits(const Lab& lab, const DeviceConfig& device) {
    std::vector<Edit> edits;
    if (device.kind == DeviceKind::Host && !lab.new_devices.host_address)
        edits.push_back(SetAddress{PortRef{device.id, 0}, std::nullopt});
    if (device.kind == DeviceKind::Router && !lab.new_devices.router_ports_up)
        for (std::size_t port = 0; port < device.ports.size(); ++port)
            edits.push_back(SetPortUp{PortRef{device.id, static_cast<std::uint8_t>(port)}, false});
    return edits;
}

} // namespace netsim::lab

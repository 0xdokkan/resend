#include "explain/explain.hpp"

#include <algorithm>
#include <chrono>
#include <span>

namespace netsim::explain {
namespace {
using namespace std::chrono_literals;
static_assert(arp_retry_interval == 1s && arp_attempts == 3 && echo_timeout == 1s,
              "the texts below describe these timers");

std::string type_name(PduKind kind) {
    switch (kind) {
    case PduKind::ArpRequest: return "ARP Request";
    case PduKind::ArpReply: return "ARP Reply";
    case PduKind::EchoRequest: return "Echo Request";
    case PduKind::EchoReply: return "Echo Reply";
    case PduKind::OtherIcmp: break;
    }
    return "ICMP message";
}

std::string device_name(const Scenario& scenario, std::optional<DeviceId> id) {
    if (!id) return "?";
    const auto* device = scenario.device(*id);
    return device ? device->name : "device " + std::to_string(id->value);
}

std::string port_name(std::optional<std::uint8_t> port) { return port ? std::to_string(*port + 1) : "?"; }

// A frame reaching a device: the arrival, then everything the device did
// about it, in one event.
Event arrival(const Scenario& scenario, const Trace& trace, std::span<const TraceRecord> records) {
    const auto& first = records.front();
    const auto& frame = trace.pdu(*first.pdu);
    const auto* arp = frame.arp();
    const auto* packet = frame.ipv4();
    const auto type = type_name(kind_of(frame));
    const auto src_ip = (arp ? arp->sender_ip : packet->src).to_string();
    const auto dst_ip = (arp ? arp->target_ip : packet->dst).to_string();
    const auto peer = scenario.peer({first.device, first.port.value_or(0)});

    Event event{.time = first.time, .episode = first.episode, .protocol = arp ? "ARP" : "ICMP"};
    event.from = peer ? std::optional(peer->device) : std::nullopt;
    event.to = first.device;
    event.link = first.link;
    event.pdu = first.pdu;
    event.summary = device_name(scenario, event.from) + " → " + device_name(scenario, event.to) + " · " + type;
    auto& detail = event.detail;
    detail = "Ethernet: " + frame.src.to_string() + " → " + frame.dst.to_string() + "\n" + (arp ? "ARP: " : "IPv4: ") +
        src_ip + " → " + dst_ip + "\n" + type + "; ingress port " + port_name(first.port) + ".";
    if (first.kind == TraceKind::FrameDropped) {
        detail += "\nCannot be received: the link or the device is disabled.";
        event.success = false;
        return event;
    }

    bool resolved = false;
    for (std::size_t i = 1; i < records.size(); ++i) {
        const auto& record = records[i];
        // A frame this device builds in response, sent or not.
        const bool response = record.pdu != first.pdu &&
            (record.kind == TraceKind::FrameSent || (record.kind == TraceKind::FrameDropped && record.reason == Reason::NoCarrier));
        if (response) {
            switch (kind_of(trace.pdu(*record.pdu))) {
            case PduKind::ArpReply:
                detail += "\nThis PC owns the requested IP: it sends a unicast ARP reply.";
                break;
            case PduKind::EchoRequest:
                if (!resolved) detail += "\nMAC resolved: the IPv4 datagram carrying the ICMP Echo Request can be sent.";
                resolved = true;
                break;
            case PduKind::EchoReply:
                detail += "\nICMP Echo Request received: an Echo Reply with the same identifier is generated.";
                break;
            case PduKind::ArpRequest:
            case PduKind::OtherIcmp:
                break;
            }
            continue;
        }
        switch (record.kind) {
        case TraceKind::MacLearned:
            detail += "\nLearned source MAC " + frame.src.to_string() + " on port " + port_name(first.port) + ".";
            break;
        case TraceKind::FrameFlooded: {
            const auto copies = std::count_if(records.begin() + static_cast<std::ptrdiff_t>(i) + 1, records.end(), [&](const auto& item) {
                return item.kind == TraceKind::FrameSent && item.pdu == first.pdu;
            });
            detail += "\nFlooded to every active port except the ingress port (" + std::to_string(copies) + ").";
            break;
        }
        case TraceKind::FrameFiltered:
            detail += "\nFrame filtered: the destination is on the ingress port.";
            break;
        case TraceKind::FrameForwarded:
            // The next record says whether the copy left the egress port.
            if (i + 1 < records.size()) {
                const auto& out = records[++i];
                detail += "\nUnicast on port " + port_name(out.port) + (out.kind == TraceKind::FrameSent ? "." : ": the link is down.");
            }
            break;
        case TraceKind::ArpCacheUpdated:
            if (arp) detail += "\nARP cache updated: " + src_ip + " = " + arp->sender_mac.to_string() + ".";
            break;
        case TraceKind::FrameDropped:
            switch (record.reason) {
            case Reason::NotForUs:
                detail += "\nFrame ignored: the destination MAC is not the local MAC.";
                break;
            case Reason::ArpForOtherHost:
                detail += "\nRequest for another address: no reply.";
                break;
            case Reason::Ipv4NotForUs:
                detail += "\nDatagram ignored: the destination IP is not the local IP.";
                break;
            case Reason::NoRoute:
                detail += "\nEcho received, but there is no return route: the source is outside the local subnet. "
                          "Routers and gateways are not implemented yet.";
                event.success = false;
                break;
            case Reason::NotModelled:
                detail += "\nReply held back: there is no ARP entry for the sender. "
                          "Resolving it via ARP before replying is not modelled in this prototype.";
                event.success = false;
                break;
            default:
                break;
            }
            break;
        case TraceKind::PingSucceeded:
            event.summary = "Ping succeeded · " + src_ip + " → " + device_name(scenario, event.to);
            detail += "\nEcho Reply matching the current ping. The destination is reachable.";
            break;
        default:
            break;
        }
    }
    return event;
}

// A timer running out at a host: a ping starting, an ARP retry, a timeout.
// One record may make an event of its own.
void timers(const Simulation& simulation, std::span<const TraceRecord> records, std::vector<Event>& events) {
    const auto& scenario = simulation.scenario();
    const auto& trace = simulation.trace();
    for (const auto& record : records) {
        Event event{.time = record.time, .episode = record.episode, .from = record.device};
        const auto source = device_name(scenario, record.device);
        const bool attempt = record.pdu && kind_of(trace.pdu(*record.pdu)) == PduKind::ArpRequest &&
            (record.kind == TraceKind::FrameSent || record.kind == TraceKind::FrameDropped);
        if (attempt) {
            const bool sent = record.kind == TraceKind::FrameSent;
            event.protocol = "ARP";
            event.summary = source + " · who has " + trace.pdu(*record.pdu).arp()->target_ip.to_string() + "?";
            event.detail = "ARP Request sent to broadcast (" + MacAddress::broadcast().to_string() + "). Attempt " +
                std::to_string(record.value) + "/" + std::to_string(arp_attempts) + ".\n" +
                (sent ? "The destination is in the local subnet. Looking for its MAC address."
                      : "The port has no active link: no frame can leave.");
            event.pdu = record.pdu;
            event.success = sent;
        } else if (record.kind == TraceKind::ArpTimeout && record.value < arp_attempts) {
            event.protocol = "ARP";
            event.summary = "No ARP reply · retrying";
            event.detail = "One simulated second has passed. The PC repeats the ARP request.";
        } else if (record.kind == TraceKind::PingSucceeded) {
            event.protocol = "ICMP";
            event.summary = "Local ping succeeded";
            event.detail = "The destination is the PC's own address: no frame crosses the cables.";
            event.to = record.device;
        } else if (record.kind == TraceKind::PingFailed) {
            event.success = false;
            switch (record.reason) {
            case Reason::NoRoute: {
                const auto* ping = simulation.ping(PingId{record.value});
                event.protocol = "IPv4";
                event.summary = "Ping failed · no route";
                event.detail = "The destination " + (ping ? ping->destination.to_string() : std::string("?")) +
                    " is outside the local subnet. This prototype has no routers or gateways yet, "
                    "so no ARP request is sent for the remote host.";
                break;
            }
            case Reason::ArpUnresolved:
                event.protocol = "ARP";
                event.summary = "Ping failed · ARP resolution timed out";
                event.detail = "No reply after three ARP requests. Check the address, the cables and that the devices are on. "
                               "No ICMP was sent.";
                break;
            default:
                event.protocol = "ICMP";
                event.summary = "Ping failed · reply timed out";
                event.detail = "The MAC is known but no Echo Reply arrived. Check the subnet and the return path too.";
                break;
            }
        } else {
            continue;
        }
        events.push_back(std::move(event));
    }
}

} // namespace

void Explainer::update(const Simulation& simulation) {
    const auto& records = simulation.trace().records();
    while (next_ < records.size()) {
        const auto episode = records[next_].episode;
        auto end = next_;
        while (end < records.size() && records[end].episode == episode) ++end;
        const std::span<const TraceRecord> group(records.data() + next_, end - next_);
        // An episode that starts with a frame at a port is an arrival; any
        // other starts with a timer.
        const auto& first = group.front();
        if (first.link && (first.kind == TraceKind::FrameReceived || first.kind == TraceKind::FrameDropped))
            events_.push_back(arrival(simulation.scenario(), simulation.trace(), group));
        else
            timers(simulation, group, events_);
        next_ = end;
    }
}

std::string describe(Problem problem) {
    switch (problem) {
    case Problem::None: return {};
    case Problem::DuplicateDeviceId: return "Two devices have the same id.";
    case Problem::InvalidDeviceId: return "Device ids start from 1.";
    case Problem::InvalidName: return "Use a name of 1 to 48 characters, without control characters.";
    case Problem::WrongPortCount: return "A PC has one port, a switch has eight.";
    case Problem::MissingAddress: return "Every PC needs an IPv4 address.";
    case Problem::InvalidHostAddress:
        return "Invalid IPv4 host address: network, broadcast, loopback and multicast addresses are excluded.";
    case Problem::InvalidPrefix: return "IPv4 prefixes go from /1 to /32.";
    case Problem::AddressOnSwitch: return "Switch ports have no IPv4 address.";
    case Problem::DuplicateLinkId: return "Two cables have the same id.";
    case Problem::InvalidLinkId: return "Cable ids start from 1.";
    case Problem::NoSuchDevice: return "Device not found.";
    case Problem::NoSuchPort: return "Port not found.";
    case Problem::NoSuchLink: return "Link not found.";
    case Problem::PortInUse: return "That port already has a cable.";
    case Problem::SameDevice: return "Connect two different devices.";
    case Problem::InvalidDelay: return "A cable's delay goes from 1 µs to 10 s.";
    case Problem::LoopNotSupported:
        return "Ethernet loop: only loop-free networks are accepted; STP is not implemented yet.";
    case Problem::TooManyDevices: return "A network holds at most 100 devices.";
    case Problem::NoFreePort: return "No free port: a PC has one port, a switch has eight.";
    case Problem::NotAHost: return "Select a PC to configure IPv4.";
    case Problem::SourceNotHost: return "Select a PC as the ping source.";
    case Problem::SourcePoweredOff: return "The source PC is powered off.";
    case Problem::InvalidDestination: return "Enter a valid unicast IPv4 address.";
    case Problem::BroadcastDestination: return "Pinging network or broadcast addresses is not supported.";
    }
    return "Unknown problem.";
}

std::string describe(Reason reason) {
    switch (reason) {
    case Reason::None: return {};
    case Reason::LinkDown: return "the cable is down";
    case Reason::DeviceDown: return "the device is powered off";
    case Reason::NoCarrier: return "the port has no active cable";
    case Reason::NotForUs: return "the destination MAC belongs to another device";
    case Reason::ArpForOtherHost: return "the ARP request asks for another address";
    case Reason::Ipv4NotForUs: return "the destination IP belongs to another device";
    case Reason::NoRoute: return "the address is outside the local subnet, and there are no routers yet";
    case Reason::NotModelled: return "this case is not simulated yet";
    case Reason::ArpUnresolved: return "three ARP requests went unanswered";
    case Reason::EchoTimeout: return "no Echo Reply within a second";
    }
    return {};
}

std::string_view name(TraceKind kind) {
    switch (kind) {
    case TraceKind::FrameSent: return "FrameSent";
    case TraceKind::FrameReceived: return "FrameReceived";
    case TraceKind::FrameDropped: return "FrameDropped";
    case TraceKind::MacLearned: return "MacLearned";
    case TraceKind::FrameFlooded: return "FrameFlooded";
    case TraceKind::FrameForwarded: return "FrameForwarded";
    case TraceKind::FrameFiltered: return "FrameFiltered";
    case TraceKind::ArpCacheUpdated: return "ArpCacheUpdated";
    case TraceKind::ArpTimeout: return "ArpTimeout";
    case TraceKind::PingSucceeded: return "PingSucceeded";
    case TraceKind::PingFailed: return "PingFailed";
    case TraceKind::PingStarted: return "PingStarted";
    }
    return "Unknown";
}

std::string_view name(Reason reason) {
    switch (reason) {
    case Reason::None: return "None";
    case Reason::LinkDown: return "LinkDown";
    case Reason::DeviceDown: return "DeviceDown";
    case Reason::NoCarrier: return "NoCarrier";
    case Reason::NotForUs: return "NotForUs";
    case Reason::ArpForOtherHost: return "ArpForOtherHost";
    case Reason::Ipv4NotForUs: return "Ipv4NotForUs";
    case Reason::NoRoute: return "NoRoute";
    case Reason::NotModelled: return "NotModelled";
    case Reason::ArpUnresolved: return "ArpUnresolved";
    case Reason::EchoTimeout: return "EchoTimeout";
    }
    return "Unknown";
}

} // namespace netsim::explain

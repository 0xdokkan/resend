#include "explain/explain.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <span>

namespace netsim::explain {
namespace {
using namespace std::chrono_literals;
static_assert(arp_retry_interval == 1s && arp_attempts == 3 && arp_cache_timeout == 60s,
              "the texts below describe these timers");

template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

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

std::string device_name(const Simulation& simulation, std::optional<DeviceId> id) {
    if (!id) return "?";
    const auto* device = simulation.known_device(*id);
    return device ? device->name : "device " + std::to_string(id->value);
}

std::string port_name(std::optional<std::uint8_t> port) { return port ? std::to_string(*port + 1) : "?"; }

bool about_ping(TraceKind kind) {
    return kind == TraceKind::PingStarted || kind == TraceKind::PingReply || kind == TraceKind::PingLost ||
        kind == TraceKind::PingSucceeded || kind == TraceKind::PingFailed;
}

std::string port_text(const Simulation& simulation, PortRef port) {
    return device_name(simulation, port.device) + " port " + std::to_string(port.port + 1);
}

std::string cable_text(const Simulation& simulation, const LinkConfig& cable) {
    return port_text(simulation, cable.a) + " — " + port_text(simulation, cable.b);
}

// Milliseconds with three decimals and no unit: "40.000".
std::string ms(SimTime time) {
    const auto text = format_ms(time);
    return text.substr(0, text.size() - 3);
}

std::string duration_text(SimTime time) {
    if (time >= 1s && time % 1s == 0ns) return std::to_string(time / 1s) + " s";
    return format_ms(time);
}

// The request of a ping that a PingReply or PingLost record is about.
const EchoStatus* echo_of(const PingStatus* ping, const TraceRecord& record) {
    if (!ping) return nullptr;
    const auto it = std::find_if(ping->echoes.begin(), ping->echoes.end(), [&](const auto& echo) { return echo.record == record.seq; });
    return it == ping->echoes.end() ? nullptr : &*it;
}

// "4 requests, 3 replies, 25% loss. Round trip min/avg/max: 40.000/40.000/40.000 ms."
std::string statistics(const PingStatus& ping) {
    const auto sent = ping.echoes.size();
    const auto replies = static_cast<std::size_t>(ping.replies());
    std::ostringstream out;
    out << sent << (sent == 1 ? " request, " : " requests, ") << replies << (replies == 1 ? " reply, " : " replies, ")
        << (sent ? static_cast<double>(sent - replies) * 100.0 / static_cast<double>(sent) : 0.0) << "% loss.";
    SimTime low = SimTime::max(), high{}, total{};
    for (const auto& echo : ping.echoes) {
        if (echo.state != EchoState::Replied) continue;
        const auto rtt = echo.finished - echo.sent;
        low = std::min(low, rtt);
        high = std::max(high, rtt);
        total += rtt;
    }
    if (replies) {
        out << " Round trip min/avg/max: " << ms(low) << '/' << ms(total / static_cast<std::int64_t>(replies)) << '/' << ms(high)
            << " ms.";
    }
    return out.str();
}

std::string loss_detail(Reason reason, const PingStatus* ping) {
    switch (reason) {
    case Reason::NoRoute:
        return "The destination " + (ping ? ping->destination.to_string() : std::string("?")) +
            " is outside the local subnet. This prototype has no routers or gateways yet, "
            "so no ARP request is sent for the remote host.";
    case Reason::ArpUnresolved:
        return "No reply after three ARP requests. Check the address, the cables and that the devices are on. "
               "No ICMP was sent.";
    case Reason::ArpQueueFull:
        return "More datagrams were waiting for the same ARP answer than the PC keeps (" + std::to_string(arp_queue_limit) +
            "): the oldest was discarded to keep the latest.";
    case Reason::DeviceDown:
        return "The PC was powered off or removed: its pings stop and their requests get no reply.";
    default:
        return "The MAC is known but no Echo Reply arrived. Check the subnet and the return path too, and whether "
               "the cached MAC still belongs to that address.";
    }
}

std::string loss_summary(Reason reason) {
    switch (reason) {
    case Reason::NoRoute: return "no route";
    case Reason::ArpUnresolved: return "ARP resolution timed out";
    case Reason::ArpQueueFull: return "ARP queue full";
    case Reason::DeviceDown: return "the PC is off";
    default: return "reply timed out";
    }
}

std::string loss_protocol(Reason reason) {
    switch (reason) {
    case Reason::NoRoute: return "IPv4";
    case Reason::ArpUnresolved:
    case Reason::ArpQueueFull: return "ARP";
    default: return "ICMP";
    }
}

Event guard_event(const Simulation& simulation, const TraceRecord& record) {
    Event event{.time = record.time, .episode = record.episode, .protocol = "SIMULATOR", .from = record.device, .pdu = record.pdu};
    event.summary = "Storm protection · frame stopped at " + device_name(simulation, record.device);
    event.detail = "This " + type_name(kind_of(simulation.trace().pdu(*record.pdu))) + " was sent " + std::to_string(record.value) +
        " times: more often than a network without loops could ever send it. The network has an Ethernet loop and "
        "nothing stops the copies going round, so the simulator removed them all. Real switches would keep "
        "flooding; spanning tree (STP) prevents this and is not implemented yet.";
    event.success = false;
    event.simulator = true;
    return event;
}

// The transmission whose arrival a record describes.
const Transmission* sender_of(const Trace& trace, const TraceRecord& record) {
    const auto& all = trace.transmissions();
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (it->pdu == record.pdu && it->link == record.link && it->to == record.device && it->to_port == record.port &&
            it->arrival == record.time && !it->stopped)
            return &*it;
    return nullptr;
}

// A frame reaching a device: the arrival, then everything the device did
// about it, in one event. The storm guard gets events of its own.
void arrival(const Simulation& simulation, std::span<const TraceRecord> records, std::vector<Event>& events) {
    const auto& trace = simulation.trace();
    const auto& first = records.front();
    const auto& frame = trace.pdu(*first.pdu);
    const auto* arp = frame.arp();
    const auto* packet = frame.ipv4();
    const auto type = type_name(kind_of(frame));
    const auto src_ip = (arp ? arp->sender_ip : packet->src).to_string();
    const auto dst_ip = (arp ? arp->target_ip : packet->dst).to_string();
    const auto* sent = sender_of(trace, first);

    Event event{.time = first.time, .episode = first.episode, .protocol = arp ? "ARP" : "ICMP"};
    event.from = sent ? std::optional(sent->from) : std::nullopt;
    event.to = first.device;
    event.link = first.link;
    event.pdu = first.pdu;
    event.summary = device_name(simulation, event.from) + " → " + device_name(simulation, event.to) + " · " + type;
    auto& detail = event.detail;
    detail = "Ethernet: " + frame.src.to_string() + " → " + frame.dst.to_string() + "\n" + (arp ? "ARP: " : "IPv4: ") +
        src_ip + " → " + dst_ip + "\n" + type + "; ingress port " + port_name(first.port) + ".";
    if (first.kind == TraceKind::FrameDropped) {
        detail += first.reason == Reason::LinkDown
            ? "\nLost on the way: the cable was disabled, unplugged or moved while the frame was on it."
            : "\nCannot be received: the device is powered off.";
        event.success = false;
        events.push_back(std::move(event));
        return;
    }

    std::vector<Event> guards;
    bool resolved = false;
    // An ARP answer releases the datagrams waiting for it.
    const auto released = [&](std::size_t from) {
        return std::count_if(records.begin() + static_cast<std::ptrdiff_t>(from), records.end(), [&](const auto& item) {
            if (!item.pdu || item.pdu == first.pdu || item.kind != TraceKind::FrameSent) return false;
            const auto kind = kind_of(trace.pdu(*item.pdu));
            return kind == PduKind::EchoRequest || kind == PduKind::EchoReply;
        });
    };
    for (std::size_t i = 1; i < records.size(); ++i) {
        const auto& record = records[i];
        // A frame this device builds in response, sent or not.
        const bool response = record.pdu && record.pdu != first.pdu &&
            (record.kind == TraceKind::FrameSent || (record.kind == TraceKind::FrameDropped && record.reason == Reason::NoCarrier));
        if (response) {
            const auto& built = trace.pdu(*record.pdu);
            switch (kind_of(built)) {
            case PduKind::ArpReply:
                detail += "\nThis PC owns the requested IP: it sends a unicast ARP reply.";
                break;
            case PduKind::EchoRequest:
            case PduKind::EchoReply:
                if (!arp) {
                    detail += "\nICMP Echo Request received: an Echo Reply with the same identifier is generated.";
                } else if (!resolved) {
                    const auto count = released(i);
                    detail += count > 1 ? "\nMAC resolved: the " + std::to_string(count) + " IPv4 datagrams waiting for it can be sent."
                        : kind_of(built) == PduKind::EchoRequest ? "\nMAC resolved: the IPv4 datagram carrying the ICMP Echo Request can be sent."
                                                                 : "\nMAC resolved: the waiting Echo Reply can be sent.";
                    resolved = true;
                }
                break;
            case PduKind::ArpRequest:
                detail += "\nICMP Echo Request received, but there is no ARP entry for the sender: the Echo Reply waits "
                          "while an ARP request asks for " + built.arp()->target_ip.to_string() + ".";
                break;
            case PduKind::OtherIcmp:
                break;
            }
            continue;
        }
        switch (record.kind) {
        case TraceKind::MacLearned:
            detail += "\nLearned source MAC " + frame.src.to_string() + " on port " + port_name(first.port) + ".";
            break;
        case TraceKind::MacEntryAged: {
            const auto* device = simulation.known_device(record.device);
            detail += "\nThe entry for " + frame.dst.to_string() + " had aged out: no frame from it for " +
                duration_text(device ? device->mac_ageing : default_mac_ageing) + ".";
            break;
        }
        case TraceKind::FrameFlooded: {
            const auto copies = std::count_if(records.begin() + static_cast<std::ptrdiff_t>(i) + 1, records.end(), [&](const auto& item) {
                return item.kind == TraceKind::FrameSent && item.pdu == first.pdu;
            });
            detail += copies == 0 ? std::string("\nTo be flooded, but no other port has an active cable.")
                : "\nFlooded to every active port except the ingress port: " + std::to_string(copies) + (copies == 1 ? " copy." : " copies.");
            break;
        }
        case TraceKind::FrameFiltered:
            detail += "\nFrame filtered: the destination is on the ingress port.";
            break;
        case TraceKind::FrameForwarded:
            // The next record says whether the copy left the egress port.
            if (i + 1 < records.size() && records[i + 1].kind != TraceKind::SimulatorGuard) {
                const auto& out = records[++i];
                detail += "\nUnicast on port " + port_name(out.port) + (out.kind == TraceKind::FrameSent ? "." : ": the port has no active cable.");
            }
            break;
        case TraceKind::ArpCacheUpdated:
            if (arp) detail += "\nARP cache updated: " + src_ip + " = " + arp->sender_mac.to_string() + ".";
            break;
        case TraceKind::ArpEntryExpired:
            detail += "\nThe ARP entry for " + Ipv4Address(record.value).to_string() + " has expired: no update for 60 s.";
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
                event.success = false;
                break;
            case Reason::UnmatchedReply:
                detail += "\nEcho Reply ignored: no request is waiting for it. Its request already timed out, or its ping ended.";
                event.success = false;
                break;
            case Reason::NoRoute:
                detail += "\nEcho received, but there is no return route: the source is outside the local subnet. "
                          "Routers and gateways are not implemented yet.";
                event.success = false;
                break;
            default:
                break;
            }
            break;
        case TraceKind::DatagramDropped:
            detail += "\nAn Echo Reply waiting for ARP was discarded: " + describe(record.reason) + ".";
            event.success = false;
            break;
        case TraceKind::PingReply: {
            const auto* ping = simulation.ping(PingId{record.value});
            const auto* echo = echo_of(ping, record);
            const auto rtt = echo ? ms(echo->finished - echo->sent) + " ms" : std::string("?");
            if (ping && ping->options.count > 1) {
                const auto number = echo ? std::to_string(echo->sequence) : std::string("?");
                event.summary = "Reply " + number + "/" + std::to_string(ping->options.count) + " · " + src_ip + " → " +
                    device_name(simulation, event.to);
                detail += "\nEcho Reply for request " + number + " of the ping. Round trip: " + rtt + ".";
            } else {
                event.summary = "Ping succeeded · " + src_ip + " → " + device_name(simulation, event.to);
                detail += "\nEcho Reply matching the current ping. The destination is reachable. Round trip: " + rtt + ".";
            }
            break;
        }
        case TraceKind::PingSucceeded:
        case TraceKind::PingFailed: {
            const auto* ping = simulation.ping(PingId{record.value});
            if (ping && ping->options.count > 1) detail += "\nPing finished: " + statistics(*ping);
            break;
        }
        case TraceKind::SimulatorGuard:
            guards.push_back(guard_event(simulation, record));
            break;
        default:
            break;
        }
    }
    events.push_back(std::move(event));
    for (auto& guard : guards) events.push_back(std::move(guard));
}

// The end of a ping: one event, which for a single request tells its fate.
Event ping_end(const Simulation& simulation, const TraceRecord& record) {
    Event event{.time = record.time, .episode = record.episode, .from = record.device};
    const auto* ping = simulation.ping(PingId{record.value});
    const bool single = !ping || ping->options.count == 1;
    const bool succeeded = record.kind == TraceKind::PingSucceeded;
    event.success = succeeded;
    if (single && succeeded) {
        event.protocol = "ICMP";
        event.summary = "Local ping succeeded";
        event.detail = "The destination is the PC's own address: no frame crosses the cables.";
        event.to = record.device;
    } else if (single) {
        event.protocol = loss_protocol(record.reason);
        event.summary = "Ping failed · " + loss_summary(record.reason);
        event.detail = loss_detail(record.reason, ping);
    } else {
        event.protocol = "ICMP";
        event.summary = succeeded ? "Ping finished · " + std::to_string(ping->replies()) + "/" + std::to_string(ping->echoes.size()) + " replies"
                                  : "Ping failed · no reply";
        event.detail = "Ping from " + device_name(simulation, record.device) + " to " + ping->destination.to_string() + ": " +
            statistics(*ping);
        if (!succeeded) event.detail += "\n" + loss_detail(record.reason, ping);
    }
    return event;
}

// A timer running out at a host: a ping starting, an ARP retry, a timeout.
// One record may make an event of its own.
void timers(const Simulation& simulation, std::span<const TraceRecord> records, std::vector<Event>& events) {
    const auto& trace = simulation.trace();
    for (const auto& record : records) {
        Event event{.time = record.time, .episode = record.episode, .from = record.device};
        const auto source = device_name(simulation, record.device);
        const auto* ping = about_ping(record.kind) ? simulation.ping(PingId{record.value}) : nullptr;
        const bool single = !ping || ping->options.count == 1;
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
        } else if (record.kind == TraceKind::ArpEntryExpired) {
            event.protocol = "ARP";
            event.summary = source + " · ARP entry for " + Ipv4Address(record.value).to_string() + " expired";
            event.detail = "The entry was not refreshed for 60 s, so the PC no longer trusts it and asks again.";
        } else if (record.kind == TraceKind::PingReply && !single) {
            const auto* echo = echo_of(ping, record);
            event.protocol = "ICMP";
            event.summary = "Local reply " + (echo ? std::to_string(echo->sequence) : std::string("?")) + "/" +
                std::to_string(ping->options.count);
            event.detail = "The destination is the PC's own address: no frame crosses the cables.";
            event.to = record.device;
        } else if (record.kind == TraceKind::PingLost && !single) {
            const auto* echo = echo_of(ping, record);
            event.protocol = loss_protocol(record.reason);
            event.summary = "Request " + (echo ? std::to_string(echo->sequence) : std::string("?")) + "/" +
                std::to_string(ping->options.count) + " lost · " + loss_summary(record.reason);
            event.detail = loss_detail(record.reason, ping);
            event.pdu = record.pdu;
            event.success = false;
        } else if (record.kind == TraceKind::PingSucceeded || record.kind == TraceKind::PingFailed) {
            event = ping_end(simulation, record);
        } else if (record.kind == TraceKind::DatagramDropped) {
            event.protocol = "ARP";
            event.summary = source + " · Echo Reply to " + Ipv4Address(record.value).to_string() + " discarded";
            event.detail = "The reply waited for ARP to find the requester's MAC address, but " + describe(record.reason) + ".";
            event.success = false;
        } else if (record.kind == TraceKind::SimulatorGuard) {
            event = guard_event(simulation, record);
        } else {
            continue;
        }
        events.push_back(std::move(event));
    }
}

std::string state_text(bool up) { return up ? "enabled" : "disabled"; }

// An edit made while the simulation ran.
Event edit_event(const Simulation& simulation, const TraceRecord& record) {
    const auto& entry = simulation.edits().at(record.value);
    Event event{.time = record.time, .episode = record.episode, .protocol = "EDIT", .from = {}, .to = {}, .link = record.link};
    if (record.device.value) event.to = record.device;
    const auto before = entry.links.empty() ? nullptr : &entry.links.front();
    const auto name = entry.device ? entry.device->name : device_name(simulation, record.device);
    std::visit(Overloaded{
                   [&](const SetLinkUp& edit) {
                       event.summary = std::string("Cable ") + (edit.up ? "enabled" : "disabled") + " · " +
                           (before ? cable_text(simulation, *before) : "?");
                       event.detail = edit.up ? "Frames can cross it again."
                                              : "Frames on it are lost when they reach the end, and the ports at its ends "
                                                "have no carrier until it is enabled again.";
                   },
                   [&](const SetDelay& edit) {
                       event.summary = "Cable delay " + format_duration(edit.delay) + " · " + (before ? cable_text(simulation, *before) : "?");
                       event.detail = "Frames already on the cable keep their arrival time; the next ones take " +
                           format_ms(edit.delay) + ".";
                   },
                   [&](const SetPortUp& edit) {
                       event.summary = port_text(simulation, edit.port) + " " + state_text(edit.up);
                       event.detail = edit.up ? "The port can carry frames again if its cable is up."
                                              : "A disabled port has no carrier: neither it nor the port at the other end of its "
                                                "cable can send, and frames on the cable are lost.";
                   },
                   [&](const SetPowered& edit) {
                       event.summary = name + (edit.powered ? " powered on" : " powered off");
                       event.detail = edit.powered ? "It starts with empty tables, as after a reboot."
                                                   : "Its caches, tables and pings are gone; frames reaching it are dropped.";
                   },
                   [&](const SetAddress& edit) {
                       const auto old = entry.device && entry.device->ports.at(edit.port.port).ipv4
                           ? entry.device->ports.at(edit.port.port).ipv4->to_string() : std::string("?");
                       event.summary = name + " · address " + edit.address.to_string();
                       event.detail = "It was " + old + ". The PC does not announce the change: other hosts keep the old "
                                      "mapping in their ARP caches until the entry expires.";
                   },
                   [&](const SetAgeing& edit) {
                       event.summary = name + " · ageing time " + duration_text(edit.ageing);
                       event.detail = "MAC addresses not seen for " + duration_text(edit.ageing) + " are forgotten.";
                   },
                   [&](const Rename&) {},
                   [&](const MoveDevice&) {},
                   [&](const Connect& edit) {
                       event.summary = "Cable plugged · " + port_text(simulation, edit.a) + " — " + port_text(simulation, edit.b);
                       event.detail = "Frames take " + format_ms(edit.delay) + " from one end to the other.";
                   },
                   [&](const Disconnect&) {
                       event.summary = "Cable unplugged · " + (before ? cable_text(simulation, *before) : "?");
                       event.detail = "Frames on it are lost.";
                   },
                   [&](const Replug& edit) {
                       event.summary = "Cable moved · " + port_text(simulation, edit.from) + " → " + port_text(simulation, edit.to);
                       event.detail = "Frames on it are lost. Switches keep sending frames for the hosts behind it to the old "
                                      "port until a frame from them arrives on the new one or their entry ages out.";
                   },
                   [&](const AddDevice&) {
                       event.summary = device_name(simulation, record.device) + " added";
                       event.detail = "It starts with empty tables.";
                   },
                   [&](const RemoveDevice&) {
                       event.summary = name + " removed";
                       event.detail = "Its cables are unplugged and frames on them are lost.";
                   },
               },
               entry.edit);
    return event;
}

} // namespace

void Explainer::update(const Simulation& simulation) {
    const auto& records = simulation.trace().records();
    while (next_ < records.size()) {
        const auto episode = records[next_].episode;
        auto end = next_;
        while (end < records.size() && records[end].episode == episode) ++end;
        const std::span<const TraceRecord> group(records.data() + next_, end - next_);
        // An episode starts with a frame at a port, an edit or a timer.
        const auto& first = group.front();
        if (first.link && (first.kind == TraceKind::FrameReceived || first.kind == TraceKind::FrameDropped)) {
            arrival(simulation, group, events_);
        } else if (first.kind == TraceKind::EditApplied) {
            events_.push_back(edit_event(simulation, first));
            timers(simulation, group.subspan(1), events_);
        } else {
            timers(simulation, group, events_);
        }
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
    case Problem::InvalidAgeing: return "A switch's ageing time goes from 10 s to 1,000,000 s.";
    case Problem::TooManyDevices: return "A network holds at most 100 devices.";
    case Problem::NoFreePort: return "No free port: a PC has one port, a switch has eight.";
    case Problem::NotAHost: return "Select a PC to configure IPv4.";
    case Problem::NotASwitch: return "Only switches have an ageing time.";
    case Problem::SourceNotHost: return "Select a PC as the ping source.";
    case Problem::SourcePoweredOff: return "The source PC is powered off.";
    case Problem::InvalidDestination: return "Enter a valid unicast IPv4 address.";
    case Problem::BroadcastDestination: return "Pinging network or broadcast addresses is not supported.";
    case Problem::InvalidPingOptions:
        return "A ping sends 1 to 1000 requests; its interval and timeout go from 10 ms to 60 s.";
    }
    return "Unknown problem.";
}

std::string describe(Reason reason) {
    switch (reason) {
    case Reason::None: return {};
    case Reason::LinkDown: return "the cable stopped carrying frames while the frame was on it";
    case Reason::DeviceDown: return "the device is powered off";
    case Reason::NoCarrier: return "the port has no active cable";
    case Reason::NotForUs: return "the destination MAC belongs to another device";
    case Reason::ArpForOtherHost: return "the ARP request asks for another address";
    case Reason::Ipv4NotForUs: return "the destination IP belongs to another device";
    case Reason::NoRoute: return "the address is outside the local subnet, and there are no routers yet";
    case Reason::ArpUnresolved: return "three ARP requests went unanswered";
    case Reason::EchoTimeout: return "no Echo Reply within the timeout";
    case Reason::ArpQueueFull: return "newer datagrams for the same address took its place in the ARP queue";
    case Reason::UnmatchedReply: return "no request is waiting for this Echo Reply";
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
    case TraceKind::PingReply: return "PingReply";
    case TraceKind::PingLost: return "PingLost";
    case TraceKind::ArpEntryExpired: return "ArpEntryExpired";
    case TraceKind::MacEntryAged: return "MacEntryAged";
    case TraceKind::DatagramDropped: return "DatagramDropped";
    case TraceKind::EditApplied: return "EditApplied";
    case TraceKind::SimulatorGuard: return "SimulatorGuard";
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
    case Reason::ArpUnresolved: return "ArpUnresolved";
    case Reason::EchoTimeout: return "EchoTimeout";
    case Reason::ArpQueueFull: return "ArpQueueFull";
    case Reason::UnmatchedReply: return "UnmatchedReply";
    }
    return "Unknown";
}

} // namespace netsim::explain

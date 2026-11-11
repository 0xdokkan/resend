#include "explain/explain.hpp"

#include "core/text.hpp"
#include "sim/routing.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <span>

namespace netsim::explain {
namespace {
using namespace std::chrono_literals;
using text::tr;
static_assert(arp_retry_interval == 1s && arp_attempts == 3 && arp_cache_timeout == 60s,
              "the texts below describe these timers");

template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

std::string type_name(PduKind kind) {
    switch (kind) {
    case PduKind::ArpRequest: return tr("ARP Request");
    case PduKind::ArpReply: return tr("ARP Reply");
    case PduKind::EchoRequest: return tr("Echo Request");
    case PduKind::EchoReply: return tr("Echo Reply");
    case PduKind::TimeExceeded: return tr("Time Exceeded");
    case PduKind::Unreachable: return tr("Destination Unreachable");
    case PduKind::OtherIcmp: break;
    }
    return tr("ICMP message");
}

// An ICMP error by its type and code: "Destination Unreachable (host)".
std::string error_name(const Ipv4Packet& packet) {
    const auto& message = packet.icmp;
    if (message.type == icmp::destination_unreachable && message.code == icmp::net_unreachable) return tr("Destination Unreachable (network)");
    if (message.type == icmp::destination_unreachable && message.code == icmp::host_unreachable) return tr("Destination Unreachable (host)");
    return type_name(kind_of(EthernetFrame{{}, {}, packet}));
}

bool is_router(const Simulation& simulation, DeviceId id) {
    const auto* device = simulation.known_device(id);
    return device && device->kind == DeviceKind::Router;
}

std::string device_name(const Simulation& simulation, std::optional<DeviceId> id) {
    if (!id) return "?";
    const auto* device = simulation.known_device(*id);
    return device ? device->name : tr("device {0}", {std::to_string(id->value)});
}

std::string port_name(std::optional<std::uint8_t> port) { return port ? std::to_string(*port + 1) : "?"; }

bool about_ping(TraceKind kind) {
    return kind == TraceKind::PingStarted || kind == TraceKind::PingReply || kind == TraceKind::PingLost ||
        kind == TraceKind::PingSucceeded || kind == TraceKind::PingFailed;
}

std::string port_text(const Simulation& simulation, PortRef port) {
    return tr("{0} port {1}", {device_name(simulation, port.device), std::to_string(port.port + 1)});
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
    std::ostringstream loss;
    loss << (sent ? static_cast<double>(sent - replies) * 100.0 / static_cast<double>(sent) : 0.0);
    const auto requests = std::to_string(sent), answers = std::to_string(replies);
    std::string out = (sent == 1 ? tr("{0} request", {requests}) : tr("{0} requests", {requests})) + ", " +
        (replies == 1 ? tr("{0} reply", {answers}) : tr("{0} replies", {answers})) + ", " + tr("{0}% loss.", {loss.str()});
    SimTime low = SimTime::max(), high{}, total{};
    for (const auto& echo : ping.echoes) {
        if (echo.state != EchoState::Replied) continue;
        const auto rtt = echo.finished - echo.sent;
        low = std::min(low, rtt);
        high = std::max(high, rtt);
        total += rtt;
    }
    if (replies) out += " " + tr("Round trip min/avg/max: {0}/{1}/{2} ms.", {ms(low), ms(total / static_cast<std::int64_t>(replies)), ms(high)});
    return out;
}

// A traceroute's hops, one line each, as traceroute prints them:
// "2  10.0.2.1  40.000 ms", or "3  *" for a hop that did not answer.
std::string hops(const PingStatus& ping) {
    std::string out;
    for (const auto& echo : ping.echoes) {
        if (!out.empty()) out += "\n";
        out += std::to_string(echo.sequence) + "  ";
        if (echo.state == EchoState::Waiting) out += "…";
        else if (echo.responder == Ipv4Address{}) out += "*";
        else out += echo.responder.to_string() + "  " + format_ms(echo.finished - echo.sent);
    }
    return out;
}

std::string loss_detail(const Simulation& simulation, Reason reason, const PingStatus* ping, const EchoStatus* echo = nullptr) {
    const auto destination = ping ? ping->destination.to_string() : std::string("?");
    const auto responder = echo && echo->responder != Ipv4Address{} ? echo->responder.to_string() : std::string("?");
    switch (reason) {
    case Reason::NoRoute:
        return tr("The destination {0} is outside the local subnet and the PC has no default gateway, so the request has "
                  "nowhere to go and no ARP request is sent. Give the PC a gateway: a router's address on its subnet.", {destination});
    case Reason::GatewayOffLink: {
        const auto* source = ping ? simulation.known_device(ping->source) : nullptr;
        const auto gateway = source && source->gateway ? source->gateway->to_string() : std::string("?");
        return tr("The destination {0} is outside the local subnet, and so is the default gateway {1}: the PC can reach only "
                  "its own subnet. The gateway must be another address of the PC's subnet, usually a router's.", {destination, gateway});
    }
    case Reason::TtlExceeded:
        return tr("{0} discarded the request because its TTL ran out, and sent back a Time Exceeded. The datagram went round a "
                  "routing loop, or the path is longer than the TTL.", {responder});
    case Reason::NetUnreachable:
        return tr("{0} has no route to {1} and sent back a Destination Unreachable (network). Check that router's routes, "
                  "or its default route.", {responder, destination});
    case Reason::HostUnreachable:
        return tr("{0} got no answer to its ARP requests for {1} and sent back a Destination Unreachable (host): no device "
                  "on that network has the address, or it is off.", {responder, destination});
    case Reason::ArpUnresolved: {
        // Through the gateway, the PC was asking for the gateway.
        const auto* source = ping ? simulation.known_device(ping->source) : nullptr;
        const auto& address = source && !source->ports.empty() ? source->ports.front().ipv4 : std::nullopt;
        if (source && source->gateway && address && !address->contains(ping->destination))
            return tr("No reply after three ARP requests for the default gateway {0}: nobody on the subnet has that address. "
                      "Check the PC's gateway, and that the router is on and cabled. No ICMP was sent.", {source->gateway->to_string()});
        return tr("No reply after three ARP requests. Check the address, the cables and that the devices are on. "
                  "No ICMP was sent.");
    }
    case Reason::ArpQueueFull:
        return tr("More datagrams were waiting for the same ARP answer than the PC keeps ({0}): the oldest was discarded "
                  "to keep the latest.", {std::to_string(arp_queue_limit)});
    case Reason::DeviceDown:
        return tr("The PC was powered off or removed: its pings stop and their requests get no reply.");
    case Reason::NoAddress:
        return tr("The PC has no IPv4 address, so it has nothing to send the request from: nothing leaves it, not even an ARP "
                  "request. Give it an address and a prefix length in the inspector.");
    default:
        return tr("The MAC is known but no Echo Reply arrived. Check the subnet and the return path too, and whether "
                  "the cached MAC still belongs to that address.");
    }
}

std::string loss_summary(Reason reason) {
    switch (reason) {
    case Reason::NoRoute: return tr("no route");
    case Reason::GatewayOffLink: return tr("gateway outside the subnet");
    case Reason::TtlExceeded: return tr("TTL exceeded");
    case Reason::NetUnreachable: return tr("network unreachable");
    case Reason::HostUnreachable: return tr("host unreachable");
    case Reason::ArpUnresolved: return tr("ARP resolution timed out");
    case Reason::ArpQueueFull: return tr("ARP queue full");
    case Reason::DeviceDown: return tr("the PC is off");
    case Reason::NoAddress: return tr("no IPv4 address");
    default: return tr("reply timed out");
    }
}

std::string loss_protocol(Reason reason) {
    switch (reason) {
    case Reason::NoRoute:
    case Reason::GatewayOffLink:
    case Reason::NoAddress: return "IPv4";
    case Reason::ArpUnresolved:
    case Reason::ArpQueueFull: return "ARP";
    default: return "ICMP";
    }
}

Event guard_event(const Simulation& simulation, const TraceRecord& record) {
    Event event{.time = record.time, .episode = record.episode, .protocol = "SIMULATOR", .from = record.device, .pdu = record.pdu};
    event.summary = tr("Storm protection · frame stopped at {0}", {device_name(simulation, record.device)});
    event.detail = tr("This {0} was sent {1} times: more often than a network without loops could ever send it. The network "
                      "has an Ethernet loop and nothing stops the copies going round, so the simulator removed them all. "
                      "Real switches would keep flooding; spanning tree (STP) prevents this and is not implemented yet.",
                      {type_name(kind_of(simulation.trace().pdu(*record.pdu))), std::to_string(record.value)});
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
        src_ip + " → " + dst_ip + (packet ? ", TTL " + std::to_string(packet->ttl) : std::string()) + "\n" +
        tr("{0}; ingress port {1}.", {type, port_name(first.port)});
    if (packet && packet->icmp.original) {
        const auto& original = *packet->icmp.original;
        detail += "\n" + tr("It is about this datagram: {0} {1} → {2}, TTL {3}.",
                              {type_name(kind_of(EthernetFrame{{}, {}, unquote(original)})), original.src.to_string(),
                               original.dst.to_string(), std::to_string(original.ttl)});
    }
    const bool router = is_router(simulation, first.device);
    if (first.kind == TraceKind::FrameDropped) {
        detail += "\n" + (first.reason == Reason::LinkDown
                               ? tr("Lost on the way: the cable was disabled, unplugged or moved while the frame was on it.")
                               : tr("Cannot be received: the device is powered off."));
        event.success = false;
        events.push_back(std::move(event));
        return;
    }

    std::vector<Event> guards;
    bool resolved = false;
    // A router forwarding the datagram, or sending an error about it: the
    // next frame it builds is that datagram, or the ARP request it waits for.
    bool routed = false, erring = false;
    // An ARP answer releases the datagrams waiting for it.
    const auto released = [&](std::size_t from) {
        return std::count_if(records.begin() + static_cast<std::ptrdiff_t>(from), records.end(), [&](const auto& item) {
            if (!item.pdu || item.pdu == first.pdu || item.kind != TraceKind::FrameSent) return false;
            return !is_arp(kind_of(trace.pdu(*item.pdu)));
        });
    };
    for (std::size_t i = 1; i < records.size(); ++i) {
        const auto& record = records[i];
        // A frame this device builds in response, sent or not.
        const bool response = record.pdu && record.pdu != first.pdu &&
            (record.kind == TraceKind::FrameSent || (record.kind == TraceKind::FrameDropped && record.reason == Reason::NoCarrier));
        if (response) {
            const auto& built = trace.pdu(*record.pdu);
            const auto out = port_name(record.port);
            const auto kind = kind_of(built);
            if (kind == PduKind::ArpReply) {
                detail += "\n" + (router ? tr("This router owns the requested IP: it sends a unicast ARP reply.")
                                          : tr("This PC owns the requested IP: it sends a unicast ARP reply."));
            } else if (kind == PduKind::ArpRequest) {
                const auto target = built.arp()->target_ip.to_string();
                if (routed || erring)
                    detail += "\n" + (routed ? tr("The next hop's MAC address is not in the ARP cache: the datagram waits while an "
                                                   "ARP request asks for {0} on port {1}.", {target, out})
                                              : tr("The ICMP error waits while an ARP request asks for {0} on port {1}.", {target, out}));
                else if (packet && built.arp()->target_ip != packet->src)
                    detail += "\n" + tr("ICMP Echo Request received from another subnet: the Echo Reply goes to the default "
                                         "gateway, and waits while an ARP request asks for {0}.", {target});
                else
                    detail += "\n" + tr("ICMP Echo Request received, but there is no ARP entry for the sender: the Echo Reply "
                                         "waits while an ARP request asks for {0}.", {target});
                routed = erring = false;
            } else if (arp) {
                if (!resolved) {
                    const auto count = released(i);
                    detail += "\n" + (count > 1 ? tr("MAC resolved: the {0} IPv4 datagrams waiting for it can be sent.", {std::to_string(count)})
                                       : kind == PduKind::EchoRequest
                                       ? tr("MAC resolved: the IPv4 datagram carrying the ICMP Echo Request can be sent.")
                                       : kind == PduKind::EchoReply ? tr("MAC resolved: the waiting Echo Reply can be sent.")
                                                                    : tr("MAC resolved: the waiting {0} can be sent.", {type_name(kind)}));
                    resolved = true;
                }
            } else if (routed) {
                detail += "\n" + tr("Forwarded from port {0} to {1}.", {out, built.dst.to_string()});
                routed = false;
            } else if (is_icmp_error(kind)) {
                detail += "\n" + tr("{0} sent back to {1} from port {2}.", {error_name(*built.ipv4()), built.ipv4()->dst.to_string(), out});
                erring = false;
            } else if (kind == PduKind::EchoReply) {
                detail += "\n" + tr("ICMP Echo Request received: an Echo Reply with the same identifier is generated.");
            }
            continue;
        }
        switch (record.kind) {
        case TraceKind::MacLearned:
            detail += "\n" + tr("Learned source MAC {0} on port {1}.", {frame.src.to_string(), port_name(first.port)});
            break;
        case TraceKind::MacEntryAged: {
            const auto* device = simulation.known_device(record.device);
            detail += "\n" + tr("The entry for {0} had aged out: no frame from it for {1}.",
                                 {frame.dst.to_string(), duration_text(device ? device->mac_ageing : default_mac_ageing)});
            break;
        }
        case TraceKind::FrameFlooded: {
            const auto copies = std::count_if(records.begin() + static_cast<std::ptrdiff_t>(i) + 1, records.end(), [&](const auto& item) {
                return item.kind == TraceKind::FrameSent && item.pdu == first.pdu;
            });
            const auto number = std::to_string(copies);
            detail += "\n" + (copies == 0 ? tr("To be flooded, but no other port has an active cable.")
                               : copies == 1 ? tr("Flooded to every active port except the ingress port: {0} copy.", {number})
                                             : tr("Flooded to every active port except the ingress port: {0} copies.", {number}));
            break;
        }
        case TraceKind::FrameFiltered:
            detail += "\n" + tr("Frame filtered: the destination is on the ingress port.");
            break;
        case TraceKind::FrameForwarded:
            // The next record says whether the copy left the egress port.
            if (i + 1 < records.size() && records[i + 1].kind != TraceKind::SimulatorGuard) {
                const auto& out = records[++i];
                detail += "\n" + (out.kind == TraceKind::FrameSent ? tr("Unicast on port {0}.", {port_name(out.port)})
                                                                     : tr("Unicast on port {0}: the port has no active cable.", {port_name(out.port)}));
            }
            break;
        case TraceKind::ArpCacheUpdated:
            if (arp) detail += "\n" + tr("ARP cache updated: {0} = {1}.", {src_ip, arp->sender_mac.to_string()});
            break;
        case TraceKind::ArpEntryExpired:
            detail += "\n" + tr("The ARP entry for {0} has expired: no update for 60 s.", {Ipv4Address(record.value).to_string()});
            break;
        case TraceKind::DatagramRouted: {
            routed = true;
            const auto hop = Ipv4Address(record.value);
            const auto egress = i + 1 < records.size() ? port_name(records[i + 1].port) : std::string("?");
            const auto route = record.route ? record.route->to_string() : std::string("?");
            detail += "\n" + (hop == packet->dst ? tr("Route: {0} is on the connected network {1}, port {2}.", {dst_ip, route, egress})
                                                  : tr("Route: {0} via {1}, port {2}.", {route, hop.to_string(), egress}));
            // The shorter routes that match too: the longest prefix wins.
            std::string shorter;
            int others = 0;
            if (const auto* device = simulation.known_device(record.device); device && record.route) {
                for (const auto& entry : routing_table(*device)) {
                    if (!entry.active() || !entry.destination.contains(packet->dst) || entry.destination.length() >= record.route->length()) continue;
                    shorter += (shorter.empty() ? "" : ", ") + entry.destination.to_string();
                    ++others;
                }
            }
            if (others == 1) detail += " " + tr("It is the longest prefix that matches: {0} matches too, but is shorter.", {shorter});
            if (others > 1) detail += " " + tr("It is the longest prefix that matches: {0} match too, but are shorter.", {shorter});
            detail += " " + tr("TTL {0} → {1}.", {std::to_string(packet->ttl), std::to_string(packet->ttl - 1)});
            break;
        }
        case TraceKind::FrameDropped:
            switch (record.reason) {
            case Reason::NotForUs:
                detail += "\n" + tr("Frame ignored: the destination MAC is not the local MAC.");
                break;
            case Reason::NoAddress:
                detail += "\n" + tr("Frame ignored: the port has no IPv4 address.");
                event.success = false;
                break;
            case Reason::UnmatchedError:
                detail += "\n" + tr("ICMP error ignored: no request is waiting for it. Its request already timed out, or its ping ended.");
                event.success = false;
                break;
            case Reason::GatewayOffLink: {
                const auto* device = simulation.known_device(record.device);
                detail += "\n" + tr("Echo received from another subnet, but the default gateway {0} is not on the local subnet: "
                                     "no reply can be sent.", {device && device->gateway ? device->gateway->to_string() : std::string("?")});
                event.success = false;
                break;
            }
            case Reason::ArpForOtherHost:
                detail += "\n" + tr("Request for another address: no reply.");
                break;
            case Reason::Ipv4NotForUs:
                detail += "\n" + tr("Datagram ignored: the destination IP is not the local IP.");
                event.success = false;
                break;
            case Reason::UnmatchedReply:
                detail += "\n" + tr("Echo Reply ignored: no request is waiting for it. Its request already timed out, or its ping ended.");
                event.success = false;
                break;
            case Reason::NoRoute:
                detail += "\n" + tr("Echo received, but there is no return route: the source is outside the local subnet "
                                     "and the PC has no default gateway.");
                event.success = false;
                break;
            default:
                break;
            }
            break;
        case TraceKind::DatagramDropped:
            event.success = false;
            if (!router) {
                detail += "\n" + tr("An Echo Reply waiting for ARP was discarded: {0}.", {describe(record.reason)});
                break;
            }
            switch (record.reason) {
            case Reason::TtlExceeded:
                detail += "\n" + tr("TTL {0}: forwarding would take it to 0, so the router discards the datagram (RFC 1812 §5.3.1).",
                                     {std::to_string(packet->ttl)});
                erring = true;
                break;
            case Reason::NoRoute:
                detail += "\n" + (record.pdu ? tr("No route matches {0}, and there is no default route: the router discards the datagram.", {dst_ip})
                                              : tr("The router has no route back to {0}: the ICMP error cannot be sent.",
                                                   {Ipv4Address(record.value).to_string()}));
                erring = record.pdu.has_value();
                break;
            case Reason::DirectedBroadcast:
                detail += "\n" + tr("{0} is the broadcast address of a network of this router: routers do not forward directed "
                                     "broadcasts (RFC 2644).", {dst_ip});
                break;
            case Reason::ArpQueueFull:
                detail += "\n" + tr("More datagrams were waiting for the same ARP answer than the router keeps ({0}): the oldest "
                                     "was discarded, without an ICMP error.", {std::to_string(arp_queue_limit)});
                break;
            default:
                detail += "\n" + tr("The datagram was discarded: {0}.", {describe(record.reason)});
                break;
            }
            if (erring && packet && icmp::is_error(packet->icmp.type)) {
                detail += " " + tr("No ICMP error is sent about an ICMP error (RFC 1122 §3.2.2).");
                erring = false;
            }
            break;
        case TraceKind::PingReply: {
            const auto* ping = simulation.ping(PingId{record.value});
            const auto* echo = echo_of(ping, record);
            const auto rtt = echo ? ms(echo->finished - echo->sent) + " ms" : std::string("?");
            if (ping && ping->options.traceroute) {
                const auto hop = echo ? std::to_string(echo->sequence) : std::string("?");
                event.summary = tr("Hop {0} · {1} reached", {hop, src_ip});
                detail += "\n" + tr("Echo Reply from the destination itself: it is {0} hops away. Round trip: {1}.", {hop, rtt});
            } else if (ping && ping->options.count > 1) {
                const auto number = echo ? std::to_string(echo->sequence) : std::string("?");
                event.summary = tr("Reply {0}/{1} · {2} → {3}", {number, std::to_string(ping->options.count), src_ip, device_name(simulation, event.to)});
                detail += "\n" + tr("Echo Reply for request {0} of the ping. Round trip: {1}.", {number, rtt});
            } else {
                event.summary = tr("Ping succeeded · {0} → {1}", {src_ip, device_name(simulation, event.to)});
                detail += "\n" + tr("Echo Reply matching the current ping. The destination is reachable. Round trip: {0}.", {rtt});
            }
            break;
        }
        case TraceKind::PingLost: {
            // An ICMP error about a request.
            const auto* ping = simulation.ping(PingId{record.value});
            const auto* echo = echo_of(ping, record);
            const auto number = echo ? std::to_string(echo->sequence) : std::string("?");
            const bool hop = ping && ping->options.traceroute && record.reason == Reason::TtlExceeded;
            event.success = hop;
            if (hop) {
                event.summary = tr("Hop {0} · {1}", {number, src_ip});
                detail += "\n" + tr("Time Exceeded from {0}: the request sent with TTL {1} ran out there, so {0} is hop {1} "
                                     "on the way to {2}.", {src_ip, number, ping->destination.to_string()});
                break;
            }
            if (ping && ping->options.traceroute) event.summary = tr("Hop {0} · {1} · {2}", {number, src_ip, loss_summary(record.reason)});
            else if (ping && ping->options.count > 1)
                event.summary = tr("Request {0}/{1} lost · {2}", {number, std::to_string(ping->options.count), loss_summary(record.reason)});
            else event.summary = tr("Ping failed · {0}", {loss_summary(record.reason)});
            detail += "\n" + loss_detail(simulation, record.reason, ping, echo);
            break;
        }
        case TraceKind::PingSucceeded:
        case TraceKind::PingFailed: {
            const auto* ping = simulation.ping(PingId{record.value});
            if (ping && ping->options.traceroute) detail += "\n" + tr("Traceroute finished:") + "\n" + hops(*ping);
            else if (ping && ping->options.count > 1) detail += "\n" + tr("Ping finished: {0}", {statistics(*ping)});
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
    if (ping && ping->options.traceroute) {
        event.protocol = succeeded ? "ICMP" : loss_protocol(record.reason);
        event.summary = succeeded ? tr("Traceroute finished · {0} reached", {ping->destination.to_string()})
                                  : tr("Traceroute failed · {0}", {loss_summary(record.reason)});
        event.detail = tr("Traceroute from {0} to {1}:", {device_name(simulation, record.device), ping->destination.to_string()}) + "\n" + hops(*ping);
        if (!succeeded) {
            const auto& last = ping->echoes.empty() ? nullptr : &ping->echoes.back();
            event.detail += "\n" + (record.reason == Reason::TtlExceeded || record.reason == Reason::EchoTimeout
                                         ? tr("No answer from {0} within {1} hops.", {ping->destination.to_string(), std::to_string(ping->options.count)})
                                         : loss_detail(simulation, record.reason, ping, last));
        }
    } else if (single && succeeded) {
        event.protocol = "ICMP";
        event.summary = tr("Local ping succeeded");
        event.detail = tr("The destination is the PC's own address: no frame crosses the cables.");
        event.to = record.device;
    } else if (single) {
        event.protocol = loss_protocol(record.reason);
        event.summary = tr("Ping failed · {0}", {loss_summary(record.reason)});
        event.detail = loss_detail(simulation, record.reason, ping);
    } else {
        event.protocol = "ICMP";
        event.summary = succeeded ? tr("Ping finished · {0}/{1} replies", {std::to_string(ping->replies()), std::to_string(ping->echoes.size())})
                                  : tr("Ping failed · no reply");
        event.detail = tr("Ping from {0} to {1}: {2}", {device_name(simulation, record.device), ping->destination.to_string(), statistics(*ping)});
        if (!succeeded) event.detail += "\n" + loss_detail(simulation, record.reason, ping);
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
            const auto target = trace.pdu(*record.pdu).arp()->target_ip;
            const auto* device = simulation.known_device(record.device);
            // A ping that starts in this episode, if any.
            const auto started = std::find_if(records.begin(), records.end(), [](const auto& item) { return item.kind == TraceKind::PingStarted; });
            const auto* starting = started != records.end() ? simulation.ping(PingId{started->value}) : nullptr;
            const bool via_gateway = device && device->kind == DeviceKind::Host && device->gateway == target &&
                (!starting || starting->destination != target);
            event.protocol = "ARP";
            event.summary = tr("{0} · who has {1}?", {source, target.to_string()});
            event.detail = tr("ARP Request sent to broadcast ({0}). Attempt {1}/{2}.",
                              {MacAddress::broadcast().to_string(), std::to_string(record.value), std::to_string(arp_attempts)}) + "\n" +
                (!sent ? tr("The port has no active link: no frame can leave.")
                 : device && device->kind == DeviceKind::Router ? tr("Looking for the MAC address of the next hop on port {0}.", {port_name(record.port)})
                 : via_gateway ? tr("The destination is outside the local subnet: the datagram goes to the default gateway. Looking for its MAC address.")
                               : tr("The destination is in the local subnet. Looking for its MAC address."));
            event.pdu = record.pdu;
            event.success = sent;
        } else if (record.kind == TraceKind::ArpTimeout && record.value < arp_attempts) {
            event.protocol = "ARP";
            event.summary = tr("No ARP reply · retrying");
            event.detail = is_router(simulation, record.device) ? tr("One simulated second has passed. The router repeats the ARP request.")
                                                                 : tr("One simulated second has passed. The PC repeats the ARP request.");
        } else if (record.kind == TraceKind::ArpEntryExpired) {
            event.protocol = "ARP";
            event.summary = tr("{0} · ARP entry for {1} expired", {source, Ipv4Address(record.value).to_string()});
            event.detail = tr("The entry was not refreshed for 60 s, so the PC no longer trusts it and asks again.");
        } else if (record.kind == TraceKind::PingReply && !single) {
            const auto* echo = echo_of(ping, record);
            event.protocol = "ICMP";
            event.summary = tr("Local reply {0}/{1}", {echo ? std::to_string(echo->sequence) : std::string("?"), std::to_string(ping->options.count)});
            event.detail = tr("The destination is the PC's own address: no frame crosses the cables.");
            event.to = record.device;
        } else if (record.kind == TraceKind::PingLost && ping && ping->options.traceroute) {
            const auto* echo = echo_of(ping, record);
            const auto number = echo ? std::to_string(echo->sequence) : std::string("?");
            event.protocol = loss_protocol(record.reason);
            if (record.reason == Reason::EchoTimeout) {
                event.summary = tr("Hop {0} · no answer", {number});
                event.detail = tr("Nothing came back within the timeout for the request with TTL {0}: traceroute shows * and "
                                  "tries the next hop.", {number});
            } else {
                event.summary = tr("Hop {0} · {1}", {number, loss_summary(record.reason)});
                event.detail = loss_detail(simulation, record.reason, ping, echo);
            }
            event.pdu = record.pdu;
            event.success = false;
        } else if (record.kind == TraceKind::PingLost && !single) {
            const auto* echo = echo_of(ping, record);
            event.protocol = loss_protocol(record.reason);
            event.summary = tr("Request {0}/{1} lost · {2}", {echo ? std::to_string(echo->sequence) : std::string("?"),
                                                              std::to_string(ping->options.count), loss_summary(record.reason)});
            event.detail = loss_detail(simulation, record.reason, ping, echo);
            event.pdu = record.pdu;
            event.success = false;
        } else if (record.kind == TraceKind::PingSucceeded || record.kind == TraceKind::PingFailed) {
            event = ping_end(simulation, record);
        } else if (record.kind == TraceKind::DatagramDropped && record.reason == Reason::NoRoute && !record.port) {
            event.protocol = "IPv4";
            event.summary = tr("{0} · no route to {1}", {source, Ipv4Address(record.value).to_string()});
            event.detail = tr("The router has no route back to {0}: the ICMP error cannot be sent.", {Ipv4Address(record.value).to_string()});
            event.success = false;
        } else if (record.kind == TraceKind::DatagramDropped && is_router(simulation, record.device)) {
            // The router's ARP requests for a next hop went unanswered, or
            // its port lost its address: each datagram waiting is dropped,
            // with a Destination Unreachable (host) back to its source.
            const auto destination = Ipv4Address(record.value).to_string();
            event.protocol = "ARP";
            event.summary = tr("{0} · datagram for {1} discarded", {source, destination});
            event.detail = record.reason == Reason::ArpUnresolved
                ? tr("Three ARP requests for the next hop went unanswered, so the router cannot deliver the datagram.")
                : tr("The datagram waited for ARP, but {0}.", {describe(record.reason)});
            const auto error = std::find_if(&record + 1, records.data() + records.size(), [&](const auto& item) {
                return item.kind == TraceKind::DatagramDropped || (item.pdu && !is_arp(kind_of(trace.pdu(*item.pdu))));
            });
            if (error != records.data() + records.size() && error->kind != TraceKind::DatagramDropped && error->pdu)
                event.detail += " " + tr("A Destination Unreachable (host) goes back to {0}.", {trace.pdu(*error->pdu).ipv4()->dst.to_string()});
            event.success = false;
        } else if (record.kind == TraceKind::DatagramDropped) {
            event.protocol = "ARP";
            event.summary = tr("{0} · Echo Reply to {1} discarded", {source, Ipv4Address(record.value).to_string()});
            event.detail = tr("The reply waited for ARP to find the requester's MAC address, but {0}.", {describe(record.reason)});
            event.success = false;
        } else if (record.kind == TraceKind::SimulatorGuard) {
            event = guard_event(simulation, record);
        } else {
            continue;
        }
        events.push_back(std::move(event));
    }
}

// An edit made while the simulation ran.
Event edit_event(const Simulation& simulation, const TraceRecord& record) {
    const auto& entry = simulation.edits().at(record.value);
    Event event{.time = record.time, .episode = record.episode, .protocol = "EDIT", .from = {}, .to = {}, .link = record.link};
    if (record.device.value) event.to = record.device;
    const auto before = entry.links.empty() ? nullptr : &entry.links.front();
    const auto name = entry.device ? entry.device->name : device_name(simulation, record.device);
    std::visit(Overloaded{
                   [&](const SetLinkUp& edit) {
                       const auto cable = before ? cable_text(simulation, *before) : std::string("?");
                       event.summary = edit.up ? tr("Cable enabled · {0}", {cable}) : tr("Cable disabled · {0}", {cable});
                       event.detail = edit.up ? tr("Frames can cross it again.")
                                              : tr("Frames on it are lost when they reach the end, and the ports at its ends "
                                                   "have no carrier until it is enabled again.");
                   },
                   [&](const SetDelay& edit) {
                       event.summary = tr("Cable delay {0} · {1}", {format_duration(edit.delay), before ? cable_text(simulation, *before) : "?"});
                       event.detail = tr("Frames already on the cable keep their arrival time; the next ones take {0}.", {format_ms(edit.delay)});
                   },
                   [&](const SetPortUp& edit) {
                       const auto port = port_text(simulation, edit.port);
                       event.summary = edit.up ? tr("{0} enabled", {port}) : tr("{0} disabled", {port});
                       event.detail = edit.up ? tr("The port can carry frames again if its cable is up.")
                                              : tr("A disabled port has no carrier: neither it nor the port at the other end of its "
                                                   "cable can send, and frames on the cable are lost.");
                   },
                   [&](const SetPowered& edit) {
                       event.summary = edit.powered ? tr("{0} powered on", {name}) : tr("{0} powered off", {name});
                       event.detail = edit.powered ? tr("It starts with empty tables, as after a reboot.")
                                                   : tr("Its caches, tables and pings are gone; frames reaching it are dropped.");
                   },
                   [&](const SetAddress& edit) {
                       const auto old = entry.device && edit.port.port < entry.device->ports.size() && entry.device->ports[edit.port.port].ipv4
                           ? entry.device->ports[edit.port.port].ipv4->to_string() : tr("none");
                       const auto address = edit.address ? edit.address->to_string() : tr("none");
                       if (entry.device && entry.device->kind == DeviceKind::Router) {
                           event.summary = tr("{0} · address {1}", {port_text(simulation, edit.port), address});
                           event.detail = tr("It was {0}. The router's connected networks, and the routes through them, change at once.", {old});
                       } else {
                           event.summary = tr("{0} · address {1}", {name, address});
                           event.detail = tr("It was {0}. The PC does not announce the change: other hosts keep the old "
                                             "mapping in their ARP caches until the entry expires.", {old});
                       }
                   },
                   [&](const SetAgeing& edit) {
                       event.summary = tr("{0} · ageing time {1}", {name, duration_text(edit.ageing)});
                       event.detail = tr("MAC addresses not seen for {0} are forgotten.", {duration_text(edit.ageing)});
                   },
                   [&](const Rename&) {},
                   [&](const MoveDevice&) {},
                   [&](const Connect& edit) {
                       event.summary = tr("Cable plugged · {0}", {port_text(simulation, edit.a) + " — " + port_text(simulation, edit.b)});
                       event.detail = tr("Frames take {0} from one end to the other.", {format_ms(edit.delay)});
                   },
                   [&](const Disconnect&) {
                       event.summary = tr("Cable unplugged · {0}", {before ? cable_text(simulation, *before) : "?"});
                       event.detail = tr("Frames on it are lost.");
                   },
                   [&](const Replug& edit) {
                       event.summary = tr("Cable moved · {0} → {1}", {port_text(simulation, edit.from), port_text(simulation, edit.to)});
                       event.detail = tr("Frames on it are lost. Switches keep sending frames for the hosts behind it to the old "
                                         "port until a frame from them arrives on the new one or their entry ages out.");
                   },
                   [&](const AddDevice&) {
                       event.summary = tr("{0} added", {device_name(simulation, record.device)});
                       event.detail = tr("It starts with empty tables.");
                   },
                   [&](const RemoveDevice&) {
                       event.summary = tr("{0} removed", {name});
                       event.detail = tr("Its cables are unplugged and frames on them are lost.");
                   },
                   [&](const RestoreLink& edit) {
                       event.summary = tr("Cable plugged back · {0}", {cable_text(simulation, edit.link)});
                       event.detail = tr("It is back as it was before it was unplugged. Frames take {0} from one end to the other.",
                                         {format_ms(edit.link.delay)});
                   },
                   [&](const SetGateway& edit) {
                       event.summary = tr("{0} · default gateway {1}", {name, edit.gateway ? edit.gateway->to_string() : tr("none")});
                       event.detail = edit.gateway ? tr("Datagrams for other subnets now go to {0}.", {edit.gateway->to_string()})
                                                   : tr("Without a default gateway the PC reaches only its own subnet.");
                   },
                   [&](const SetRoutes& edit) {
                       const auto count = edit.routes.size();
                       event.summary = tr("{0} · routes changed", {name});
                       event.detail = (count == 1 ? tr("The router now has 1 static route.")
                                                  : tr("The router now has {0} static routes.", {std::to_string(count)})) + " " +
                           tr("The next datagrams it forwards follow them.");
                   },
                   [&](const SetPorts& edit) {
                       event.summary = tr("{0} · {1} ports", {name, std::to_string(edit.ports.size())});
                       event.detail = tr("The router's ports changed; their connected networks and routes change with them.");
                   },
                   [&](const RestoreDevice& edit) {
                       const auto cables = edit.links.size();
                       event.summary = tr("{0} restored", {name});
                       event.detail = cables == 0 ? tr("It is back as it was before it was removed, with no cable, and starts with empty tables, as after a reboot.")
                           : cables == 1 ? tr("It is back as it was before it was removed, with its cable, and starts with empty tables, as after a reboot.")
                                         : tr("It is back as it was before it was removed, with its {0} cables, and starts with empty tables, as after a reboot.",
                                              {std::to_string(cables)});
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

std::string port_text(const Scenario& scenario, PortRef port) {
    const auto* device = scenario.device(port.device);
    return tr("{0} port {1}", {device ? device->name : tr("device {0}", {std::to_string(port.device.value)}), std::to_string(port.port + 1)});
}

std::string describe(Problem problem) {
    switch (problem) {
    case Problem::None: return {};
    case Problem::DuplicateDeviceId: return tr("Two devices have the same id.");
    case Problem::InvalidDeviceId: return tr("Device ids start from 1.");
    case Problem::InvalidName: return tr("Use a name of 1 to 48 characters, without control characters.");
    case Problem::WrongPortCount: return tr("A PC has one port, a switch eight, a router two to eight.");
    case Problem::MissingAddress: return tr("Every PC needs an IPv4 address.");
    case Problem::InvalidHostAddress:
        return tr("Invalid IPv4 host address: network, broadcast, loopback and multicast addresses are excluded.");
    case Problem::InvalidPrefix: return tr("IPv4 prefixes go from /1 to /32.");
    case Problem::AddressOnSwitch: return tr("Switch ports have no IPv4 address.");
    case Problem::DuplicateLinkId: return tr("Two cables have the same id.");
    case Problem::InvalidLinkId: return tr("Cable ids start from 1.");
    case Problem::NoSuchDevice: return tr("Device not found.");
    case Problem::NoSuchPort: return tr("Port not found.");
    case Problem::NoSuchLink: return tr("Link not found.");
    case Problem::PortInUse: return tr("That port already has a cable.");
    case Problem::SameDevice: return tr("Connect two different devices.");
    case Problem::InvalidDelay: return tr("A cable's delay goes from 1 µs to 10 s.");
    case Problem::InvalidAgeing: return tr("A switch's ageing time goes from 10 s to 1,000,000 s.");
    case Problem::TooManyDevices: return tr("A network holds at most 100 devices.");
    case Problem::NoFreePort: return tr("No free port: every port of the device has a cable.");
    case Problem::NotAHost: return tr("Select a PC to configure IPv4.");
    case Problem::NotASwitch: return tr("Only switches have an ageing time.");
    case Problem::NotARouter: return tr("Only routers have routes and a choice of ports.");
    case Problem::InvalidGateway: return tr("A default gateway is a unicast IPv4 address, and only PCs have one.");
    case Problem::InvalidRoute:
        return tr("A route goes to a network, with no host bits set, through a unicast next hop; only routers have routes.");
    case Problem::DuplicateRoute: return tr("Two routes go to the same network.");
    case Problem::TooManyRoutes: return tr("A router holds at most 32 static routes.");
    case Problem::Locked: return tr("The lab keeps this as it gave it.");
    case Problem::SourceNotHost: return tr("Select a PC as the ping source.");
    case Problem::SourcePoweredOff: return tr("The source PC is powered off.");
    case Problem::InvalidDestination: return tr("Enter a valid unicast IPv4 address.");
    case Problem::BroadcastDestination: return tr("Pinging network or broadcast addresses is not supported.");
    case Problem::InvalidPingOptions:
        return tr("A ping sends 1 to 1000 requests; its interval and timeout go from 10 ms to 60 s.");
    }
    return tr("Unknown problem.");
}

std::string describe(Reason reason) {
    switch (reason) {
    case Reason::None: return {};
    case Reason::LinkDown: return tr("the cable stopped carrying frames while the frame was on it");
    case Reason::DeviceDown: return tr("the device is powered off");
    case Reason::NoCarrier: return tr("the port has no active cable");
    case Reason::NotForUs: return tr("the destination MAC belongs to another device");
    case Reason::ArpForOtherHost: return tr("the ARP request asks for another address");
    case Reason::Ipv4NotForUs: return tr("the destination IP belongs to another device");
    case Reason::NoRoute: return tr("there is no route to the destination");
    case Reason::ArpUnresolved: return tr("three ARP requests went unanswered");
    case Reason::EchoTimeout: return tr("no Echo Reply within the timeout");
    case Reason::ArpQueueFull: return tr("newer datagrams for the same address took its place in the ARP queue");
    case Reason::UnmatchedReply: return tr("no request is waiting for this Echo Reply");
    case Reason::TtlExceeded: return tr("its TTL ran out at a router");
    case Reason::NetUnreachable: return tr("a router has no route to the destination");
    case Reason::HostUnreachable: return tr("the last router got no ARP reply from the destination");
    case Reason::GatewayOffLink: return tr("the default gateway is not on the local subnet");
    case Reason::NoAddress: return tr("the device has no IPv4 address on that port");
    case Reason::UnmatchedError: return tr("no request is waiting for this ICMP error");
    case Reason::DirectedBroadcast: return tr("routers do not forward to a network's broadcast address");
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
    case TraceKind::DatagramRouted: return "DatagramRouted";
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
    case Reason::TtlExceeded: return "TtlExceeded";
    case Reason::NetUnreachable: return "NetUnreachable";
    case Reason::HostUnreachable: return "HostUnreachable";
    case Reason::GatewayOffLink: return "GatewayOffLink";
    case Reason::NoAddress: return "NoAddress";
    case Reason::UnmatchedError: return "UnmatchedError";
    case Reason::DirectedBroadcast: return "DirectedBroadcast";
    }
    return "Unknown";
}

} // namespace netsim::explain

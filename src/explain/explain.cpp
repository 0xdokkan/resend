#include "explain/explain.hpp"

#include "core/text.hpp"

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
    case PduKind::OtherIcmp: break;
    }
    return tr("ICMP message");
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

std::string loss_detail(Reason reason, const PingStatus* ping) {
    switch (reason) {
    case Reason::NoRoute:
        return tr("The destination {0} is outside the local subnet. This prototype has no routers or gateways yet, "
                  "so no ARP request is sent for the remote host.", {ping ? ping->destination.to_string() : std::string("?")});
    case Reason::ArpUnresolved:
        return tr("No reply after three ARP requests. Check the address, the cables and that the devices are on. "
                  "No ICMP was sent.");
    case Reason::ArpQueueFull:
        return tr("More datagrams were waiting for the same ARP answer than the PC keeps ({0}): the oldest was discarded "
                  "to keep the latest.", {std::to_string(arp_queue_limit)});
    case Reason::DeviceDown:
        return tr("The PC was powered off or removed: its pings stop and their requests get no reply.");
    default:
        return tr("The MAC is known but no Echo Reply arrived. Check the subnet and the return path too, and whether "
                  "the cached MAC still belongs to that address.");
    }
}

std::string loss_summary(Reason reason) {
    switch (reason) {
    case Reason::NoRoute: return tr("no route");
    case Reason::ArpUnresolved: return tr("ARP resolution timed out");
    case Reason::ArpQueueFull: return tr("ARP queue full");
    case Reason::DeviceDown: return tr("the PC is off");
    default: return tr("reply timed out");
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
        src_ip + " → " + dst_ip + "\n" + tr("{0}; ingress port {1}.", {type, port_name(first.port)});
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
                detail += "\n" + tr("This PC owns the requested IP: it sends a unicast ARP reply.");
                break;
            case PduKind::EchoRequest:
            case PduKind::EchoReply:
                if (!arp) {
                    detail += "\n" + tr("ICMP Echo Request received: an Echo Reply with the same identifier is generated.");
                } else if (!resolved) {
                    const auto count = released(i);
                    detail += "\n" + (count > 1 ? tr("MAC resolved: the {0} IPv4 datagrams waiting for it can be sent.", {std::to_string(count)})
                                       : kind_of(built) == PduKind::EchoRequest
                                       ? tr("MAC resolved: the IPv4 datagram carrying the ICMP Echo Request can be sent.")
                                       : tr("MAC resolved: the waiting Echo Reply can be sent."));
                    resolved = true;
                }
                break;
            case PduKind::ArpRequest:
                detail += "\n" + tr("ICMP Echo Request received, but there is no ARP entry for the sender: the Echo Reply "
                                     "waits while an ARP request asks for {0}.", {built.arp()->target_ip.to_string()});
                break;
            case PduKind::OtherIcmp:
                break;
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
        case TraceKind::FrameDropped:
            switch (record.reason) {
            case Reason::NotForUs:
                detail += "\n" + tr("Frame ignored: the destination MAC is not the local MAC.");
                break;
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
                detail += "\n" + tr("Echo received, but there is no return route: the source is outside the local subnet. "
                                     "Routers and gateways are not implemented yet.");
                event.success = false;
                break;
            default:
                break;
            }
            break;
        case TraceKind::DatagramDropped:
            detail += "\n" + tr("An Echo Reply waiting for ARP was discarded: {0}.", {describe(record.reason)});
            event.success = false;
            break;
        case TraceKind::PingReply: {
            const auto* ping = simulation.ping(PingId{record.value});
            const auto* echo = echo_of(ping, record);
            const auto rtt = echo ? ms(echo->finished - echo->sent) + " ms" : std::string("?");
            if (ping && ping->options.count > 1) {
                const auto number = echo ? std::to_string(echo->sequence) : std::string("?");
                event.summary = tr("Reply {0}/{1} · {2} → {3}", {number, std::to_string(ping->options.count), src_ip, device_name(simulation, event.to)});
                detail += "\n" + tr("Echo Reply for request {0} of the ping. Round trip: {1}.", {number, rtt});
            } else {
                event.summary = tr("Ping succeeded · {0} → {1}", {src_ip, device_name(simulation, event.to)});
                detail += "\n" + tr("Echo Reply matching the current ping. The destination is reachable. Round trip: {0}.", {rtt});
            }
            break;
        }
        case TraceKind::PingSucceeded:
        case TraceKind::PingFailed: {
            const auto* ping = simulation.ping(PingId{record.value});
            if (ping && ping->options.count > 1) detail += "\n" + tr("Ping finished: {0}", {statistics(*ping)});
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
        event.summary = tr("Local ping succeeded");
        event.detail = tr("The destination is the PC's own address: no frame crosses the cables.");
        event.to = record.device;
    } else if (single) {
        event.protocol = loss_protocol(record.reason);
        event.summary = tr("Ping failed · {0}", {loss_summary(record.reason)});
        event.detail = loss_detail(record.reason, ping);
    } else {
        event.protocol = "ICMP";
        event.summary = succeeded ? tr("Ping finished · {0}/{1} replies", {std::to_string(ping->replies()), std::to_string(ping->echoes.size())})
                                  : tr("Ping failed · no reply");
        event.detail = tr("Ping from {0} to {1}: {2}", {device_name(simulation, record.device), ping->destination.to_string(), statistics(*ping)});
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
            event.summary = tr("{0} · who has {1}?", {source, trace.pdu(*record.pdu).arp()->target_ip.to_string()});
            event.detail = tr("ARP Request sent to broadcast ({0}). Attempt {1}/{2}.",
                              {MacAddress::broadcast().to_string(), std::to_string(record.value), std::to_string(arp_attempts)}) + "\n" +
                (sent ? tr("The destination is in the local subnet. Looking for its MAC address.")
                      : tr("The port has no active link: no frame can leave."));
            event.pdu = record.pdu;
            event.success = sent;
        } else if (record.kind == TraceKind::ArpTimeout && record.value < arp_attempts) {
            event.protocol = "ARP";
            event.summary = tr("No ARP reply · retrying");
            event.detail = tr("One simulated second has passed. The PC repeats the ARP request.");
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
        } else if (record.kind == TraceKind::PingLost && !single) {
            const auto* echo = echo_of(ping, record);
            event.protocol = loss_protocol(record.reason);
            event.summary = tr("Request {0}/{1} lost · {2}", {echo ? std::to_string(echo->sequence) : std::string("?"),
                                                              std::to_string(ping->options.count), loss_summary(record.reason)});
            event.detail = loss_detail(record.reason, ping);
            event.pdu = record.pdu;
            event.success = false;
        } else if (record.kind == TraceKind::PingSucceeded || record.kind == TraceKind::PingFailed) {
            event = ping_end(simulation, record);
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
                       const auto old = entry.device && entry.device->ports.at(edit.port.port).ipv4
                           ? entry.device->ports.at(edit.port.port).ipv4->to_string() : std::string("?");
                       event.summary = tr("{0} · address {1}", {name, edit.address.to_string()});
                       event.detail = tr("It was {0}. The PC does not announce the change: other hosts keep the old "
                                         "mapping in their ARP caches until the entry expires.", {old});
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
    case Problem::WrongPortCount: return tr("A PC has one port, a switch has eight.");
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
    case Problem::NoFreePort: return tr("No free port: a PC has one port, a switch has eight.");
    case Problem::NotAHost: return tr("Select a PC to configure IPv4.");
    case Problem::NotASwitch: return tr("Only switches have an ageing time.");
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
    case Reason::NoRoute: return tr("the address is outside the local subnet, and there are no routers yet");
    case Reason::ArpUnresolved: return tr("three ARP requests went unanswered");
    case Reason::EchoTimeout: return tr("no Echo Reply within the timeout");
    case Reason::ArpQueueFull: return tr("newer datagrams for the same address took its place in the ARP queue");
    case Reason::UnmatchedReply: return tr("no request is waiting for this Echo Reply");
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

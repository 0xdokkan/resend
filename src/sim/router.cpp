#include "sim/arp.hpp"
#include "sim/node.hpp"
#include "sim/routing.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace netsim {
namespace {

// A router (RFC 1812): an IPv4 interface on each port with an address, ARP
// on each, forwarding by the longest prefix match with the TTL decremented,
// ICMP errors back to the source of the datagrams it cannot forward, and
// the echo responder for its own addresses. It runs no pings.
class Router final : public Node {
public:
    void receive(Context& context, const Arrival& arrival) override {
        const auto& frame = context.frame(arrival.pdu);
        const Nic nic{arrival.port};
        if (!nic.accepts(context, frame)) {
            context.note(arrival, TraceKind::FrameDropped, Reason::NotForUs);
            return;
        }
        if (!context.config().ports.at(arrival.port).ipv4) {
            context.note(arrival, TraceKind::FrameDropped, Reason::NoAddress);
            return;
        }
        if (const auto* packet = frame.arp()) {
            const auto sender = packet->sender_mac;
            for (auto& outgoing : arp_[arrival.port].receive(context, nic, arrival, *packet))
                leave(context, arrival.port, std::move(outgoing), sender);
        } else {
            receive_ipv4(context, arrival, frame, *frame.ipv4());
        }
    }

    void timer(Context& context, const Timer& timer) override {
        if (timer.kind != TimerKind::ArpRetry) return;
        const auto port = static_cast<std::uint8_t>(timer.key >> 32);
        const auto next_hop = Arp::timer_address(timer.key);
        const auto& ports = context.config().ports;
        auto& resolver = arp_[port];
        if (port >= ports.size() || !ports[port].ipv4) {
            // The port lost its address, or the port itself is gone.
            for (const auto& outgoing : resolver.abandon(next_hop)) drop(context, port, outgoing, Reason::NoAddress);
            return;
        }
        for (const auto& outgoing : resolver.retry(context, Nic{port}, next_hop)) {
            drop(context, port, outgoing, Reason::ArpUnresolved);
            error(context, outgoing.packet, icmp::destination_unreachable, icmp::host_unreachable);
        }
    }

    std::vector<ArpCacheEntry> arp_cache(SimTime now) const override {
        std::vector<ArpCacheEntry> entries;
        for (const auto& [port, resolver] : arp_)
            for (const auto& entry : resolver.entries(now, port)) entries.push_back(entry);
        return entries;
    }

private:
    static bool owns(const DeviceConfig& config, Ipv4Address address) {
        return std::any_of(config.ports.begin(), config.ports.end(), [&](const auto& port) { return port.ipv4 && port.ipv4->address() == address; });
    }

    // The network or broadcast address of a connected network, which a
    // router does not forward to (RFC 2644) and sends no errors about.
    static bool directed_broadcast(const DeviceConfig& config, Ipv4Address address) {
        return std::any_of(config.ports.begin(), config.ports.end(), [&](const auto& port) {
            return port.ipv4 && port.ipv4->length() < 31 && port.ipv4->contains(address) &&
                (address == port.ipv4->network() || address == port.ipv4->broadcast());
        });
    }

    void receive_ipv4(Context& context, const Arrival& arrival, const EthernetFrame& frame, const Ipv4Packet& packet) {
        const auto& config = context.config();
        if (owns(config, packet.dst)) {
            local(context, arrival, packet);
            return;
        }
        if (directed_broadcast(config, packet.dst)) {
            drop(context, arrival, packet, Reason::DirectedBroadcast);
            return;
        }
        // As Linux does, the route comes first: a datagram without a route
        // is unreachable whatever its TTL.
        const auto route = lookup(routing_table(config), packet.dst);
        if (!route) {
            drop(context, arrival, packet, Reason::NoRoute);
            if (!frame.dst.is_broadcast()) error(context, packet, icmp::destination_unreachable, icmp::net_unreachable);
            return;
        }
        // RFC 1812 §5.3.1: a datagram that would leave with TTL 0 is
        // discarded, and the source told.
        if (packet.ttl <= 1) {
            drop(context, arrival, packet, Reason::TtlExceeded);
            if (!frame.dst.is_broadcast()) error(context, packet, icmp::time_exceeded, icmp::ttl_exceeded);
            return;
        }
        const auto hop = next_hop(*route, packet.dst);
        context.record({.kind = TraceKind::DatagramRouted, .port = arrival.port, .link = arrival.link, .pdu = arrival.pdu,
                        .value = hop.bits(), .route = route->destination});
        auto forwarded = packet;
        --forwarded.ttl;
        send(context, *route->port, hop, {forwarded, std::nullopt});
    }

    // A datagram for one of the router's addresses: Echo Requests get their
    // reply, from the address they were sent to.
    void local(Context& context, const Arrival& arrival, const Ipv4Packet& packet) {
        if (packet.protocol != ip_protocol::icmp) return;
        const auto& message = packet.icmp;
        if (message.type == icmp::echo_reply) {
            context.note(arrival, TraceKind::FrameDropped, Reason::UnmatchedReply);
            return;
        }
        if (icmp::is_error(message.type)) {
            context.note(arrival, TraceKind::FrameDropped, Reason::UnmatchedError);
            return;
        }
        if (message.type != icmp::echo_request) return;
        const IcmpMessage reply{icmp::echo_reply, 0, message.identifier, message.sequence, message.data_size};
        originate(context, {packet.dst, packet.src, default_ttl, ip_protocol::icmp, 0, reply}, false);
    }

    // An ICMP error about `original` back to its source, unless the rules of
    // RFC 1122 §3.2.2 and RFC 1812 §4.3.2.7 forbid one: never about an ICMP
    // error, a datagram to a broadcast address or from an address that is
    // not a single host's, or a datagram the router sent itself.
    void error(Context& context, const Ipv4Packet& original, std::uint8_t type, std::uint8_t code) {
        const auto& config = context.config();
        if (original.protocol == ip_protocol::icmp && icmp::is_error(original.icmp.type)) return;
        if (!is_unicast(original.dst) || !is_unicast(original.src) || directed_broadcast(config, original.src)) return;
        if (owns(config, original.src)) return;
        IcmpMessage message{type, code, 0, 0, 0, quote(original)};
        originate(context, {Ipv4Address{}, original.src, default_ttl, ip_protocol::icmp, 0, message}, true);
    }

    // A datagram the router itself sends. Its source, if `from_egress`, is
    // the address of the port it leaves from (RFC 1812 §4.3.2.4).
    void originate(Context& context, Ipv4Packet packet, bool from_egress) {
        const auto route = lookup(routing_table(context.config()), packet.dst);
        if (!route) {
            context.record({.kind = TraceKind::DatagramDropped, .reason = Reason::NoRoute, .value = packet.dst.bits()});
            return;
        }
        if (from_egress) packet.src = context.config().ports.at(*route->port).ipv4->address();
        packet.identification = ++identification_;
        send(context, *route->port, next_hop(*route, packet.dst), {packet, std::nullopt});
    }

    // Sends a datagram from `port` to `next_hop`, resolving it first if
    // needed. When the queue for that address is full the oldest datagram
    // is discarded without an error, as Linux does.
    void send(Context& context, std::uint8_t port, Ipv4Address next_hop, Outgoing outgoing) {
        const Nic nic{port};
        auto& resolver = arp_[port];
        if (const auto mac = resolver.lookup(context, nic, next_hop)) leave(context, port, std::move(outgoing), *mac);
        else if (const auto dropped = resolver.resolve(context, nic, next_hop, std::move(outgoing))) drop(context, port, *dropped, Reason::ArpQueueFull);
    }

    void leave(Context& context, std::uint8_t port, Outgoing outgoing, MacAddress next_hop) {
        const Nic nic{port};
        nic.send(context, {next_hop, nic.mac(context), outgoing.packet});
    }

    void drop(Context& context, const Arrival& arrival, const Ipv4Packet& packet, Reason reason) {
        context.record({.kind = TraceKind::DatagramDropped, .reason = reason, .port = arrival.port, .link = arrival.link,
                        .pdu = arrival.pdu, .value = packet.dst.bits()});
    }
    void drop(Context& context, std::uint8_t port, const Outgoing& outgoing, Reason reason) {
        context.record({.kind = TraceKind::DatagramDropped, .reason = reason, .port = port, .value = outgoing.packet.dst.bits()});
    }

    std::map<std::uint8_t, Arp> arp_;  // by port
    std::uint16_t identification_ = 0;  // of the last datagram the router sent itself
};

} // namespace

std::unique_ptr<Node> make_router() { return std::make_unique<Router>(); }

} // namespace netsim

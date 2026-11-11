#include "sim/arp.hpp"
#include "sim/node.hpp"
#include "sim/ping_app.hpp"

#include <utility>

namespace netsim {
namespace {

// A host with one interface: Ethernet, ARP, IPv4 with a default gateway for
// the other subnets (RFC 1122 §3.3.1), the ICMP echo responder (RFC 792)
// and the ping and traceroute commands.
class Host final : public Node {
public:
    void receive(Context& context, const Arrival& arrival) override {
        const auto& frame = context.frame(arrival.pdu);
        if (!nic_.accepts(context, frame)) {
            context.note(arrival, TraceKind::FrameDropped, Reason::NotForUs);
            return;
        }
        // Without an address the host takes no part in IPv4: no request is
        // for it, no datagram either.
        if (!has_address(context)) {
            context.note(arrival, TraceKind::FrameDropped, frame.arp() ? Reason::ArpForOtherHost : Reason::Ipv4NotForUs);
            return;
        }
        if (const auto* packet = frame.arp()) {
            const auto sender = packet->sender_mac;
            for (auto& outgoing : arp_.receive(context, nic_, arrival, *packet)) leave(context, std::move(outgoing), sender);
        } else {
            receive_ipv4(context, arrival, *frame.ipv4());
        }
    }

    void timer(Context& context, const Timer& timer) override {
        switch (timer.kind) {
        case TimerKind::PingSend:
            send_request(context, PingId{static_cast<std::uint32_t>(timer.key)});
            break;
        case TimerKind::ArpRetry:
            // The address was removed while the host was asking: it cannot ask again.
            if (!has_address(context)) {
                for (const auto& outgoing : arp_.abandon(Arp::timer_address(timer.key))) discard(context, outgoing, Reason::NoAddress);
                break;
            }
            for (const auto& outgoing : arp_.retry(context, nic_, Arp::timer_address(timer.key)))
                discard(context, outgoing, Reason::ArpUnresolved);
            break;
        case TimerKind::EchoTimeout:
            pings_.timeout(context, timer.key);
            break;
        }
    }

    std::vector<ArpCacheEntry> arp_cache(SimTime now) const override { return arp_.entries(now); }

private:
    void receive_ipv4(Context& context, const Arrival& arrival, const Ipv4Packet& packet) {
        const auto self = nic_.address(context);
        if (packet.dst != self.address()) {
            context.note(arrival, TraceKind::FrameDropped, Reason::Ipv4NotForUs);
            return;
        }
        if (packet.protocol != ip_protocol::icmp) return;
        const auto& message = packet.icmp;
        if (message.type == icmp::echo_reply) {
            if (!pings_.reply(context, arrival, packet.src, message)) context.note(arrival, TraceKind::FrameDropped, Reason::UnmatchedReply);
            return;
        }
        if (icmp::is_error(message.type)) {
            if (!pings_.error(context, arrival, packet)) context.note(arrival, TraceKind::FrameDropped, Reason::UnmatchedError);
            return;
        }
        if (message.type != icmp::echo_request) return;
        Reason reason{};
        const auto hop = next_hop(context, packet.src, reason);
        if (!hop) {
            context.note(arrival, TraceKind::FrameDropped, reason);
            return;
        }
        const IcmpMessage reply{icmp::echo_reply, 0, message.identifier, message.sequence, message.data_size};
        route(context, {Ipv4Packet{self.address(), packet.src, default_ttl, ip_protocol::icmp, 0, reply}, std::nullopt}, *hop);
    }

    bool has_address(const Context& context) const { return context.config().ports.at(nic_.port).ipv4.has_value(); }

    void send_request(Context& context, PingId id) {
        if (!has_address(context)) {
            // The request is numbered and lost at once: there is no source address to send it from.
            const auto outgoing = pings_.request(context, id, Ipv4Address{});
            context.echo_lost(id, outgoing.packet.icmp.sequence, Reason::NoAddress);
            return;
        }
        const auto self = nic_.address(context);
        auto outgoing = pings_.request(context, id, self.address());
        const auto sequence = outgoing.packet.icmp.sequence;
        const auto destination = outgoing.packet.dst;
        Reason reason{};
        if (destination == self.address()) {
            context.echo_replied(id, sequence, nullptr);
        } else if (const auto hop = next_hop(context, destination, reason)) {
            route(context, std::move(outgoing), *hop);
        } else {
            context.echo_lost(id, sequence, reason);
        }
    }

    // Where a datagram for `destination` goes first (RFC 1122 §3.3.1.1):
    // straight to it on the local subnet, otherwise to the default gateway,
    // which has to be another address of the subnet.
    std::optional<Ipv4Address> next_hop(const Context& context, Ipv4Address destination, Reason& reason) const {
        const auto self = nic_.address(context);
        if (self.contains(destination)) return destination;
        const auto& gateway = context.config().gateway;
        if (gateway && self.contains(*gateway) && *gateway != self.address()) return *gateway;
        reason = gateway ? Reason::GatewayOffLink : Reason::NoRoute;
        return std::nullopt;
    }

    // Sends a datagram to its next hop, resolving it first if needed.
    void route(Context& context, Outgoing outgoing, Ipv4Address next_hop) {
        if (const auto mac = arp_.lookup(context, nic_, next_hop)) leave(context, std::move(outgoing), *mac);
        else if (const auto dropped = arp_.resolve(context, nic_, next_hop, std::move(outgoing))) discard(context, *dropped, Reason::ArpQueueFull);
    }

    // A datagram that cannot leave: a ping request is lost, anything else
    // is recorded as dropped.
    void discard(Context& context, const Outgoing& outgoing, Reason reason) {
        if (outgoing.ping) context.echo_lost(*outgoing.ping, outgoing.packet.icmp.sequence, reason);
        else context.record({.kind = TraceKind::DatagramDropped, .reason = reason, .port = nic_.port, .value = outgoing.packet.dst.bits()});
    }

    // Datagrams are numbered as they leave, so the numbers have no gaps.
    void leave(Context& context, Outgoing outgoing, MacAddress next_hop) {
        outgoing.packet.identification = ++identification_;
        const auto pdu = nic_.send(context, {next_hop, nic_.mac(context), outgoing.packet});
        if (outgoing.ping) pings_.sent(context, *outgoing.ping, outgoing.packet.icmp.sequence, pdu);
    }

    Nic nic_{0};
    Arp arp_;
    PingApp pings_;
    std::uint16_t identification_ = 0;  // of the last IPv4 datagram sent
};

} // namespace

std::unique_ptr<Node> make_host() { return std::make_unique<Host>(); }

} // namespace netsim

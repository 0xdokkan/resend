#include "sim/arp.hpp"
#include "sim/node.hpp"
#include "sim/ping_app.hpp"

#include <utility>

namespace netsim {
namespace {

// A host with one interface: Ethernet, ARP, IPv4 limited to its own subnet,
// the ICMP echo responder (RFC 792) and the ping command.
class Host final : public Node {
public:
    void receive(Context& context, const Arrival& arrival) override {
        const auto& frame = context.frame(arrival.pdu);
        if (!nic_.accepts(context, frame)) {
            context.note(arrival, TraceKind::FrameDropped, Reason::NotForUs);
            return;
        }
        if (const auto* packet = frame.arp()) {
            const auto ready = arp_.receive(context, nic_, arrival, *packet);
            for (const auto& outgoing : ready) leave(context, outgoing, *arp_.lookup(packet->sender_ip));
        } else {
            receive_ipv4(context, arrival, *frame.ipv4());
        }
    }

    void timer(Context& context, const Timer& timer) override {
        const PingId ping{static_cast<std::uint32_t>(timer.key)};
        switch (timer.kind) {
        case TimerKind::PingStart:
            start_ping(context, ping);
            break;
        case TimerKind::ArpRetry:
            for (const auto& outgoing : arp_.retry(context, nic_, Arp::timer_address(timer.key)))
                if (outgoing.ping) context.finish_ping(*outgoing.ping, PingState::Failed, Reason::ArpUnresolved);
            break;
        case TimerKind::EchoTimeout:
            pings_.timeout(context, ping);
            break;
        }
    }

    std::vector<ArpCacheEntry> arp_cache() const override { return arp_.entries(); }

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
            pings_.reply(context, arrival, packet.src, message);
            return;
        }
        if (message.type != icmp::echo_request) return;
        if (!self.contains(packet.src)) {
            context.note(arrival, TraceKind::FrameDropped, Reason::NoRoute);
            return;
        }
        // The sender was learned from the ARP exchange that preceded its
        // request. Resolving it here instead is not modelled yet.
        const auto next_hop = arp_.lookup(packet.src);
        if (!next_hop) {
            context.note(arrival, TraceKind::FrameDropped, Reason::NotModelled);
            return;
        }
        const IcmpMessage reply{icmp::echo_reply, 0, message.identifier, message.sequence};
        leave(context, {Ipv4Packet{self.address(), packet.src, 64, ip_protocol::icmp, 0, reply}, std::nullopt}, *next_hop);
    }

    void start_ping(Context& context, PingId id) {
        context.record({.kind = TraceKind::PingStarted, .value = id.value});
        const auto self = nic_.address(context);
        const auto destination = context.ping(id).destination;
        if (destination == self.address()) {
            context.finish_ping(id, PingState::Succeeded, Reason::None);
            return;
        }
        if (!self.contains(destination)) {
            context.finish_ping(id, PingState::Failed, Reason::NoRoute);
            return;
        }
        Outgoing outgoing{Ipv4Packet{self.address(), destination, 64, ip_protocol::icmp, 0, PingApp::request(id)}, id};
        if (const auto next_hop = arp_.lookup(destination)) leave(context, outgoing, *next_hop);
        else arp_.resolve(context, nic_, destination, std::move(outgoing));
    }

    // Datagrams are numbered as they leave, so the numbers have no gaps.
    void leave(Context& context, Outgoing outgoing, MacAddress next_hop) {
        outgoing.packet.identification = ++identification_;
        nic_.send(context, {next_hop, nic_.mac(context), outgoing.packet});
        if (outgoing.ping) pings_.sent(context, *outgoing.ping);
    }

    Nic nic_{0};
    Arp arp_;
    PingApp pings_;
    std::uint16_t identification_ = 0;  // of the last IPv4 datagram sent
};

} // namespace

std::unique_ptr<Node> make_host() { return std::make_unique<Host>(); }

} // namespace netsim

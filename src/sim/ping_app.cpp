#include "sim/ping_app.hpp"

namespace netsim {

Outgoing PingApp::request(Context& context, PingId id, Ipv4Address source) {
    auto& echo = context.next_echo(id);
    const auto sequence = echo.sequence;
    const auto& options = context.ping(id).options;
    // A traceroute sends its next request once this one has an answer.
    if (!options.traceroute && sequence < options.count) context.schedule(options.interval, TimerKind::PingSend, id.value);
    echo.ttl = options.traceroute ? static_cast<std::uint8_t>(sequence) : options.ttl;
    const IcmpMessage message{icmp::echo_request, 0, static_cast<std::uint16_t>(id.value), sequence, ping_data_size};
    return {Ipv4Packet{source, context.ping(id).destination, echo.ttl, ip_protocol::icmp, 0, message}, id};
}

void PingApp::sent(Context& context, PingId id, std::uint16_t sequence, PduId pdu) {
    waiting_[timer_key(id, sequence)] = {context.schedule(context.ping(id).options.timeout, TimerKind::EchoTimeout, timer_key(id, sequence)), pdu};
}

std::map<std::uint64_t, PingApp::Waiting>::iterator PingApp::find(Context& context, std::uint16_t identifier, std::uint16_t sequence,
                                                                   Ipv4Address to) {
    for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
        const PingId id{static_cast<std::uint32_t>(it->first >> 16)};
        if (static_cast<std::uint16_t>(id.value) == identifier && static_cast<std::uint16_t>(it->first) == sequence &&
            context.ping(id).destination == to)
            return it;
    }
    return waiting_.end();
}

bool PingApp::reply(Context& context, const Arrival& arrival, Ipv4Address from, const IcmpMessage& message) {
    const auto it = find(context, message.identifier, message.sequence, from);
    if (it == waiting_.end()) return false;
    const auto key = it->first;
    context.cancel(it->second.timer);
    waiting_.erase(it);
    context.echo_replied(PingId{static_cast<std::uint32_t>(key >> 16)}, static_cast<std::uint16_t>(key), &arrival);
    return true;
}

bool PingApp::error(Context& context, const Arrival& arrival, const Ipv4Packet& packet) {
    const auto& original = packet.icmp.original;
    if (!original || original->protocol != ip_protocol::icmp || original->type != icmp::echo_request) return false;
    const auto it = find(context, original->identifier, original->sequence, original->dst);
    if (it == waiting_.end()) return false;
    const auto key = it->first;
    context.cancel(it->second.timer);
    waiting_.erase(it);
    const auto reason = packet.icmp.type == icmp::time_exceeded ? Reason::TtlExceeded
        : packet.icmp.code == icmp::host_unreachable                 ? Reason::HostUnreachable
                                                                     : Reason::NetUnreachable;
    context.echo_error(PingId{static_cast<std::uint32_t>(key >> 16)}, static_cast<std::uint16_t>(key), reason, arrival, packet.src);
    return true;
}

void PingApp::timeout(Context& context, std::uint64_t key) {
    const auto it = waiting_.find(key);
    if (it == waiting_.end()) return;
    const auto request = it->second.request;
    waiting_.erase(it);
    context.echo_lost(PingId{static_cast<std::uint32_t>(key >> 16)}, static_cast<std::uint16_t>(key), Reason::EchoTimeout, request);
}

} // namespace netsim

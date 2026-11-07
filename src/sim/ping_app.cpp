#include "sim/ping_app.hpp"

namespace netsim {

Outgoing PingApp::request(Context& context, PingId id, Ipv4Address source) {
    const auto sequence = context.next_echo(id).sequence;
    const auto& status = context.ping(id);
    if (sequence < status.options.count) context.schedule(status.options.interval, TimerKind::PingSend, id.value);
    const IcmpMessage message{icmp::echo_request, 0, static_cast<std::uint16_t>(id.value), sequence, ping_data_size};
    return {Ipv4Packet{source, status.destination, default_ttl, ip_protocol::icmp, 0, message}, id};
}

void PingApp::sent(Context& context, PingId id, std::uint16_t sequence, PduId pdu) {
    waiting_[timer_key(id, sequence)] = {context.schedule(context.ping(id).options.timeout, TimerKind::EchoTimeout, timer_key(id, sequence)), pdu};
}

bool PingApp::reply(Context& context, const Arrival& arrival, Ipv4Address from, const IcmpMessage& message) {
    for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
        const PingId id{static_cast<std::uint32_t>(it->first >> 16)};
        const auto sequence = static_cast<std::uint16_t>(it->first);
        if (static_cast<std::uint16_t>(id.value) != message.identifier || sequence != message.sequence ||
            context.ping(id).destination != from)
            continue;
        context.cancel(it->second.timer);
        waiting_.erase(it);
        context.echo_replied(id, sequence, &arrival);
        return true;
    }
    return false;
}

void PingApp::timeout(Context& context, std::uint64_t key) {
    const auto it = waiting_.find(key);
    if (it == waiting_.end()) return;
    const auto request = it->second.request;
    waiting_.erase(it);
    context.echo_lost(PingId{static_cast<std::uint32_t>(key >> 16)}, static_cast<std::uint16_t>(key), Reason::EchoTimeout, request);
}

} // namespace netsim

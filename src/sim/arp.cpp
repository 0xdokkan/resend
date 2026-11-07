#include "sim/arp.hpp"

#include <utility>

namespace netsim {

std::optional<MacAddress> Arp::lookup(Ipv4Address address) const {
    const auto it = cache_.find(address);
    return it == cache_.end() ? std::nullopt : std::optional(it->second);
}

void Arp::resolve(Context& context, const Nic& nic, Ipv4Address next_hop, Outgoing outgoing) {
    auto [resolution, fresh] = resolving_.try_emplace(next_hop);
    resolution->second.waiting.push_back(std::move(outgoing));
    if (fresh) request(context, nic, next_hop, resolution->second);
}

void Arp::request(Context& context, const Nic& nic, Ipv4Address next_hop, Resolution& resolution) {
    ++resolution.attempt;
    const auto mac = nic.mac(context);
    nic.send(context, {MacAddress::broadcast(), mac, ArpPacket{ArpPacket::Op::Request, mac, nic.address(context).address(), {}, next_hop}},
             resolution.attempt);
    resolution.timer = context.schedule(arp_retry_interval, TimerKind::ArpRetry, timer_key(nic, next_hop));
}

std::vector<Outgoing> Arp::receive(Context& context, const Nic& nic, const Arrival& arrival, const ArpPacket& packet) {
    const auto self = nic.address(context).address();
    const bool for_us = packet.target_ip == self;
    // The merge step: refresh a known sender; learn it when the packet is
    // for us, since we are about to talk to it.
    const bool merge = cache_.contains(packet.sender_ip) || for_us;
    if (merge) {
        cache_[packet.sender_ip] = packet.sender_mac;
        context.note(arrival, TraceKind::ArpCacheUpdated);
    }
    if (!for_us) {
        context.note(arrival, TraceKind::FrameDropped, Reason::ArpForOtherHost);
    } else if (packet.op == ArpPacket::Op::Request) {
        const auto mac = nic.mac(context);
        nic.send(context, {packet.sender_mac, mac, ArpPacket{ArpPacket::Op::Reply, mac, self, packet.sender_mac, packet.sender_ip}});
    }

    const auto it = merge ? resolving_.find(packet.sender_ip) : resolving_.end();
    if (it == resolving_.end()) return {};
    context.cancel(it->second.timer);
    auto ready = std::move(it->second.waiting);
    resolving_.erase(it);
    return ready;
}

std::vector<Outgoing> Arp::retry(Context& context, const Nic& nic, Ipv4Address next_hop) {
    const auto it = resolving_.find(next_hop);
    if (it == resolving_.end()) return {};
    auto& resolution = it->second;
    context.record({.kind = TraceKind::ArpTimeout, .port = nic.port, .value = resolution.attempt});
    if (resolution.attempt < arp_attempts) {
        request(context, nic, next_hop, resolution);
        return {};
    }
    auto dropped = std::move(resolution.waiting);
    resolving_.erase(it);
    return dropped;
}

std::vector<ArpCacheEntry> Arp::entries() const {
    std::vector<ArpCacheEntry> entries;
    for (const auto& [ip, mac] : cache_) entries.push_back({ip, mac});
    return entries;
}

} // namespace netsim

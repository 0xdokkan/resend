#include "sim/arp.hpp"

#include <iterator>
#include <utility>

namespace netsim {

std::optional<MacAddress> Arp::lookup(Context& context, const Nic& nic, Ipv4Address address) {
    const auto it = cache_.find(address);
    if (it == cache_.end()) return std::nullopt;
    if (it->second.valid(context.now())) return it->second.mac;
    context.record({.kind = TraceKind::ArpEntryExpired, .port = nic.port, .value = address.bits()});
    cache_.erase(it);
    return std::nullopt;
}

std::optional<Outgoing> Arp::resolve(Context& context, const Nic& nic, Ipv4Address next_hop, Outgoing outgoing) {
    auto [resolution, fresh] = resolving_.try_emplace(next_hop);
    auto& waiting = resolution->second.waiting;
    waiting.push_back(std::move(outgoing));
    if (fresh) request(context, nic, next_hop, resolution->second);
    if (waiting.size() <= arp_queue_limit) return std::nullopt;
    auto oldest = std::move(waiting.front());
    waiting.pop_front();
    return oldest;
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
    // The merge step: refresh a sender we know or are asking for; learn it
    // when the packet is for us, since we are about to talk to it.
    const auto known = cache_.find(packet.sender_ip);
    const bool merge = (known != cache_.end() && known->second.valid(context.now())) || resolving_.contains(packet.sender_ip) || for_us;
    if (merge) {
        cache_[packet.sender_ip] = {packet.sender_mac, context.now()};
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
    std::vector<Outgoing> ready(std::make_move_iterator(it->second.waiting.begin()), std::make_move_iterator(it->second.waiting.end()));
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
    std::vector<Outgoing> dropped(std::make_move_iterator(resolution.waiting.begin()), std::make_move_iterator(resolution.waiting.end()));
    resolving_.erase(it);
    return dropped;
}

std::vector<ArpCacheEntry> Arp::entries(SimTime now) const {
    std::map<Ipv4Address, ArpCacheEntry> rows;
    for (const auto& [ip, entry] : cache_)
        if (entry.valid(now)) rows[ip] = {ip, entry.mac, ArpState::Reachable, entry.updated + arp_cache_timeout, 0};
    for (const auto& [ip, resolution] : resolving_)
        rows.try_emplace(ip, ArpCacheEntry{ip, {}, ArpState::Incomplete, {}, resolution.waiting.size()});
    std::vector<ArpCacheEntry> entries;
    for (const auto& [ip, row] : rows) entries.push_back(row);
    return entries;
}

} // namespace netsim

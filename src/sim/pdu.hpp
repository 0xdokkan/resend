#pragma once

#include "core/address.hpp"

#include <cstdint>

namespace netsim {

enum class PduKind : std::uint8_t { ArpRequest, ArpReply, EchoRequest, EchoReply };

constexpr bool is_arp(PduKind kind) { return kind == PduKind::ArpRequest || kind == PduKind::ArpReply; }

// One Ethernet frame as the current engine models it, with the ARP or
// IPv4/ICMP payload flattened into the address fields. The new engine
// replaces it with layered, immutable frames (docs/engine.md).
struct Frame {
    PduKind kind{};
    MacAddress src_mac, dst_mac;
    Ipv4Address src_ip, dst_ip;  // ARP sender/target, or IPv4 source/destination
    std::uint64_t ping_id{};     // ICMP Echo only
    bool operator==(const Frame&) const = default;
};

} // namespace netsim

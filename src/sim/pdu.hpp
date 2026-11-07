#pragma once

#include "core/address.hpp"

#include <cstdint>
#include <variant>

namespace netsim {

// Protocol data units, layer by layer, as plain values. A frame is archived
// once in the trace and never changes; a router that decrements the TTL
// archives a new one.

struct ArpPacket {
    enum class Op : std::uint16_t { Request = 1, Reply = 2 };
    Op op = Op::Request;
    MacAddress sender_mac;
    Ipv4Address sender_ip;
    MacAddress target_mac;  // all zeros in a request
    Ipv4Address target_ip;
    bool operator==(const ArpPacket&) const = default;
};

namespace icmp {
constexpr std::uint8_t echo_reply = 0;
constexpr std::uint8_t destination_unreachable = 3;
constexpr std::uint8_t echo_request = 8;
constexpr std::uint8_t time_exceeded = 11;
} // namespace icmp

struct IcmpMessage {
    std::uint8_t type = icmp::echo_request;
    std::uint8_t code = 0;
    std::uint16_t identifier = 0;  // Echo: which ping
    std::uint16_t sequence = 0;    // Echo: which request of that ping
    bool operator==(const IcmpMessage&) const = default;
};

namespace ip_protocol {
constexpr std::uint8_t icmp = 1;
} // namespace ip_protocol

struct Ipv4Packet {
    Ipv4Address src, dst;
    std::uint8_t ttl = 64;
    std::uint8_t protocol = ip_protocol::icmp;
    std::uint16_t identification = 0;
    IcmpMessage icmp;  // the only payload modelled so far
    bool operator==(const Ipv4Packet&) const = default;
};

namespace ethertype {
constexpr std::uint16_t ipv4 = 0x0800;
constexpr std::uint16_t arp = 0x0806;
} // namespace ethertype

struct EthernetFrame {
    MacAddress dst, src;
    std::variant<ArpPacket, Ipv4Packet> payload;
    const ArpPacket* arp() const { return std::get_if<ArpPacket>(&payload); }
    const Ipv4Packet* ipv4() const { return std::get_if<Ipv4Packet>(&payload); }
    std::uint16_t type() const { return arp() ? ethertype::arp : ethertype::ipv4; }
    bool operator==(const EthernetFrame&) const = default;
};

// What a frame is, for labels, colours and explanations.
enum class PduKind : std::uint8_t { ArpRequest, ArpReply, EchoRequest, EchoReply, OtherIcmp };

constexpr bool is_arp(PduKind kind) { return kind == PduKind::ArpRequest || kind == PduKind::ArpReply; }

inline PduKind kind_of(const EthernetFrame& frame) {
    if (const auto* arp = frame.arp()) return arp->op == ArpPacket::Op::Request ? PduKind::ArpRequest : PduKind::ArpReply;
    const auto& message = frame.ipv4()->icmp;
    if (message.type == icmp::echo_request) return PduKind::EchoRequest;
    if (message.type == icmp::echo_reply) return PduKind::EchoReply;
    return PduKind::OtherIcmp;
}

} // namespace netsim

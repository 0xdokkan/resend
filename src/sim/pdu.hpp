#pragma once

#include "core/address.hpp"

#include <cstdint>
#include <optional>
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
// Destination Unreachable codes.
constexpr std::uint8_t net_unreachable = 0;
constexpr std::uint8_t host_unreachable = 1;
// Time Exceeded codes.
constexpr std::uint8_t ttl_exceeded = 0;

// Errors report a problem with another datagram; the rest are queries.
constexpr bool is_error(std::uint8_t type) { return type == destination_unreachable || type == time_exceeded; }
} // namespace icmp

namespace ip_protocol {
constexpr std::uint8_t icmp = 1;
} // namespace ip_protocol

// The datagram an ICMP error is about, as the error carries it: its IPv4
// header and its ICMP message, data included. RFC 792 asks for the header
// and the first 8 bytes of data at least; routers quote as much as fits in
// 576 bytes (RFC 1812 §4.3.2.3), which here is always the whole datagram,
// as Linux does. Errors are never sent about errors, so a quote holds a
// query, never another quote.
struct QuotedDatagram {
    Ipv4Address src, dst;
    std::uint8_t ttl = 0;  // as it was when the error was sent
    std::uint8_t protocol = ip_protocol::icmp;
    std::uint16_t identification = 0;
    std::uint8_t type = icmp::echo_request, code = 0;
    std::uint16_t identifier = 0, sequence = 0, data_size = 0;
    bool operator==(const QuotedDatagram&) const = default;
};

struct IcmpMessage {
    std::uint8_t type = icmp::echo_request;
    std::uint8_t code = 0;
    std::uint16_t identifier = 0;  // Echo: which ping; 0 in errors, where these 4 bytes are unused
    std::uint16_t sequence = 0;    // Echo: which request of that ping
    // Echo: bytes of data after the header. Their content is not modelled:
    // the wire module fills them with 0x00, 0x01, ... as ping does, and a
    // reply carries the same data as its request (RFC 792). 0 in errors.
    std::uint16_t data_size = 0;
    // Errors: the datagram the error is about.
    std::optional<QuotedDatagram> original{};
    bool operator==(const IcmpMessage&) const = default;
};

// The TTL of datagrams a host sends, as in Linux and macOS (RFC 1700's
// recommended default).
constexpr std::uint8_t default_ttl = 64;

struct Ipv4Packet {
    Ipv4Address src, dst;
    std::uint8_t ttl = default_ttl;
    std::uint8_t protocol = ip_protocol::icmp;
    std::uint16_t identification = 0;
    IcmpMessage icmp;  // the only payload modelled so far
    bool operator==(const Ipv4Packet&) const = default;
};

// The quote of a datagram that an ICMP error carries, and the datagram
// back from its quote.
inline QuotedDatagram quote(const Ipv4Packet& packet) {
    const auto& message = packet.icmp;
    return {packet.src, packet.dst, packet.ttl, packet.protocol, packet.identification,
            message.type, message.code, message.identifier, message.sequence, message.data_size};
}
inline Ipv4Packet unquote(const QuotedDatagram& quoted) {
    return {quoted.src, quoted.dst, quoted.ttl, quoted.protocol, quoted.identification,
            IcmpMessage{quoted.type, quoted.code, quoted.identifier, quoted.sequence, quoted.data_size}};
}

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
enum class PduKind : std::uint8_t { ArpRequest, ArpReply, EchoRequest, EchoReply, OtherIcmp, TimeExceeded, Unreachable };

constexpr bool is_arp(PduKind kind) { return kind == PduKind::ArpRequest || kind == PduKind::ArpReply; }
constexpr bool is_icmp_error(PduKind kind) { return kind == PduKind::TimeExceeded || kind == PduKind::Unreachable; }

inline PduKind kind_of(const EthernetFrame& frame) {
    if (const auto* arp = frame.arp()) return arp->op == ArpPacket::Op::Request ? PduKind::ArpRequest : PduKind::ArpReply;
    const auto& message = frame.ipv4()->icmp;
    switch (message.type) {
    case icmp::echo_request: return PduKind::EchoRequest;
    case icmp::echo_reply: return PduKind::EchoReply;
    case icmp::time_exceeded: return PduKind::TimeExceeded;
    case icmp::destination_unreachable: return PduKind::Unreachable;
    default: return PduKind::OtherIcmp;
    }
}

} // namespace netsim

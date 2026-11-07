#pragma once

#include "sim/pdu.hpp"
#include "sim/trace.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

// Frames as bytes on the wire, and captures that Wireshark and tcpdump read.
namespace netsim::wire {

// Ethernet II frames without preamble and FCS, so the shortest is 60 bytes:
// shorter frames are padded with zeros.
constexpr std::size_t min_frame_size = 60;

// The bytes of a frame, in network byte order: Ethernet II; ARP for IPv4
// over Ethernet (RFC 826); IPv4 with a 20-byte header and its checksum
// (RFC 791) and no flags, carrying ICMP with its checksum (RFC 792). Echo
// data is 0x00, 0x01, ...: ping's pattern, without the timestamp ping
// writes over its first bytes.
std::vector<std::uint8_t> encode(const EthernetFrame& frame);
// Reads what encode() writes, padding included. Anything the model cannot
// represent gives nullopt: another EtherType, an ARP packet not for IPv4
// over Ethernet, IPv4 options or fragments, a wrong checksum, other data.
std::optional<EthernetFrame> decode(std::span<const std::uint8_t> bytes);
// The Internet checksum (RFC 1071) of `bytes`, ready to store.
std::uint16_t checksum(std::span<const std::uint8_t> bytes);

// A capture in the pcap format with nanosecond timestamps (link type
// Ethernet): every frame put on a cable, or only those on `link`, at the
// simulated time it was sent. Simulated time 0 is the Unix epoch.
void write_pcap(std::ostream& out, const Trace& trace, std::optional<LinkId> link = std::nullopt);

} // namespace netsim::wire

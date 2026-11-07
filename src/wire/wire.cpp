#include "wire/wire.hpp"

#include "core/text.hpp"

#include <array>
#include <utility>

namespace netsim::wire {
namespace {
using text::tr;
constexpr std::size_t ethernet_header = 14, ipv4_header = 20, icmp_header = 8;

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    put16(out, static_cast<std::uint16_t>(value >> 16));
    put16(out, static_cast<std::uint16_t>(value));
}
void put(std::vector<std::uint8_t>& out, const MacAddress& mac) {
    for (const auto byte : mac.bytes()) out.push_back(byte);
}
void store16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value >> 8);
    out[at + 1] = static_cast<std::uint8_t>(value);
}

// Reads big-endian fields from a span, remembering whether it ran short.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool ok() const { return ok_; }
    std::size_t position() const { return position_; }
    std::uint8_t u8() {
        if (position_ >= bytes_.size()) {
            ok_ = false;
            return 0;
        }
        return bytes_[position_++];
    }
    std::uint16_t u16() {
        const auto high = u8();
        return static_cast<std::uint16_t>(high << 8 | u8());
    }
    std::uint32_t u32() {
        const std::uint32_t high = u16();
        return high << 16 | u16();
    }
    MacAddress mac() {
        MacAddress::Bytes bytes{};
        for (auto& byte : bytes) byte = u8();
        return MacAddress(bytes);
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

void put_le32(std::ostream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8), static_cast<char>(value >> 16),
                                    static_cast<char>(value >> 24)};
    out.write(bytes.data(), bytes.size());
}
void put_le16(std::ostream& out, std::uint16_t value) {
    const std::array<char, 2> bytes{static_cast<char>(value), static_cast<char>(value >> 8)};
    out.write(bytes.data(), bytes.size());
}
} // namespace

std::uint16_t checksum(std::span<const std::uint8_t> bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const std::uint32_t low = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        sum += std::uint32_t{bytes[i]} << 8 | low;
    }
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}

std::vector<std::uint8_t> encode(const EthernetFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(min_frame_size + 64);
    put(out, frame.dst);
    put(out, frame.src);
    put16(out, frame.type());
    if (const auto* arp = frame.arp()) {
        put16(out, 1);       // hardware: Ethernet
        put16(out, ethertype::ipv4);
        out.push_back(6);    // hardware address length
        out.push_back(4);    // protocol address length
        put16(out, static_cast<std::uint16_t>(arp->op));
        put(out, arp->sender_mac);
        put32(out, arp->sender_ip.bits());
        put(out, arp->target_mac);
        put32(out, arp->target_ip.bits());
    } else {
        const auto& packet = *frame.ipv4();
        const auto& message = packet.icmp;
        const auto start = out.size();
        out.push_back(0x45);  // version 4, 5 words of header
        out.push_back(0);     // DSCP and ECN
        put16(out, static_cast<std::uint16_t>(ipv4_header + icmp_header + message.data_size));
        put16(out, packet.identification);
        put16(out, 0);        // no flags, no fragment offset
        out.push_back(packet.ttl);
        out.push_back(packet.protocol);
        put16(out, 0);        // checksum, below
        put32(out, packet.src.bits());
        put32(out, packet.dst.bits());
        store16(out, start + 10, checksum(std::span(out).subspan(start, ipv4_header)));

        const auto icmp_start = out.size();
        out.push_back(message.type);
        out.push_back(message.code);
        put16(out, 0);  // checksum, below
        put16(out, message.identifier);
        put16(out, message.sequence);
        for (std::size_t i = 0; i < message.data_size; ++i) out.push_back(static_cast<std::uint8_t>(i));
        store16(out, icmp_start + 2, checksum(std::span(out).subspan(icmp_start)));
    }
    if (out.size() < min_frame_size) out.resize(min_frame_size, 0);
    return out;
}

std::optional<EthernetFrame> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < min_frame_size) return std::nullopt;
    Reader read(bytes);
    EthernetFrame frame;
    frame.dst = read.mac();
    frame.src = read.mac();
    const auto type = read.u16();
    if (type == ethertype::arp) {
        ArpPacket arp;
        if (read.u16() != 1 || read.u16() != ethertype::ipv4 || read.u8() != 6 || read.u8() != 4) return std::nullopt;
        const auto op = read.u16();
        if (op != 1 && op != 2) return std::nullopt;
        arp.op = static_cast<ArpPacket::Op>(op);
        arp.sender_mac = read.mac();
        arp.sender_ip = Ipv4Address(read.u32());
        arp.target_mac = read.mac();
        arp.target_ip = Ipv4Address(read.u32());
        frame.payload = arp;
        return read.ok() ? std::optional(frame) : std::nullopt;
    }
    if (type != ethertype::ipv4) return std::nullopt;

    constexpr auto start = ethernet_header;
    if (read.u8() != 0x45 || read.u8() != 0) return std::nullopt;
    const auto total = read.u16();
    Ipv4Packet packet;
    packet.identification = read.u16();
    if (read.u16() != 0) return std::nullopt;
    packet.ttl = read.u8();
    packet.protocol = read.u8();
    read.u16();
    packet.src = Ipv4Address(read.u32());
    packet.dst = Ipv4Address(read.u32());
    if (!read.ok() || total < ipv4_header + icmp_header || start + total > bytes.size() || packet.protocol != ip_protocol::icmp ||
        checksum(bytes.subspan(start, ipv4_header)) != 0 || checksum(bytes.subspan(start + ipv4_header, total - ipv4_header)) != 0)
        return std::nullopt;
    auto& message = packet.icmp;
    message.type = read.u8();
    message.code = read.u8();
    read.u16();
    message.identifier = read.u16();
    message.sequence = read.u16();
    message.data_size = static_cast<std::uint16_t>(total - ipv4_header - icmp_header);
    for (std::size_t i = 0; i < message.data_size; ++i)
        if (read.u8() != static_cast<std::uint8_t>(i)) return std::nullopt;
    frame.payload = packet;
    return read.ok() ? std::optional(frame) : std::nullopt;
}

namespace {
std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] << 8 | bytes[at + 1]);
}
std::string hex(unsigned value, int digits) {
    static constexpr char numerals[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) text += numerals[(value >> shift) & 0xfU];
    return text;
}
std::string colon_bytes(std::span<const std::uint8_t> bytes) {
    static constexpr char numerals[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        if (!text.empty()) text += ':';
        text += numerals[byte >> 4];
        text += numerals[byte & 0xfU];
    }
    return text;
}
MacAddress mac_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    MacAddress::Bytes value{};
    for (std::size_t i = 0; i < value.size(); ++i) value[i] = bytes[at + i];
    return MacAddress(value);
}
Ipv4Address ipv4_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    return Ipv4Address(static_cast<std::uint32_t>(read16(bytes, at)) << 16 | read16(bytes, at + 2));
}
std::string checked(std::span<const std::uint8_t> bytes) { return checksum(bytes) == 0 ? tr("correct") : tr("incorrect"); }
} // namespace

std::vector<Layer> dissect(std::span<const std::uint8_t> bytes) {
    const auto frame = decode(bytes);
    if (!frame) return {};
    std::vector<Layer> layers;
    const auto mac = [&](std::size_t at) {
        const auto address = mac_at(bytes, at);
        return std::pair{address.to_string(), address == MacAddress::broadcast() ? tr("Broadcast") : std::string()};
    };
    Layer ethernet{"Ethernet II", "eth", 0, ethernet_header, {}};
    const auto [dst, dst_meaning] = mac(0);
    const auto [src, src_meaning] = mac(6);
    const auto type = read16(bytes, 12);
    ethernet.fields = {
        {tr("Destination"), "eth.dst", 0, 6, dst, dst_meaning},
        {tr("Source"), "eth.src", 6, 6, src, src_meaning},
        {tr("Type"), "eth.type", 12, 2, hex(type, 4), type == ethertype::arp ? "ARP" : "IPv4"},
    };
    std::size_t end = ethernet_header;
    if (const auto* arp = frame->arp()) {
        constexpr std::size_t at = ethernet_header;
        layers.push_back(std::move(ethernet));
        layers.push_back({"Address Resolution Protocol", "arp", at, 28, {
            {tr("Hardware type"), "arp.hw.type", at, 2, std::to_string(read16(bytes, at)), "Ethernet"},
            {tr("Protocol type"), "arp.proto.type", at + 2, 2, hex(read16(bytes, at + 2), 4), "IPv4"},
            {tr("Hardware size"), "arp.hw.size", at + 4, 1, std::to_string(bytes[at + 4])},
            {tr("Protocol size"), "arp.proto.size", at + 5, 1, std::to_string(bytes[at + 5])},
            {tr("Opcode"), "arp.opcode", at + 6, 2, std::to_string(read16(bytes, at + 6)), arp->op == ArpPacket::Op::Request ? tr("request") : tr("reply")},
            {tr("Sender MAC address"), "arp.src.hw_mac", at + 8, 6, mac_at(bytes, at + 8).to_string()},
            {tr("Sender IP address"), "arp.src.proto_ipv4", at + 14, 4, ipv4_at(bytes, at + 14).to_string()},
            {tr("Target MAC address"), "arp.dst.hw_mac", at + 18, 6, mac_at(bytes, at + 18).to_string()},
            {tr("Target IP address"), "arp.dst.proto_ipv4", at + 24, 4, ipv4_at(bytes, at + 24).to_string()},
        }});
        end = at + 28;
    } else {
        const auto& packet = *frame->ipv4();
        constexpr std::size_t at = ethernet_header, icmp_at = ethernet_header + ipv4_header;
        const auto total = read16(bytes, at + 2);
        layers.push_back(std::move(ethernet));
        layers.push_back({"Internet Protocol Version 4", "ip", at, ipv4_header, {
            {tr("Version"), "ip.version", at, 1, std::to_string(bytes[at] >> 4)},
            {tr("Header length"), "ip.hdr_len", at, 1, std::to_string((bytes[at] & 0xfU) * 4), tr("bytes")},
            {tr("Differentiated services"), "ip.dsfield", at + 1, 1, hex(bytes[at + 1], 2)},
            {tr("Total length"), "ip.len", at + 2, 2, std::to_string(total), tr("bytes")},
            {tr("Identification"), "ip.id", at + 4, 2, hex(read16(bytes, at + 4), 4)},
            {tr("Flags"), "ip.flags", at + 6, 1, hex(static_cast<unsigned>(bytes[at + 6] >> 5) << 5, 2), tr("none")},
            {tr("Fragment offset"), "ip.frag_offset", at + 6, 2, std::to_string(read16(bytes, at + 6) & 0x1fffU)},
            {tr("Time to live"), "ip.ttl", at + 8, 1, std::to_string(packet.ttl), tr("hops left")},
            {tr("Protocol"), "ip.proto", at + 9, 1, std::to_string(packet.protocol), "ICMP"},
            {tr("Header checksum"), "ip.checksum", at + 10, 2, hex(read16(bytes, at + 10), 4), checked(bytes.subspan(at, ipv4_header))},
            {tr("Source address"), "ip.src", at + 12, 4, packet.src.to_string()},
            {tr("Destination address"), "ip.dst", at + 16, 4, packet.dst.to_string()},
        }});
        const auto& message = packet.icmp;
        const std::size_t icmp_length = total - ipv4_header;
        const auto kind = message.type == icmp::echo_request ? tr("Echo (ping) request")
            : message.type == icmp::echo_reply ? tr("Echo (ping) reply") : std::string();
        layers.push_back({"Internet Control Message Protocol", "icmp", icmp_at, icmp_header, {
            {tr("Type"), "icmp.type", icmp_at, 1, std::to_string(message.type), kind},
            {tr("Code"), "icmp.code", icmp_at + 1, 1, std::to_string(message.code)},
            {tr("Checksum"), "icmp.checksum", icmp_at + 2, 2, hex(read16(bytes, icmp_at + 2), 4), checked(bytes.subspan(icmp_at, icmp_length))},
            {tr("Identifier"), "icmp.ident", icmp_at + 4, 2, std::to_string(message.identifier)},
            {tr("Sequence number"), "icmp.seq", icmp_at + 6, 2, std::to_string(message.sequence)},
        }});
        end = icmp_at + icmp_header;
        if (message.data_size > 0) {
            const auto data = bytes.subspan(end, message.data_size);
            layers.back().length += message.data_size;
            layers.push_back({"Data", "data", end, message.data_size, {
                {tr("Data"), "data.data", end, message.data_size, colon_bytes(data), tr("{0} bytes", {std::to_string(message.data_size)})},
            }});
            end += message.data_size;
        }
    }
    if (end < bytes.size()) {
        layers.front().fields.push_back({tr("Padding"), "eth.padding", end, bytes.size() - end, colon_bytes(bytes.subspan(end))});
    }
    return layers;
}

void write_pcap(std::ostream& out, const Trace& trace, std::optional<LinkId> link) {
    put_le32(out, 0xa1b23c4d);  // pcap with nanosecond timestamps
    put_le16(out, 2);
    put_le16(out, 4);
    put_le32(out, 0);           // time zone: UTC
    put_le32(out, 0);           // timestamp accuracy
    put_le32(out, 65535);       // snapshot length
    put_le32(out, 1);           // link type: Ethernet
    for (const auto& sent : trace.transmissions()) {
        if (link && sent.link != *link) continue;
        const auto bytes = encode(trace.pdu(sent.pdu));
        const auto ns = sent.start.count();
        put_le32(out, static_cast<std::uint32_t>(ns / 1'000'000'000));
        put_le32(out, static_cast<std::uint32_t>(ns % 1'000'000'000));
        put_le32(out, static_cast<std::uint32_t>(bytes.size()));
        put_le32(out, static_cast<std::uint32_t>(bytes.size()));
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

} // namespace netsim::wire

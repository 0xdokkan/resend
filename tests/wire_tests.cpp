// Frames as bytes and pcap captures.

#include "check.hpp"
#include "sim_support.hpp"
#include "wire/wire.hpp"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace simtest;
using Bytes = std::vector<std::uint8_t>;
namespace wire = netsim::wire;

Bytes hex(std::string_view text) {
    Bytes bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return bytes;
}

// The sample LAN after a ping from PC-A to PC-B.
Simulation sample_run() {
    Simulation simulation(netsim::sample_lan());
    ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    drain(simulation);
    return simulation;
}

void checksums() {
    // RFC 1071's example, an odd length, and an IPv4 header from the wild.
    CHECK(wire::checksum(hex("0001f203f4f5f6f7")) == 0x220d);
    CHECK(wire::checksum(hex("0001f203f4f5f6")) == static_cast<std::uint16_t>(~(0x0001U + 0xf203U + 0xf4f5U + 0xf600U + 2U)));
    CHECK(wire::checksum(hex("450000730000400040110000c0a80001c0a800c7")) == 0xb861);
    // A block with its checksum in place sums to zero.
    CHECK(wire::checksum(hex("45000073000040004011b861c0a80001c0a800c7")) == 0);
    CHECK(wire::checksum({}) == 0xffff);
}

void known_frames() {
    const auto simulation = sample_run();
    const auto& frames = simulation.trace().pdus();
    // PC-A's ARP request, padded to 60 bytes.
    CHECK(wire::encode(frames.at(0)) ==
          hex("ffffffffffff02000000000108060001080006040001020000000001c0a8010a000000000000c0a80114000000000000000000000000000000000000"));
    // Its Echo Request: 14 + 20 + 8 + 56 bytes, both checksums as computed
    // independently.
    CHECK(netsim::kind_of(frames.at(2)) == netsim::PduKind::EchoRequest);
    CHECK(wire::encode(frames.at(2)) ==
          hex("020000000002020000000001080045000054000100004001f739c0a8010ac0a80114080000eb0001000100010203040506070809"
              "0a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f3031323334353637"));
}

void round_trips() {
    // Every frame of a few runs reads back as itself.
    std::vector<Simulation> runs;
    runs.push_back(sample_run());
    {
        Simulation simulation(star(4));
        ping(simulation, DeviceId{2}, ip("192.168.1.12"), {.count = 3});
        ping(simulation, DeviceId{3}, ip("192.168.1.99"));
        ping(simulation, DeviceId{4}, ip("192.168.1.11"), {.count = 2, .interval = 10ms});
        drain(simulation);
        runs.push_back(std::move(simulation));
    }
    int frames = 0;
    for (const auto& run : runs)
        for (const auto& frame : run.trace().pdus()) {
            const auto bytes = wire::encode(frame);
            CHECK(bytes.size() >= wire::min_frame_size);
            const auto back = wire::decode(bytes);
            CHECK(back.has_value() && *back == frame);
            ++frames;
        }
    CHECK(frames > 20);
    // Other data sizes too, odd ones included.
    auto frame = runs[0].trace().pdu(netsim::PduId{2});
    for (const std::uint16_t size : std::initializer_list<std::uint16_t>{0, 1, 17, 56, 1472}) {
        std::get<netsim::Ipv4Packet>(frame.payload).icmp.data_size = size;
        const auto bytes = wire::encode(frame);
        CHECK(bytes.size() == std::max<std::size_t>(wire::min_frame_size, 14U + 20U + 8U + size));
        CHECK(wire::decode(bytes) == frame);
    }
}

void refused_bytes() {
    const auto simulation = sample_run();
    const auto arp = wire::encode(simulation.trace().pdu(netsim::PduId{0}));
    const auto echo = wire::encode(simulation.trace().pdu(netsim::PduId{2}));
    const auto changed = [](Bytes bytes, std::size_t at, std::uint8_t value) {
        bytes.at(at) = value;
        return bytes;
    };
    CHECK(!wire::decode(Bytes(arp.begin(), arp.begin() + 59)));  // short
    CHECK(!wire::decode(changed(arp, 12, 0x86)));                // IPv6
    CHECK(!wire::decode(changed(arp, 15, 6)));                   // hardware type 6
    CHECK(!wire::decode(changed(arp, 21, 3)));                   // RARP request
    CHECK(!wire::decode(changed(echo, 14, 0x46)));               // IPv4 options
    CHECK(!wire::decode(changed(echo, 20, 0x20)));               // a fragment
    CHECK(!wire::decode(changed(echo, 23, 17)));                 // UDP
    CHECK(!wire::decode(changed(echo, 26, 0xc1)));               // IPv4 checksum wrong
    CHECK(!wire::decode(changed(echo, 50, 0xff)));               // ICMP checksum wrong
    CHECK(!wire::decode(Bytes(echo.begin(), echo.end() - 1)));   // shorter than its length
    // Data other than ping's pattern, with a correct checksum.
    auto other = changed(changed(echo, 42, 0x01), 36, 0);
    other[37] = 0;
    const auto sum = wire::checksum(std::span(other).subspan(34));
    other[36] = static_cast<std::uint8_t>(sum >> 8);
    other[37] = static_cast<std::uint8_t>(sum);
    CHECK(!wire::decode(other));
}

std::uint32_t le32(const std::string& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t{static_cast<std::uint8_t>(bytes.at(at + i))} << (8 * i);
    return value;
}

void pcap_files() {
    // The ARP retry at 1 s gives a timestamp with whole seconds.
    Simulation simulation(netsim::sample_lan());
    CHECK(simulation.apply(netsim::SetLinkUp{netsim::LinkId{2}, false}) == Problem::None);
    ping(simulation, DeviceId{1}, ip("192.168.1.20"));
    simulation.run_until(1500ms);
    CHECK(simulation.apply(netsim::SetLinkUp{netsim::LinkId{2}, true}) == Problem::None);
    drain(simulation);
    const auto& trace = simulation.trace();

    std::ostringstream out;
    wire::write_pcap(out, trace);
    const auto file = out.str();
    // Header: nanosecond magic, version 2.4, no time zone, snapshot length, Ethernet.
    CHECK(file.substr(0, 4) == std::string("\x4d\x3c\xb2\xa1", 4));
    CHECK(le32(file, 4) == (4U << 16 | 2U) && le32(file, 8) == 0 && le32(file, 12) == 0);
    CHECK(le32(file, 16) == 65535 && le32(file, 20) == 1);
    // One record per transmission, in order, each with its frame.
    std::size_t at = 24, index = 0;
    while (at < file.size()) {
        const auto& sent = trace.transmissions().at(index++);
        const auto bytes = wire::encode(trace.pdu(sent.pdu));
        CHECK(le32(file, at) == sent.start / 1s && le32(file, at + 4) == (sent.start % 1s).count());
        CHECK(le32(file, at + 8) == bytes.size() && le32(file, at + 12) == bytes.size());
        CHECK(file.compare(at + 16, bytes.size(), reinterpret_cast<const char*>(bytes.data()), bytes.size()) == 0);
        at += 16 + bytes.size();
    }
    CHECK(at == file.size() && index == trace.transmissions().size());
    const auto late = std::find_if(trace.transmissions().begin(), trace.transmissions().end(), [](const auto& sent) { return sent.start >= 1s; });
    CHECK(late != trace.transmissions().end() && late->start == 1s);

    // One cable: only its frames.
    std::ostringstream one;
    wire::write_pcap(one, trace, netsim::LinkId{2});
    std::size_t expected = 24;
    for (const auto& sent : trace.transmissions())
        if (sent.link == netsim::LinkId{2}) expected += 16 + wire::encode(trace.pdu(sent.pdu)).size();
    CHECK(one.str().size() == expected && expected > 24);
    CHECK(one.str().substr(0, 24) == file.substr(0, 24));
}
} // namespace

int main() {
    checksums();
    known_frames();
    round_trips();
    refused_bytes();
    pcap_files();
    std::cout << "Wire tests passed: " << test::checks << " checks.\n";
}

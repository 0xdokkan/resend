// Frames as bytes and pcap captures.

#include "check.hpp"
#include "routed_networks.hpp"
#include "sim_support.hpp"
#include "wire/wire.hpp"

#include <algorithm>
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

// Runs through routers, between them every ICMP error resend sends: Time
// Exceeded from a routing loop, Destination Unreachable (network) from a
// missing return route, (host) from a router whose ARP requests go
// unanswered, and a traceroute.
std::vector<Simulation> routed_runs() {
    std::vector<Simulation> runs;
    runs.emplace_back(routed::routing_loop());
    ping(runs.back(), DeviceId{1}, ip("10.9.9.9"));
    runs.emplace_back(routed::chain(false));
    ping(runs.back(), DeviceId{1}, ip("10.2.0.20"));
    runs.emplace_back(routed::two_lans());
    CHECK(runs.back().apply(netsim::SetPowered{DeviceId{5}, false}) == Problem::None);
    ping(runs.back(), DeviceId{1}, ip("10.2.0.20"), {.timeout = 5s});
    runs.emplace_back(routed::longest_prefix());
    ping(runs.back(), DeviceId{1}, ip("10.20.30.10"), {.count = 4, .traceroute = true});
    for (auto& run : runs) drain(run);
    return runs;
}

// The first frame of `kind` in a run.
const netsim::EthernetFrame& first(const Simulation& run, netsim::PduKind kind) {
    const auto& frames = run.trace().pdus();
    const auto found = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) { return netsim::kind_of(frame) == kind; });
    CHECK(found != frames.end());
    return *found;
}

// Puts back the IPv4 header checksum at `at` and the ICMP checksum after it.
void fix_checksums(Bytes& bytes, std::size_t at, std::size_t length) {
    bytes[at + 10] = bytes[at + 11] = 0;
    auto sum = wire::checksum(std::span(bytes).subspan(at, 20));
    bytes[at + 10] = static_cast<std::uint8_t>(sum >> 8);
    bytes[at + 11] = static_cast<std::uint8_t>(sum);
    bytes[at + 22] = bytes[at + 23] = 0;
    sum = wire::checksum(std::span(bytes).subspan(at + 20, length - 20));
    bytes[at + 22] = static_cast<std::uint8_t>(sum >> 8);
    bytes[at + 23] = static_cast<std::uint8_t>(sum);
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
    // ICMP errors carry the datagram they are about, quoted whole.
    const auto routed = routed_runs();
    int errors = 0;
    for (const auto& run : routed)
        for (const auto& frame : run.trace().pdus()) {
            const auto back = wire::decode(wire::encode(frame));
            CHECK(back.has_value() && *back == frame);
            errors += netsim::is_icmp_error(netsim::kind_of(frame));
        }
    CHECK(errors >= 6);
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

    // A Time Exceeded: 14 + 20 + 8 + the 84 bytes of the Echo Request.
    const auto runs = routed_runs();
    const auto error = wire::encode(first(runs[0], netsim::PduKind::TimeExceeded));
    CHECK(error.size() == 14 + 20 + 8 + 84 && wire::decode(error));
    const auto refused = [&](std::size_t at, std::uint8_t value) {
        auto bytes = changed(error, at, value);
        fix_checksums(bytes, 42, 84);  // the quoted datagram
        fix_checksums(bytes, 14, 112);
        return !wire::decode(bytes);
    };
    CHECK(!refused(14 + 1, 0));       // nothing changed: the checksums put back
    CHECK(refused(38, 1));            // an unused byte not zero
    CHECK(refused(42 + 2, 0x55));     // a quote shorter than its length says
    CHECK(refused(42 + 9, 17));       // a quoted UDP datagram
    CHECK(refused(42 + 20, 11));      // an error about an error
    CHECK(!wire::decode(Bytes(error.begin(), error.end() - 1)));
    auto quote_damaged = error;
    quote_damaged[42 + 22] ^= 1;      // the quoted ICMP checksum no longer matches
    fix_checksums(quote_damaged, 14, 112);
    CHECK(!wire::decode(quote_damaged));
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

void dissection() {
    // Every byte of a frame belongs to exactly one field, in order, and
    // the values are read from the bytes.
    const auto run = sample_run();
    int frames = 0;
    for (const auto& frame : run.trace().pdus()) {
        const auto bytes = wire::encode(frame);
        const auto layers = wire::dissect(bytes);
        CHECK(!layers.empty());
        std::vector<int> owners(bytes.size(), 0);
        for (const auto& layer : layers)
            for (const auto& field : layer.fields)
                // Version and header length share a byte; the flags share
                // one with the fragment offset, which spans two.
                if (field.name != "ip.hdr_len" && field.name != "ip.flags")
                    for (std::size_t at = field.offset; at < field.offset + field.length; ++at) ++owners.at(at);
        CHECK(std::all_of(owners.begin(), owners.end(), [](int count) { return count == 1; }));
        ++frames;
    }
    CHECK(frames == 4);

    const auto request = wire::dissect(wire::encode(run.trace().pdu(netsim::PduId{0})));
    CHECK(request.size() == 2 && request[0].label == "Ethernet II" && request[1].name == "arp");
    CHECK((request[0].fields[0] == wire::Field{"Destination", "eth.dst", 0, 6, "ff:ff:ff:ff:ff:ff", "Broadcast"}));
    CHECK((request[0].fields[2] == wire::Field{"Type", "eth.type", 12, 2, "0x0806", "ARP"}));
    CHECK(request[0].fields.back().name == "eth.padding" && request[0].fields.back().length == 18);
    CHECK((request[1].fields[4] == wire::Field{"Opcode", "arp.opcode", 20, 2, "1", "request"}));
    CHECK(request[1].fields[6].value == "192.168.1.10" && request[1].fields[8].value == "192.168.1.20");

    const auto echo = wire::dissect(wire::encode(run.trace().pdu(netsim::PduId{2})));
    CHECK(echo.size() == 4 && echo[2].name == "icmp" && echo[3].name == "data");
    CHECK(echo[1].offset == 14 && echo[1].length == 20 && echo[2].offset == 34 && echo[2].length == 64);
    CHECK(echo[1].fields[7].value == "64" && echo[1].fields[9].meaning == "correct");
    CHECK((echo[2].fields[0] == wire::Field{"Type", "icmp.type", 34, 1, "8", "Echo (ping) request"}));
    CHECK(echo[2].fields[2].meaning == "correct" && echo[3].fields[0].length == 56);
    CHECK(echo[3].fields[0].value.starts_with("00:01:02:03") && echo[3].fields[0].meaning == "56 bytes");
    CHECK(echo[0].fields.size() == 3);  // no padding

    // A Time Exceeded: the error, then the quoted IPv4 header, ICMP header
    // and data, each a layer of its own.
    const auto runs = routed_runs();
    for (const auto& routed_run : runs)
        for (const auto& frame : routed_run.trace().pdus()) {
            const auto bytes = wire::encode(frame);
            std::vector<int> owners(bytes.size(), 0);
            for (const auto& layer : wire::dissect(bytes))
                for (const auto& field : layer.fields)
                    if (field.name != "ip.hdr_len" && field.name != "ip.flags")
                        for (std::size_t at = field.offset; at < field.offset + field.length; ++at) ++owners.at(at);
            CHECK(std::all_of(owners.begin(), owners.end(), [](int count) { return count == 1; }));
        }
    const auto error = wire::dissect(wire::encode(first(runs[0], netsim::PduKind::TimeExceeded)));
    CHECK(error.size() == 6 && error[2].name == "icmp" && error[3].name == "ip" && error[4].name == "icmp" && error[5].name == "data");
    CHECK(error[1].fields[7].value == "64" && error[2].offset == 34 && error[2].length == 8 + 84);
    CHECK((error[2].fields[0] == wire::Field{"Type", "icmp.type", 34, 1, "11", "Time-to-live exceeded"}));
    CHECK((error[2].fields[1] == wire::Field{"Code", "icmp.code", 35, 1, "0", "Time to live exceeded in transit"}));
    CHECK((error[2].fields[3] == wire::Field{"Unused", "icmp.unused", 38, 4, "00:00:00:00", ""}));
    CHECK(error[3].label == "Internet Protocol Version 4, quoted" && error[3].offset == 42 && error[3].length == 20);
    CHECK(error[3].fields[7].value == "1" && error[3].fields[11].value == "10.9.9.9");
    CHECK(error[4].label == "Internet Control Message Protocol, quoted" && error[4].fields[0].meaning == "Echo (ping) request");
    CHECK(error[5].fields[0].length == 56);
    const auto unreachable = wire::dissect(wire::encode(first(runs[1], netsim::PduKind::Unreachable)));
    CHECK((unreachable[2].fields[0] == wire::Field{"Type", "icmp.type", 34, 1, "3", "Destination unreachable"}));
    CHECK((unreachable[2].fields[1] == wire::Field{"Code", "icmp.code", 35, 1, "0", "Network unreachable"}));
    CHECK(unreachable[4].fields[0].meaning == "Echo (ping) reply");
    const auto host = wire::dissect(wire::encode(first(runs[2], netsim::PduKind::Unreachable)));
    CHECK(host[2].fields[1].value == "1" && host[2].fields[1].meaning == "Host unreachable");

    // A damaged checksum is not dissected, like anything decode() refuses.
    auto bytes = wire::encode(run.trace().pdu(netsim::PduId{2}));
    bytes[30] ^= 1;
    CHECK(wire::dissect(bytes).empty());
    CHECK(wire::dissect(Bytes(10, 0)).empty());
}

int main() {
    checksums();
    known_frames();
    round_trips();
    refused_bytes();
    pcap_files();
    dissection();
    std::cout << "Wire tests passed: " << test::checks << " checks.\n";
}

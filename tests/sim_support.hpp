#pragma once

// Helpers shared by the simulation test executables.

#include "check.hpp"
#include "scenario/edit.hpp"
#include "sim/simulation.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace simtest {
using netsim::DeviceId;
using netsim::Ipv4Address;
using netsim::Problem;
using netsim::Reason;
using netsim::Scenario;
using netsim::Simulation;
using netsim::TraceKind;

inline void drain(Simulation& simulation) {
    for (int count = 0; simulation.pending(); ++count) {
        CHECK(simulation.step());
        CHECK(count < 100'000);
    }
    CHECK(!simulation.step());
}

inline Ipv4Address ip(const char* text) { return *Ipv4Address::parse(text); }
inline Ipv4Address address_of(const Scenario& scenario, DeviceId id) { return scenario.device(id)->ports[0].ipv4->address(); }

inline netsim::PingId ping(Simulation& simulation, DeviceId source, Ipv4Address destination, const netsim::PingOptions& options = {}) {
    Problem problem = Problem::NoSuchDevice;
    const auto id = simulation.start_ping(source, destination, problem, options);
    CHECK(id.has_value() && problem == Problem::None);
    return id.value_or(netsim::PingId{});
}

inline int count(const Simulation& simulation, TraceKind kind, std::optional<DeviceId> device = {}, std::optional<Reason> reason = {}) {
    const auto& records = simulation.trace().records();
    return static_cast<int>(std::count_if(records.begin(), records.end(), [&](const auto& record) {
        return record.kind == kind && (!device || record.device == *device) && (!reason || record.reason == *reason);
    }));
}

// ARP requests a host sent, or tried to send.
inline int arp_requests(const Simulation& simulation, DeviceId host) {
    const auto& records = simulation.trace().records();
    return static_cast<int>(std::count_if(records.begin(), records.end(), [&](const auto& record) {
        return record.device == host && record.pdu && (record.kind == TraceKind::FrameSent || record.kind == TraceKind::FrameDropped) &&
            netsim::kind_of(simulation.trace().pdu(*record.pdu)) == netsim::PduKind::ArpRequest &&
            simulation.trace().pdu(*record.pdu).src == simulation.scenario().device(host)->ports[0].mac;
    }));
}

// A switch (id 1) with `hosts` PCs (ids 2, 3, ...) on its first ports.
inline Scenario star(int hosts) {
    Scenario scenario;
    CHECK(netsim::edit::add_device(scenario, netsim::DeviceKind::Switch, 0, 0) == Problem::None);
    for (int i = 0; i < hosts; ++i) {
        DeviceId id{};
        CHECK(netsim::edit::add_device(scenario, netsim::DeviceKind::Host, 0, 0, &id) == Problem::None);
        CHECK(netsim::edit::connect(scenario, id, DeviceId{1}) == Problem::None);
    }
    return scenario;
}

// FNV-1a over little-endian integers: the same bytes on every platform.
struct Hash {
    std::uint64_t value = 14695981039346656037ULL;
    void add(std::uint64_t number) {
        for (int i = 0; i < 8; ++i) {
            value ^= (number >> (8 * i)) & 0xffU;
            value *= 1099511628211ULL;
        }
    }
    void add(netsim::MacAddress mac) {
        for (const auto byte : mac.bytes()) add(byte);
    }
    void add(Ipv4Address address) { add(address.bits()); }
    void add(netsim::SimTime time) { add(static_cast<std::uint64_t>(time.count())); }
    template<class T>
    void add(const std::optional<T>& item) {
        add(item.has_value());
        if (item) add(static_cast<std::uint64_t>(item->value));
    }
};

// Everything a run produced: records, frames, transmissions and pings.
inline void add_trace(Hash& hash, const Simulation& simulation) {
    const auto& trace = simulation.trace();
    for (const auto& record : trace.records()) {
        hash.add(record.time);
        hash.add(record.seq);
        hash.add(record.episode);
        hash.add(static_cast<std::uint64_t>(record.kind));
        hash.add(static_cast<std::uint64_t>(record.reason));
        hash.add(record.device.value);
        hash.add(record.port.has_value());
        hash.add(record.port.value_or(0));
        hash.add(record.link);
        hash.add(record.pdu);
        hash.add(record.value);
    }
    for (const auto& frame : trace.pdus()) {
        hash.add(frame.dst);
        hash.add(frame.src);
        hash.add(frame.type());
        if (const auto* arp = frame.arp()) {
            hash.add(static_cast<std::uint64_t>(arp->op));
            hash.add(arp->sender_mac);
            hash.add(arp->sender_ip);
            hash.add(arp->target_mac);
            hash.add(arp->target_ip);
        } else {
            const auto& packet = *frame.ipv4();
            hash.add(packet.src);
            hash.add(packet.dst);
            hash.add(packet.ttl);
            hash.add(packet.protocol);
            hash.add(packet.identification);
            hash.add(packet.icmp.type);
            hash.add(packet.icmp.code);
            hash.add(packet.icmp.identifier);
            hash.add(packet.icmp.sequence);
            hash.add(packet.icmp.data_size);
        }
    }
    for (const auto& sent : trace.transmissions()) {
        hash.add(sent.pdu.value);
        hash.add(sent.link.value);
        hash.add(sent.from.value);
        hash.add(sent.to.value);
        hash.add(sent.from_port);
        hash.add(sent.to_port);
        hash.add(sent.start);
        hash.add(sent.arrival);
        hash.add(sent.record);
        hash.add(sent.stopped);
    }
    for (const auto& status : simulation.pings()) {
        hash.add(status.id.value);
        hash.add(static_cast<std::uint64_t>(status.state));
        hash.add(static_cast<std::uint64_t>(status.reason));
        hash.add(status.started);
        hash.add(status.finished);
        for (const auto& echo : status.echoes) {
            hash.add(echo.sequence);
            hash.add(static_cast<std::uint64_t>(echo.state));
            hash.add(static_cast<std::uint64_t>(echo.reason));
            hash.add(echo.sent);
            hash.add(echo.finished);
            hash.add(echo.record);
        }
    }
}

} // namespace simtest

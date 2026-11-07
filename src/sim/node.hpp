#pragma once

#include "sim/simulation.hpp"

#include <memory>
#include <vector>

namespace netsim {

// A frame that just arrived at a port, as a node receives it. Records about
// what the node decides refer to it.
struct Arrival {
    std::uint8_t port = 0;
    LinkId link{};
    PduId pdu{};
};

enum class Transmit : std::uint8_t {
    Sent,
    NoCarrier,  // nothing recorded: the caller says what it means
    Stopped,    // the storm guard stopped this frame
};

// What a node may do to the rest of the simulation, always at the current
// time and on behalf of its own device.
class Context {
public:
    Context(Simulation& simulation, DeviceId device) : simulation_(simulation), device_(device) {}

    SimTime now() const { return simulation_.now(); }
    DeviceId device() const { return device_; }
    const DeviceConfig& config() const { return *simulation_.scenario_.device(device_); }
    const EthernetFrame& frame(PduId pdu) const { return simulation_.trace_.pdu(pdu); }
    PduId archive(const EthernetFrame& frame) { return simulation_.trace_.archive(frame); }

    // Puts the frame on the cable plugged into `port`, recording FrameSent
    // and the transmission. Without carrier (no cable, cable down, a port
    // disabled at either end, this device powered off) nothing is recorded.
    Transmit transmit(std::uint8_t port, PduId pdu, std::uint32_t value = 0);

    TimerHandle schedule(SimTime delay, TimerKind kind, std::uint64_t key);
    bool cancel(TimerHandle& handle) { return simulation_.scheduler_.cancel(handle); }

    // Appends a record at this device and returns its seq. `note` fills in
    // the arrival's port, cable and frame, for the decisions a node takes
    // about it.
    std::uint64_t record(TraceRecord record);
    void note(const Arrival& arrival, TraceKind kind, Reason reason = Reason::None);

    // Pings. A host's ping application reports here what happens to each
    // request; a ping ends when the last of its requests has an outcome.
    PingStatus& ping(PingId id) { return simulation_.pings_.at(id.value - 1); }
    // The next request of a ping, sent now. The first one starts the ping.
    EchoStatus& next_echo(PingId id);
    void echo_replied(PingId id, std::uint16_t sequence, const Arrival* arrival);
    void echo_lost(PingId id, std::uint16_t sequence, Reason reason, std::optional<PduId> request = std::nullopt);
    // An ICMP error about a request arrived from `from`: the request is lost
    // for `reason` at once.
    void echo_error(PingId id, std::uint16_t sequence, Reason reason, const Arrival& arrival, Ipv4Address from);
    // Ends a ping at once: its waiting requests are lost for `reason`.
    void abort_ping(PingId id, Reason reason);

private:
    EchoStatus& lose(PingId id, std::uint16_t sequence, Reason reason, std::optional<PduId> request);
    // Ends the ping once every request has an outcome, or at once when it
    // is aborted. A traceroute that goes on sends its next request.
    void settle(PingId id, std::optional<Reason> abort);

    Simulation& simulation_;
    DeviceId device_;
};

class Node {
public:
    virtual ~Node() = default;
    virtual void receive(Context& context, const Arrival& arrival) = 0;
    virtual void timer(Context& context, const Timer& timer) = 0;
    virtual std::vector<ArpCacheEntry> arp_cache(SimTime) const { return {}; }
    virtual std::vector<MacTableEntry> mac_table(SimTime, const DeviceConfig&) const { return {}; }
};

std::unique_ptr<Node> make_host();
std::unique_ptr<Node> make_switch();
std::unique_ptr<Node> make_router();

} // namespace netsim

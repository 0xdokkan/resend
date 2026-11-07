#pragma once

#include "core/ids.hpp"
#include "core/sim_time.hpp"
#include "sim/pdu.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace netsim {

// Facts recorded by the simulation. Records carry no text: explanations are
// derived from them, which keeps the model, its tests and the language apart.
enum class TraceKind : std::uint8_t {
    FrameSent,        // a frame left a port onto an active cable
    FrameReceived,    // a frame arrived at a port of a powered device
    FrameDropped,     // the frame stops here; the reason says why
    MacLearned,       // a switch bound the source MAC to the ingress port
    FrameFlooded,     // a switch sends the frame out of every other port
    FrameForwarded,   // a switch sends the frame out of the learned port only
    FrameFiltered,    // the learned port is the ingress port: nothing is sent
    ArpCacheUpdated,  // the receiver maps the ARP sender address to its MAC
    ArpTimeout,       // a retransmission interval passed without a reply
    PingSucceeded,    // a ping ended with at least one reply
    PingFailed,       // a ping ended without any reply; the reason is its first loss
    PingStarted,      // a host starts a ping
    PingReply,        // an Echo Reply answered one request of a ping
    PingLost,         // one request of a ping got no reply; the reason says why
    ArpEntryExpired,  // a host needed an ARP entry and found it expired
    MacEntryAged,     // a switch found the destination's entry aged out: it floods
    DatagramDropped,  // a datagram that is not a ping request could not leave the host
    EditApplied,      // the network was edited while the simulation runs
    SimulatorGuard,   // the simulator stopped a frame copied too many times: not network behaviour
};

enum class Reason : std::uint8_t {
    None,
    LinkDown,         // the cable stopped carrying while the frame was on it
    DeviceDown,       // the receiving device is off; for pings, the source went off or away
    NoCarrier,        // the sending port has no active cable
    NotForUs,         // destination MAC is neither the local MAC nor broadcast
    ArpForOtherHost,  // the ARP target address is not the local address
    Ipv4NotForUs,     // destination IPv4 address is not the local address
    NoRoute,          // destination outside the local subnet, and no routers yet
    ArpUnresolved,    // three ARP requests went unanswered
    EchoTimeout,      // no matching Echo Reply within the timeout
    ArpQueueFull,     // newer datagrams for the same unresolved address took its place
    UnmatchedReply,   // an Echo Reply no request is waiting for: it timed out or its ping ended
};

// `port` is zero-based. Switch decisions refer to the ingress port; the
// FrameSent records that follow them name the egress ports.
// `episode` groups the records produced by one scheduled event: a frame
// arriving, a timer firing, an edit. `value` is a number whose meaning
// depends on the kind:
// - the attempt, for ARP requests sent or dropped and for ArpTimeout;
// - the ping's id, for every Ping* record;
// - an IPv4 address (its 32 bits): the one whose entry expired for
//   ArpEntryExpired, the datagram's destination for DatagramDropped;
// - the edit's index in Simulation::edits(), for EditApplied;
// - the number of times the frame was sent, for SimulatorGuard;
// - 0 otherwise.
struct TraceRecord {
    SimTime time{};
    std::uint64_t seq = 0;  // position in the trace
    std::uint64_t episode = 0;
    TraceKind kind{};
    Reason reason = Reason::None;
    DeviceId device{};
    std::optional<std::uint8_t> port{};
    std::optional<LinkId> link{};
    std::optional<PduId> pdu{};
    std::uint32_t value = 0;
    bool operator==(const TraceRecord&) const = default;
};

// A frame on a cable, from the moment it leaves a port until it reaches the
// other end. What happens on arrival is in the records at `arrival`.
struct Transmission {
    PduId pdu{};
    LinkId link{};
    DeviceId from{}, to{};
    std::uint8_t from_port = 0, to_port = 0;  // zero-based
    SimTime start{}, arrival{};
    std::uint64_t record = 0;  // seq of the FrameSent record
    // Removed from the cable by the storm guard; `arrival` is then the time
    // it was removed.
    bool stopped = false;
    bool operator==(const Transmission&) const = default;
};

// Append-only records plus the frames they refer to. A frame is archived
// once and shared by every record about it, including the copies a switch
// floods, so a frame can be followed across the network by its id.
// Archived frames never move: references to them stay valid.
// Transmissions are kept in the order they start; the storm guard may cut
// one short, nothing else changes them.
class Trace {
public:
    PduId archive(const EthernetFrame& frame) {
        pdus_.push_back(frame);
        return PduId{static_cast<std::uint32_t>(pdus_.size() - 1)};
    }
    void add(SimTime time, TraceRecord record) {
        record.time = time;
        record.seq = records_.size();
        records_.push_back(record);
    }
    void add(Transmission transmission) { transmissions_.push_back(transmission); }
    void stop(std::size_t transmission, SimTime time) {
        auto& item = transmissions_.at(transmission);
        item.arrival = time;
        item.stopped = true;
    }
    void clear() {
        records_.clear();
        pdus_.clear();
        transmissions_.clear();
    }
    const EthernetFrame& pdu(PduId id) const { return pdus_.at(id.value); }
    const std::deque<EthernetFrame>& pdus() const { return pdus_; }
    const std::vector<TraceRecord>& records() const { return records_; }
    const std::vector<Transmission>& transmissions() const { return transmissions_; }

private:
    std::vector<TraceRecord> records_;
    std::deque<EthernetFrame> pdus_;
    std::vector<Transmission> transmissions_;
};

} // namespace netsim

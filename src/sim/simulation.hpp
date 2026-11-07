#pragma once

#include "core/problem.hpp"
#include "scenario/edit.hpp"
#include "scenario/scenario.hpp"
#include "sim/medium.hpp"
#include "sim/scheduler.hpp"
#include "sim/trace.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace netsim {

using PingId = Id<struct PingTag>;

// How hosts resolve addresses (RFC 826, RFC 1122 §2.3.2), as in common
// stacks' defaults: a request is repeated after a second without a reply,
// three times in all, and at most three datagrams wait for an answer; when
// a fourth comes the oldest is discarded, so the latest is always kept, as
// Linux did by default for years. An entry not refreshed for a minute
// expires, the classic BSD timeout.
constexpr SimTime arp_retry_interval = std::chrono::seconds(1);
constexpr std::uint32_t arp_attempts = 3;
constexpr std::size_t arp_queue_limit = 3;
constexpr SimTime arp_cache_timeout = std::chrono::seconds(60);

// A ping, as the ping command runs it: `count` Echo Requests, one every
// `interval`. Each request waits `timeout` for its reply from the moment it
// leaves the host, so the wait for ARP is not counted; its round trip is
// measured from the moment the ping sent it, as ping does.
struct PingOptions {
    std::uint16_t count = 1;
    SimTime interval = std::chrono::seconds(1);
    SimTime timeout = std::chrono::seconds(1);
    bool operator==(const PingOptions&) const = default;
};
constexpr std::uint16_t max_ping_count = 1000;
constexpr SimTime min_ping_time = std::chrono::milliseconds(10);
constexpr SimTime max_ping_time = std::chrono::seconds(60);
// Data bytes after the ICMP header, as ping sends by default: 64 bytes of ICMP.
constexpr std::uint16_t ping_data_size = 56;

enum class PingState : std::uint8_t { Running, Succeeded, Failed };
enum class EchoState : std::uint8_t { Waiting, Replied, Lost };

// One Echo Request of a ping.
struct EchoStatus {
    std::uint16_t sequence = 0;  // from 1
    EchoState state = EchoState::Waiting;
    Reason reason = Reason::None;  // why it was lost
    SimTime sent{}, finished{};    // handed to IPv4; replied or lost
    std::uint64_t record = 0;      // seq of its PingReply or PingLost record
    bool operator==(const EchoStatus&) const = default;
};

struct PingStatus {
    PingId id{};
    DeviceId source{};
    Ipv4Address destination;
    PingOptions options{};
    // Succeeded with at least one reply, as ping's exit status.
    PingState state = PingState::Running;
    Reason reason = Reason::None;  // the first loss, when there was no reply
    SimTime started{}, finished{};
    std::vector<EchoStatus> echoes{};  // the requests sent so far, in order
    int replies() const;
    bool operator==(const PingStatus&) const = default;
};

enum class ArpState : std::uint8_t { Incomplete, Reachable };
struct ArpCacheEntry {
    Ipv4Address ip;
    MacAddress mac;  // zero while incomplete
    ArpState state = ArpState::Reachable;
    SimTime expires{};       // reachable: when the entry expires
    std::size_t queued = 0;  // incomplete: datagrams waiting for the answer
    bool operator==(const ArpCacheEntry&) const = default;
};
struct MacTableEntry {
    MacAddress mac;
    std::uint8_t port = 0;  // zero-based
    SimTime updated{};      // when a frame from `mac` last arrived
    bool operator==(const MacTableEntry&) const = default;
};

// What the scheduler holds: a frame reaching the end of a cable, or a timer
// of a device. A timer's key means what its node decides.
enum class TimerKind : std::uint8_t { PingSend, ArpRetry, EchoTimeout };
struct Timer {
    DeviceId device{};
    TimerKind kind{};
    std::uint64_t key = 0;
};
struct Delivery {
    LinkId link{};
    PortRef to;
    PduId pdu{};
    std::uint64_t serial = 0;      // the cable's serial when the frame left
    std::size_t transmission = 0;  // index in the trace's transmissions
};
using Scheduled = std::variant<Delivery, Timer>;
using TimerHandle = Scheduler<Scheduled>::Handle;

// An edit made while the simulation ran. The device and cables it changed
// are kept as they were before it, so that removed ones can still be named.
struct EditEntry {
    SimTime time{};
    Edit edit;
    std::optional<DeviceConfig> device;
    std::vector<LinkConfig> links;
};

class Node;
class Context;

// The network in motion, built from a scenario that passes validate().
// Every step is deterministic: the same scenario and the same commands at
// the same times give the same trace, on every platform.
class Simulation {
public:
    explicit Simulation(Scenario scenario);
    ~Simulation();
    Simulation(Simulation&&) noexcept;
    Simulation& operator=(Simulation&&) noexcept;

    const Scenario& scenario() const { return scenario_; }
    const Trace& trace() const { return trace_; }
    SimTime now() const { return scheduler_.now(); }
    bool pending() const { return !scheduler_.empty(); }
    std::optional<SimTime> next_event_time() const { return scheduler_.next_time(); }
    // Runs the next scheduled event. False if there was none.
    bool step();
    // Runs every event due at or before `time`, then moves the clock there.
    void run_until(SimTime time);

    // Changes the network at the current time. Edits to names and positions
    // are taken silently; the others are recorded and take effect at once:
    // a frame on a cable that stops carrying is lost when it reaches the
    // end, a device that is powered off or removed forgets everything it
    // learned and its pings end.
    Problem apply(const Edit& edit);
    const std::vector<EditEntry>& edits() const { return edits_; }
    // A device of the scenario, or one an edit removed, as it was then.
    const DeviceConfig* known_device(DeviceId id) const;

    // Starts a ping from a host at the current time. Several pings may run
    // at once, from the same host or from different ones.
    std::optional<PingId> start_ping(DeviceId source, Ipv4Address destination, Problem& problem, const PingOptions& options = {});
    const std::vector<PingStatus>& pings() const { return pings_; }
    const PingStatus* ping(PingId id) const;

    // The state of a device's tables at the current time, in address order.
    std::vector<ArpCacheEntry> arp_cache(DeviceId device) const;
    std::vector<MacTableEntry> mac_table(DeviceId device) const;

    // Frames on the cables right now.
    std::size_t frames_in_flight() const { return in_flight_; }
    // How many times one frame may be sent before the storm guard stops it.
    // Without a loop a frame crosses each cable at most once, so a frame
    // sent four times as often as there are cables, and at least 64 times,
    // is going round a loop.
    std::uint32_t storm_limit() const;

private:
    friend class Context;
    Node* node(DeviceId device) const;
    std::uint64_t record(TraceRecord record);
    void deliver(const Delivery& delivery);
    // A device that is powered off or removed: its timers are cancelled and
    // its pings end.
    void shut_down(DeviceId device);
    // Stops every copy of a frame that went round too often.
    void guard(DeviceId device, PduId pdu);

    Scenario scenario_;
    Medium medium_{scenario_};
    Scheduler<Scheduled> scheduler_;
    Trace trace_;
    std::map<DeviceId, std::unique_ptr<Node>> nodes_;
    std::vector<PingStatus> pings_;
    std::vector<EditEntry> edits_;
    std::vector<std::uint32_t> copies_;  // transmissions per frame
    std::set<PduId> stopped_;            // frames the storm guard stopped
    std::size_t in_flight_ = 0;
    std::uint64_t episode_ = 0;
    // The highest ids the network has had, removed devices and cables included.
    std::uint32_t last_device_ = 0, last_link_ = 0;
};

} // namespace netsim

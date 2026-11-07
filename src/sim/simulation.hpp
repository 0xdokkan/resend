#pragma once

#include "core/problem.hpp"
#include "scenario/scenario.hpp"
#include "sim/medium.hpp"
#include "sim/scheduler.hpp"
#include "sim/trace.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace netsim {

using PingId = Id<struct PingTag>;

// How hosts wait, as in common stacks' defaults: an ARP request is repeated
// after a second without a reply, three times in all; a ping waits a second
// for its Echo Reply.
constexpr SimTime arp_retry_interval = std::chrono::seconds(1);
constexpr std::uint32_t arp_attempts = 3;
constexpr SimTime echo_timeout = std::chrono::seconds(1);

enum class PingState : std::uint8_t { Running, Succeeded, Failed };

struct PingStatus {
    PingId id{};
    DeviceId source{};
    Ipv4Address destination;
    PingState state = PingState::Running;
    Reason reason = Reason::None;  // why it failed
    SimTime started{}, finished{};
    bool operator==(const PingStatus&) const = default;
};

struct ArpCacheEntry {
    Ipv4Address ip;
    MacAddress mac;
    bool operator==(const ArpCacheEntry&) const = default;
};
struct MacTableEntry {
    MacAddress mac;
    std::uint8_t port = 0;  // zero-based
    bool operator==(const MacTableEntry&) const = default;
};

// What the scheduler holds: a frame reaching the end of a cable, or a timer
// of a device. A timer's key means what its node decides.
enum class TimerKind : std::uint8_t { PingStart, ArpRetry, EchoTimeout };
struct Timer {
    DeviceId device{};
    TimerKind kind{};
    std::uint64_t key = 0;
};
struct Delivery {
    LinkId link{};
    PortRef to;
    PduId pdu{};
};
using Scheduled = std::variant<Delivery, Timer>;
using TimerHandle = Scheduler<Scheduled>::Handle;

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

    // Starts a ping from a host at the current time. Several pings may run
    // at once, from the same host or from different ones.
    std::optional<PingId> start_ping(DeviceId source, Ipv4Address destination, Problem& problem);
    const std::vector<PingStatus>& pings() const { return pings_; }
    const PingStatus* ping(PingId id) const;

    // The state of a device's tables, in address order.
    std::vector<ArpCacheEntry> arp_cache(DeviceId device) const;
    std::vector<MacTableEntry> mac_table(DeviceId device) const;

private:
    friend class Context;
    Node* node(DeviceId device) const;
    void record(TraceRecord record);
    void deliver(const Delivery& delivery);

    Scenario scenario_;
    Medium medium_{scenario_};
    Scheduler<Scheduled> scheduler_;
    Trace trace_;
    std::map<DeviceId, std::unique_ptr<Node>> nodes_;
    std::vector<PingStatus> pings_;
    std::uint64_t episode_ = 0;
};

} // namespace netsim

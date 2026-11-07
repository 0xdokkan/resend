#pragma once

#include "core/problem.hpp"
#include "sim/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Readable explanations of what the simulation recorded, and of why a
// command or a scenario was refused. All the interface's English about the
// network lives here, apart from the short labels drawn on the canvas.
namespace netsim::explain {

// One step of the story: a frame arriving somewhere and what the device
// did with it, a request sent, a timer running out.
struct Event {
    SimTime time{};
    std::uint64_t episode = 0;
    // "ARP", "ICMP", "IPv4"; "EDIT" for a change to the network,
    // "SIMULATOR" for the simulator's own interventions.
    std::string protocol{};
    std::string summary{}, detail{};
    std::optional<DeviceId> from{}, to{};
    std::optional<LinkId> link{};
    std::optional<PduId> pdu{};  // the frame the event is about
    bool success = true;
    // The simulator stepped in (storm protection): never network behaviour.
    bool simulator = false;
    bool operator==(const Event&) const = default;
};

// Builds events from the records of a trace, an episode at a time.
// Call it between simulation steps, when every episode is complete.
class Explainer {
public:
    // Explains the records added since the last call.
    void update(const Simulation& simulation);
    void clear() { *this = {}; }
    const std::vector<Event>& events() const { return events_; }

private:
    std::size_t next_ = 0;  // first record not explained yet
    std::vector<Event> events_;
};

std::string describe(Problem problem);
std::string describe(Reason reason);
// A port by its device's name, counted from 1: "PC-A port 1".
std::string port_text(const Scenario& scenario, PortRef port);
// Identifiers for listings such as the CLI's trace: "FrameSent", "NoCarrier".
std::string_view name(TraceKind kind);
std::string_view name(Reason reason);

} // namespace netsim::explain

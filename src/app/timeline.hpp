#pragma once

#include "explain/explain.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace resend {

// Which events the timeline lists. Protocols are the events' own: ARP,
// ICMP and IPv4 for the network, edits for the changes made to it while it
// ran, the simulator for its storm protection.
struct EventFilter {
    bool arp = true, icmp = true, ipv4 = true, edits = true, simulator = true;
    bool problems_only = false;              // drops, losses and failures
    std::optional<netsim::DeviceId> device;  // only events from or at this device
    bool accepts(const netsim::explain::Event& event) const;
    // Whether some events may be hidden.
    bool active() const;
    bool operator==(const EventFilter&) const = default;
};

// The indexes of the events the filter accepts, in order.
std::vector<std::size_t> filter_events(const std::vector<netsim::explain::Event>& events, const EventFilter& filter);

} // namespace resend

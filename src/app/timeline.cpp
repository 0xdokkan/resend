#include "app/timeline.hpp"

namespace resend {

bool EventFilter::accepts(const netsim::explain::Event& event) const {
    const auto& protocol = event.protocol;
    const bool shown = protocol == "ARP" ? arp
        : protocol == "ICMP" ? icmp
        : protocol == "IPv4" ? ipv4
        : protocol == "EDIT" ? edits
        : protocol == "SIMULATOR" ? simulator
        : true;
    if (!shown) return false;
    if (problems_only && event.success) return false;
    return !device || event.from == device || event.to == device;
}

bool EventFilter::active() const { return *this != EventFilter{}; }

std::vector<std::size_t> filter_events(const std::vector<netsim::explain::Event>& events, const EventFilter& filter) {
    std::vector<std::size_t> shown;
    shown.reserve(events.size());
    for (std::size_t index = 0; index < events.size(); ++index)
        if (filter.accepts(events[index])) shown.push_back(index);
    return shown;
}

} // namespace resend

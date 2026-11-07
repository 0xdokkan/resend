#pragma once

#include "scenario/scenario.hpp"

#include <cstdint>
#include <map>
#include <optional>

namespace netsim {

// The cables: which port each one joins, how long a frame takes on it, and
// whether a port has carrier. Rebuilt from the scenario after every edit.
//
// Each cable has a serial number, which changes whenever the cable stops
// carrying frames: it goes down, a port at either end is disabled, an end
// is moved or the cable is unplugged. A frame remembers the serial it was
// sent with; if the serial has changed when it reaches the far end, the
// frame was lost on the way.
class Medium {
public:
    struct Path {
        LinkId link{};
        PortRef to;
        SimTime delay{};
        std::uint64_t serial = 0;
    };

    explicit Medium(const Scenario& scenario) { update(scenario); }
    void update(const Scenario& scenario);
    // Where a frame sent from `port` goes, if the port has carrier: a cable
    // that is up, enabled ports at both ends and a powered sender.
    std::optional<Path> path(PortRef port) const;
    // Whether a frame sent on `link` with `serial` can still arrive.
    bool intact(LinkId link, std::uint64_t serial) const;

private:
    struct End {
        Path path;
        bool carrier = false;
    };
    struct Cable {
        PortRef a, b;
        bool usable = false;  // up, with both ports enabled
        std::uint64_t serial = 0;
    };
    std::map<PortRef, End> ends_;
    std::map<LinkId, Cable> cables_;
    std::uint64_t last_serial_ = 0;
};

} // namespace netsim

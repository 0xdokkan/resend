#pragma once

#include "scenario/scenario.hpp"

#include <map>
#include <optional>

namespace netsim {

// The cables: which port each one joins, how long a frame takes on it, and
// whether a port has carrier. Built once from the scenario.
class Medium {
public:
    struct Path {
        LinkId link{};
        PortRef to;
        SimTime delay{};
    };

    explicit Medium(const Scenario& scenario);
    // Where a frame sent from `port` goes, if the port has carrier: a cable
    // that is up, enabled ports at both ends and a powered sender.
    std::optional<Path> path(PortRef port) const;
    bool up(LinkId link) const;

private:
    struct End {
        Path path;
        bool carrier = false;
    };
    std::map<PortRef, End> ends_;
    std::map<LinkId, bool> up_;
};

} // namespace netsim

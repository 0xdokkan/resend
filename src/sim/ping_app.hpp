#pragma once

#include "sim/nic.hpp"

#include <map>

namespace netsim {

// The ping command of one host. Each ping sends one Echo Request whose
// identifier is the ping's id, and succeeds when the matching Echo Reply
// arrives within echo_timeout of the request leaving.
class PingApp {
public:
    static IcmpMessage request(PingId id) { return {icmp::echo_request, 0, static_cast<std::uint16_t>(id.value), 1}; }
    // The request left: the timeout starts.
    void sent(Context& context, PingId id);
    // An Echo Reply reached the host. True if it ended a ping.
    bool reply(Context& context, const Arrival& arrival, Ipv4Address from, const IcmpMessage& message);
    void timeout(Context& context, PingId id);

private:
    std::map<std::uint32_t, TimerHandle> waiting_;  // by ping id
};

} // namespace netsim

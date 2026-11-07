#pragma once

#include "sim/nic.hpp"

#include <map>

namespace netsim {

// The ping command of one host, which also runs traceroutes. Each request
// of ping `id` carries the id as its ICMP identifier and its number, from 1,
// as its sequence; it succeeds when the matching Echo Reply arrives within
// the ping's timeout of the request leaving the host, and fails at once if
// an ICMP error about it arrives first.
class PingApp {
public:
    // The ping's next Echo Request, as a datagram from `source`. Schedules
    // the request after it, if any.
    Outgoing request(Context& context, PingId id, Ipv4Address source);
    // The request left the host as frame `pdu`: its timeout starts.
    void sent(Context& context, PingId id, std::uint16_t sequence, PduId pdu);
    // An Echo Reply reached the host. True if it answered a waiting request.
    bool reply(Context& context, const Arrival& arrival, Ipv4Address from, const IcmpMessage& message);
    // An ICMP error reached the host. True if it was about a waiting
    // request, which is then lost.
    bool error(Context& context, const Arrival& arrival, const Ipv4Packet& packet);
    void timeout(Context& context, std::uint64_t key);

    static std::uint64_t timer_key(PingId id, std::uint16_t sequence) { return std::uint64_t{id.value} << 16 | sequence; }

private:
    struct Waiting {
        TimerHandle timer;
        PduId request{};
    };
    // The waiting request with this identifier and sequence, sent to `to`.
    std::map<std::uint64_t, Waiting>::iterator find(Context& context, std::uint16_t identifier, std::uint16_t sequence, Ipv4Address to);
    std::map<std::uint64_t, Waiting> waiting_;  // by timer key
};

} // namespace netsim

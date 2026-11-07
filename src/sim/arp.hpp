#pragma once

#include "sim/nic.hpp"

#include <deque>
#include <map>
#include <vector>

namespace netsim {

// Address resolution on one interface (RFC 826, RFC 1122 §2.3.2): the cache
// with its timeouts, the requests repeated while nobody answers, and the
// datagrams waiting for an answer. It never sends datagrams itself: it hands
// them back once they can leave, or once they cannot.
class Arp {
public:
    // The MAC of `address` if the cache has a valid entry. An entry found
    // expired is removed, and the expiry recorded.
    std::optional<MacAddress> lookup(Context& context, const Nic& nic, Ipv4Address address);
    // Queues the datagram until `next_hop` is resolved, asking for it unless
    // a request is already out. If the queue was full, returns the oldest
    // datagram, which has to be discarded.
    std::optional<Outgoing> resolve(Context& context, const Nic& nic, Ipv4Address next_hop, Outgoing outgoing);
    // An ARP packet that reached the interface. Updates the cache, answers
    // requests for the interface's address, and returns the datagrams that
    // waited for the sender, which can now leave for the sender's MAC.
    std::vector<Outgoing> receive(Context& context, const Nic& nic, const Arrival& arrival, const ArpPacket& packet);
    // The retry timer for `next_hop`: asks again, or after the last attempt
    // gives up and returns the datagrams that waited.
    std::vector<Outgoing> retry(Context& context, const Nic& nic, Ipv4Address next_hop);
    std::vector<ArpCacheEntry> entries(SimTime now) const;

    // The timer key for a resolution on an interface.
    static std::uint64_t timer_key(const Nic& nic, Ipv4Address next_hop) {
        return std::uint64_t{nic.port} << 32 | next_hop.bits();
    }
    static Ipv4Address timer_address(std::uint64_t key) { return Ipv4Address{static_cast<std::uint32_t>(key)}; }

private:
    struct Entry {
        MacAddress mac;
        SimTime updated{};
        bool valid(SimTime now) const { return now - updated < arp_cache_timeout; }
    };
    struct Resolution {
        std::uint32_t attempt = 0;
        TimerHandle timer;
        std::deque<Outgoing> waiting;
    };
    void request(Context& context, const Nic& nic, Ipv4Address next_hop, Resolution& resolution);

    std::map<Ipv4Address, Entry> cache_;
    std::map<Ipv4Address, Resolution> resolving_;  // the incomplete entries
};

} // namespace netsim

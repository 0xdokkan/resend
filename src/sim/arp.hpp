#pragma once

#include "sim/nic.hpp"

#include <map>
#include <vector>

namespace netsim {

// Address resolution on one interface (RFC 826): the cache, the requests
// repeated while nobody answers, and the datagrams waiting for an answer.
// It never sends datagrams itself: it hands them back once they can leave,
// or once they cannot.
class Arp {
public:
    std::optional<MacAddress> lookup(Ipv4Address address) const;
    // Queues the datagram until `next_hop` is resolved, asking for it unless
    // a request is already out.
    void resolve(Context& context, const Nic& nic, Ipv4Address next_hop, Outgoing outgoing);
    // An ARP packet that reached the interface. Updates the cache, answers
    // requests for the interface's address, and returns the datagrams that
    // waited for the sender, which is now in the cache.
    std::vector<Outgoing> receive(Context& context, const Nic& nic, const Arrival& arrival, const ArpPacket& packet);
    // The retry timer for `next_hop`: asks again, or after the last attempt
    // gives up and returns the datagrams that waited.
    std::vector<Outgoing> retry(Context& context, const Nic& nic, Ipv4Address next_hop);
    std::vector<ArpCacheEntry> entries() const;

    // The timer key for a resolution on an interface.
    static std::uint64_t timer_key(const Nic& nic, Ipv4Address next_hop) {
        return std::uint64_t{nic.port} << 32 | next_hop.bits();
    }
    static Ipv4Address timer_address(std::uint64_t key) { return Ipv4Address{static_cast<std::uint32_t>(key)}; }

private:
    struct Resolution {
        std::uint32_t attempt = 0;
        TimerHandle timer;
        std::vector<Outgoing> waiting;
    };
    void request(Context& context, const Nic& nic, Ipv4Address next_hop, Resolution& resolution);

    std::map<Ipv4Address, MacAddress> cache_;
    std::map<Ipv4Address, Resolution> resolving_;
};

} // namespace netsim

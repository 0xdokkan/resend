#include "sim/node.hpp"

#include <map>

namespace netsim {
namespace {

// A transparent learning bridge (IEEE 802.1D without spanning tree): it
// learns where each source MAC lives, then forwards, filters or floods.
// Entries never age: a simulation lasts seconds, the usual age is minutes.
class Bridge final : public Node {
public:
    void receive(Context& context, const Arrival& arrival) override {
        const auto& frame = context.frame(arrival.pdu);
        const auto [entry, added] = table_.try_emplace(frame.src, arrival.port);
        if (added || entry->second != arrival.port) {
            entry->second = arrival.port;
            context.note(arrival, TraceKind::MacLearned);
        }

        const auto destination = table_.find(frame.dst);
        if (frame.dst.is_broadcast() || destination == table_.end()) {
            context.note(arrival, TraceKind::FrameFlooded);
            const auto ports = context.config().ports.size();
            for (std::uint8_t port = 0; port < ports; ++port)
                if (port != arrival.port) context.transmit(port, arrival.pdu);
        } else if (destination->second == arrival.port) {
            context.note(arrival, TraceKind::FrameFiltered);
        } else {
            context.note(arrival, TraceKind::FrameForwarded);
            const auto egress = destination->second;
            if (!context.transmit(egress, arrival.pdu))
                context.record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .port = egress, .pdu = arrival.pdu});
        }
    }

    void timer(Context&, const Timer&) override {}

    std::vector<MacTableEntry> mac_table() const override {
        std::vector<MacTableEntry> entries;
        for (const auto& [mac, port] : table_) entries.push_back({mac, port});
        return entries;
    }

private:
    std::map<MacAddress, std::uint8_t> table_;
};

} // namespace

std::unique_ptr<Node> make_switch() { return std::make_unique<Bridge>(); }

} // namespace netsim

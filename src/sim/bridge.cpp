#include "sim/node.hpp"

#include <map>

namespace netsim {
namespace {

// A transparent learning bridge (IEEE 802.1D without spanning tree): it
// learns where each source MAC lives, then forwards, filters or floods. An
// address not seen again within the switch's ageing time is forgotten.
class Bridge final : public Node {
public:
    void receive(Context& context, const Arrival& arrival) override {
        const auto& frame = context.frame(arrival.pdu);
        const auto now = context.now();
        const auto ageing = context.config().mac_ageing;
        const auto fresh = [&](const Entry& entry) { return now - entry.updated < ageing; };

        const auto [source, added] = table_.try_emplace(frame.src, Entry{arrival.port, now});
        const bool learned = added || source->second.port != arrival.port || !fresh(source->second);
        source->second = {arrival.port, now};
        if (learned) context.note(arrival, TraceKind::MacLearned);

        auto destination = table_.find(frame.dst);
        if (destination != table_.end() && !fresh(destination->second)) {
            context.note(arrival, TraceKind::MacEntryAged);
            table_.erase(destination);
            destination = table_.end();
        }
        if (frame.dst.is_broadcast() || destination == table_.end()) {
            context.note(arrival, TraceKind::FrameFlooded);
            const auto ports = context.config().ports.size();
            for (std::uint8_t port = 0; port < ports; ++port)
                if (port != arrival.port && context.transmit(port, arrival.pdu) == Transmit::Stopped) break;
        } else if (destination->second.port == arrival.port) {
            context.note(arrival, TraceKind::FrameFiltered);
        } else {
            context.note(arrival, TraceKind::FrameForwarded);
            const auto egress = destination->second.port;
            if (context.transmit(egress, arrival.pdu) == Transmit::NoCarrier)
                context.record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .port = egress, .pdu = arrival.pdu});
        }
    }

    void timer(Context&, const Timer&) override {}

    std::vector<MacTableEntry> mac_table(SimTime now, const DeviceConfig& config) const override {
        std::vector<MacTableEntry> entries;
        for (const auto& [mac, entry] : table_)
            if (now - entry.updated < config.mac_ageing) entries.push_back({mac, entry.port, entry.updated});
        return entries;
    }

private:
    struct Entry {
        std::uint8_t port = 0;
        SimTime updated{};
    };
    std::map<MacAddress, Entry> table_;
};

} // namespace

std::unique_ptr<Node> make_switch() { return std::make_unique<Bridge>(); }

} // namespace netsim

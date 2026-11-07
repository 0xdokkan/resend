#pragma once

#include "sim/node.hpp"

#include <optional>

namespace netsim {

// One Ethernet interface of a host (later, of a router): a port, its MAC
// and its IPv4 address, read from the device's configuration.
struct Nic {
    std::uint8_t port = 0;

    MacAddress mac(const Context& context) const { return context.config().ports.at(port).mac; }
    Ipv4Prefix address(const Context& context) const { return *context.config().ports.at(port).ipv4; }
    // Frames for this interface: its own MAC or broadcast.
    bool accepts(const Context& context, const EthernetFrame& frame) const {
        return frame.dst == mac(context) || frame.dst.is_broadcast();
    }
    // Sends a new frame from this port. Without carrier it is dropped here.
    PduId send(Context& context, const EthernetFrame& frame, std::uint32_t value = 0) const {
        const auto pdu = context.archive(frame);
        if (context.transmit(port, pdu, value) == Transmit::NoCarrier)
            context.record({.kind = TraceKind::FrameDropped, .reason = Reason::NoCarrier, .port = port, .pdu = pdu, .value = value});
        return pdu;
    }
};

// An IPv4 datagram on its way out, and the ping it belongs to, if any: the
// ping's reply timeout starts when the datagram leaves.
struct Outgoing {
    Ipv4Packet packet;
    std::optional<PingId> ping;
};

} // namespace netsim

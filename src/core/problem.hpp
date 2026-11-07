#pragma once

#include <cstdint>

namespace netsim {

// Why a command or a scenario was refused. Codes only: the explain module
// turns them into text, so the model stays independent of the language.
enum class Problem : std::uint8_t {
    None,
    // Scenario structure
    DuplicateDeviceId,
    InvalidDeviceId,
    InvalidName,
    WrongPortCount,
    MissingAddress,       // a host port without an IPv4 address
    InvalidHostAddress,   // network, broadcast, loopback, multicast, /0
    InvalidPrefix,        // a prefix length outside 1..32
    AddressOnSwitch,      // a switch port with an IPv4 address
    DuplicateLinkId,
    InvalidLinkId,
    NoSuchDevice,
    NoSuchPort,
    NoSuchLink,
    PortInUse,            // a port already has a cable
    SameDevice,           // a cable between two ports of one device
    InvalidDelay,         // outside 1 µs .. 10 s
    LoopNotSupported,     // an Ethernet loop, refused until storm protection exists
    TooManyDevices,
    NoFreePort,
    NotAHost,
    // Ping
    SourceNotHost,
    SourcePoweredOff,
    InvalidDestination,   // not a unicast address
    BroadcastDestination, // the network or broadcast address of the source's subnet
};

constexpr bool ok(Problem problem) { return problem == Problem::None; }

} // namespace netsim

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
    WrongPortCount,       // a host has 1 port, a switch 8, a router 2 to 8
    MissingAddress,       // a host port without an IPv4 address: refused before scenario format 3, unused since
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
    InvalidAgeing,        // a switch's MAC ageing time outside 10 s .. 1,000,000 s
    TooManyDevices,
    NoFreePort,
    NotAHost,
    NotASwitch,
    // Ping
    SourceNotHost,
    SourcePoweredOff,
    InvalidDestination,   // not a unicast address
    BroadcastDestination, // the network or broadcast address of the source's subnet
    InvalidPingOptions,   // count, interval, timeout or TTL out of range
    // Routing; new codes go at the end, as tests hash them
    NotARouter,
    InvalidGateway,       // not a unicast address, or on a device that is not a host
    InvalidRoute,         // host bits in the destination, a next hop that is not unicast, or not on a router
    DuplicateRoute,       // two routes of a router to the same destination
    TooManyRoutes,
    // Labs
    Locked,               // the lab keeps the device or the setting as it gave it
};

constexpr bool ok(Problem problem) { return problem == Problem::None; }

} // namespace netsim

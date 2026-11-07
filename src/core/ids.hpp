#pragma once

#include <compare>
#include <cstdint>

namespace netsim {

// Identifier that only compares with identifiers of the same kind, so a link
// id cannot be passed where a device id is expected.
template<class Tag>
struct Id {
    std::uint32_t value = 0;
    constexpr auto operator<=>(const Id&) const = default;
};

using DeviceId = Id<struct DeviceTag>;
using LinkId = Id<struct LinkTag>;
using PduId = Id<struct PduTag>;  // index in the trace's PDU archive

} // namespace netsim

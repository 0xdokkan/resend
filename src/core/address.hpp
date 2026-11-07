#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace netsim {

// IPv4 address, stored in host byte order.
class Ipv4Address {
public:
    constexpr Ipv4Address() = default;
    constexpr explicit Ipv4Address(std::uint32_t bits) : bits_(bits) {}
    // Dotted quad only: four decimal octets without leading zeros or extra text.
    static std::optional<Ipv4Address> parse(std::string_view text);
    constexpr std::uint32_t bits() const { return bits_; }
    std::string to_string() const;
    constexpr auto operator<=>(const Ipv4Address&) const = default;

private:
    std::uint32_t bits_ = 0;
};

// Network mask for a prefix length from 0 to 32.
constexpr std::uint32_t prefix_mask(int length) {
    return length <= 0 ? 0 : length >= 32 ? 0xffffffffU : 0xffffffffU << (32 - length);
}

class MacAddress {
public:
    using Bytes = std::array<std::uint8_t, 6>;
    constexpr MacAddress() = default;
    constexpr explicit MacAddress(Bytes bytes) : bytes_(bytes) {}
    static constexpr MacAddress broadcast() { return MacAddress(Bytes{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}); }
    // Locally administered unicast address 02:00:xx:xx:xx:xx built from an id.
    static MacAddress local(std::uint32_t id);
    constexpr const Bytes& bytes() const { return bytes_; }
    constexpr bool is_broadcast() const { return *this == broadcast(); }
    std::string to_string() const;  // lowercase hex, colon separated
    constexpr auto operator<=>(const MacAddress&) const = default;

private:
    Bytes bytes_{};
};

} // namespace netsim

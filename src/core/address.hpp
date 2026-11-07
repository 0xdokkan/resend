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

// Unicast in the sense of RFC 1122 §3.2.1.3: not "this network" (0/8),
// loopback (127/8), multicast (224/4) or reserved (240/4).
constexpr bool is_unicast(Ipv4Address address) {
    const auto first = address.bits() >> 24;
    return first != 0 && first != 127 && first < 224;
}

// An address with a prefix length: an interface address such as
// 192.168.1.10/24, or a network such as 192.168.1.0/24.
class Ipv4Prefix {
public:
    constexpr Ipv4Prefix() = default;
    // `length` is clamped to 0..32; the host bits are kept.
    constexpr Ipv4Prefix(Ipv4Address address, int length)
        : address_(address), length_(length < 0 ? 0 : length > 32 ? 32 : length) {}
    // "a.b.c.d/n" with n from 0 to 32, no spaces or leading zeros.
    static std::optional<Ipv4Prefix> parse(std::string_view text);
    constexpr Ipv4Address address() const { return address_; }
    constexpr int length() const { return length_; }
    constexpr std::uint32_t mask() const { return prefix_mask(length_); }
    constexpr Ipv4Address network() const { return Ipv4Address(address_.bits() & mask()); }
    constexpr Ipv4Address broadcast() const { return Ipv4Address(address_.bits() | ~mask()); }
    constexpr bool contains(Ipv4Address other) const { return (other.bits() & mask()) == network().bits(); }
    constexpr Ipv4Prefix canonical() const { return {network(), length_}; }
    // Whether the address may be given to an interface on this prefix:
    // unicast, and neither the network nor the broadcast address, except on
    // /31 (RFC 3021) and /32 links, which have neither.
    constexpr bool usable_host() const {
        if (!is_unicast(address_) || length_ == 0) return false;
        if (length_ >= 31) return true;
        return address_ != network() && address_ != broadcast();
    }
    std::string to_string() const;
    constexpr auto operator<=>(const Ipv4Prefix&) const = default;

private:
    Ipv4Address address_;
    int length_ = 0;
};

class MacAddress {
public:
    using Bytes = std::array<std::uint8_t, 6>;
    constexpr MacAddress() = default;
    constexpr explicit MacAddress(Bytes bytes) : bytes_(bytes) {}
    static constexpr MacAddress broadcast() { return MacAddress(Bytes{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}); }
    // Locally administered unicast address 02:00:xx:xx:xx:xx built from an id.
    static MacAddress local(std::uint32_t id);
    // Six pairs of hex digits separated by colons, either case.
    static std::optional<MacAddress> parse(std::string_view text);
    constexpr const Bytes& bytes() const { return bytes_; }
    constexpr bool is_broadcast() const { return *this == broadcast(); }
    std::string to_string() const;  // lowercase hex, colon separated
    constexpr auto operator<=>(const MacAddress&) const = default;

private:
    Bytes bytes_{};
};

} // namespace netsim

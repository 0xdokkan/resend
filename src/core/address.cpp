#include "core/address.hpp"

#include <charconv>

namespace netsim {

std::optional<Ipv4Address> Ipv4Address::parse(std::string_view text) {
    std::uint32_t result = 0;
    std::size_t begin = 0;
    for (int i = 0; i < 4; ++i) {
        const auto end = text.find('.', begin);
        if ((i < 3 && end == std::string_view::npos) || (i == 3 && end != std::string_view::npos)) return {};
        const auto count = (end == std::string_view::npos ? text.size() : end) - begin;
        if (count == 0 || count > 3 || (count > 1 && text[begin] == '0')) return {};
        unsigned value = 0;
        const auto* first = text.data() + begin;
        const auto conversion = std::from_chars(first, first + count, value);
        if (conversion.ec != std::errc{} || conversion.ptr != first + count || value > 255) return {};
        result = (result << 8) | value;
        begin = end == std::string_view::npos ? text.size() : end + 1;
    }
    return Ipv4Address(result);
}

std::string Ipv4Address::to_string() const {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        if (shift != 24) out += '.';
        out += std::to_string((bits_ >> shift) & 0xff);
    }
    return out;
}

std::optional<Ipv4Prefix> Ipv4Prefix::parse(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) return {};
    const auto address = Ipv4Address::parse(text.substr(0, slash));
    const auto digits = text.substr(slash + 1);
    if (!address || digits.empty() || digits.size() > 2 || (digits.size() > 1 && digits[0] == '0')) return {};
    int length = 0;
    const auto conversion = std::from_chars(digits.data(), digits.data() + digits.size(), length);
    if (conversion.ec != std::errc{} || conversion.ptr != digits.data() + digits.size() || length > 32) return {};
    return Ipv4Prefix(*address, length);
}

std::string Ipv4Prefix::to_string() const { return address_.to_string() + "/" + std::to_string(length_); }

std::optional<MacAddress> MacAddress::parse(std::string_view text) {
    if (text.size() != 17) return {};
    Bytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0 && text[i * 3 - 1] != ':') return {};
        unsigned value = 0;
        const auto* first = text.data() + i * 3;
        const auto conversion = std::from_chars(first, first + 2, value, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != first + 2) return {};
        bytes[i] = static_cast<std::uint8_t>(value);
    }
    return MacAddress(bytes);
}

MacAddress MacAddress::local(std::uint32_t id) {
    return MacAddress(Bytes{0x02, 0x00, static_cast<std::uint8_t>(id >> 24), static_cast<std::uint8_t>(id >> 16),
                            static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id)});
}

std::string MacAddress::to_string() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i) out += ':';
        out += digits[bytes_[i] >> 4];
        out += digits[bytes_[i] & 0xf];
    }
    return out;
}

} // namespace netsim

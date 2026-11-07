#include "core/sim_time.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace netsim {

std::optional<SimTime> parse_duration(std::string_view text) {
    struct Unit { std::string_view suffix; std::int64_t nanoseconds; };
    static constexpr std::array<Unit, 5> units{{{"ns", 1}, {"us", 1'000}, {"µs", 1'000}, {"ms", 1'000'000}, {"s", 1'000'000'000}}};
    std::int64_t scale = 0;
    for (const auto& unit : units) {
        if (text.size() > unit.suffix.size() && text.ends_with(unit.suffix)) {
            scale = unit.nanoseconds;
            text.remove_suffix(unit.suffix.size());
            break;
        }
    }
    if (scale == 0 || text.empty() || text.front() == '.' || text.back() == '.') return std::nullopt;

    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    std::int64_t whole = 0, fraction = 0, divisor = 1;
    bool after_point = false;
    for (const char c : text) {
        if (c == '.' && !after_point) { after_point = true; continue; }
        if (c < '0' || c > '9') return std::nullopt;
        const int digit = c - '0';
        if (!after_point) {
            if (whole > (limit - digit) / 10) return std::nullopt;
            whole = whole * 10 + digit;
        } else {
            if (divisor >= scale) {
                if (digit != 0) return std::nullopt;  // finer than a nanosecond
                continue;
            }
            fraction = fraction * 10 + digit;
            divisor *= 10;
        }
    }
    if (whole > limit / scale) return std::nullopt;
    // `divisor` never exceeds `scale`, a power of ten, so this is exact.
    return SimTime{whole * scale + fraction * (scale / divisor)};
}

std::string format_ms(SimTime time) {
    const auto ns = time.count();
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%lld.%03lld ms", static_cast<long long>(ns / 1'000'000),
                  static_cast<long long>(ns % 1'000'000 / 1'000));
    return buffer;
}

} // namespace netsim

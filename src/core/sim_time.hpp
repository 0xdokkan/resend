#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace netsim {

// Simulated time since the start of the simulation. Nanoseconds leave room
// for serialization delays (1500 bytes at 1 Gb/s take 12 µs).
using SimTime = std::chrono::duration<std::int64_t, std::nano>;

// A non-negative decimal number followed by a unit, without spaces:
// "15ms", "1.5s", "250us", "40ns". Values that are not a whole number of
// nanoseconds, or that do not fit, are rejected.
std::optional<SimTime> parse_duration(std::string_view text);

// Milliseconds with three decimals, e.g. "15.000 ms" or "1015.250 ms".
std::string format_ms(SimTime time);

} // namespace netsim

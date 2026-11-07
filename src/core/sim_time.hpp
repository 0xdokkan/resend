#pragma once

#include <chrono>
#include <cstdint>

namespace netsim {

// Simulated time since the start of the simulation. Nanoseconds leave room
// for serialization delays (1500 bytes at 1 Gb/s take 12 µs).
using SimTime = std::chrono::duration<std::int64_t, std::nano>;

} // namespace netsim

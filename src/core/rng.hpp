#pragma once

#include <cstdint>
#include <random>

namespace netsim {

// Seeded random numbers that are identical on every platform. The standard
// fixes mt19937_64's output, but not what the std::uniform_*_distribution
// classes make of it, which differs between libstdc++, libc++ and MSVC:
// ranges are derived here instead.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : engine_(seed) {}
    std::uint64_t next() { return engine_(); }
    // Uniform in [0, bound), bound > 0, by rejection: no modulo bias.
    std::uint64_t below(std::uint64_t bound) {
        const std::uint64_t limit = UINT64_MAX - UINT64_MAX % bound;
        std::uint64_t value = 0;
        do value = engine_();
        while (value >= limit);
        return value % bound;
    }
    // Uniform in [low, high].
    std::int64_t between(std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low) + 1));
    }
    // True with probability numerator / denominator.
    bool chance(std::uint64_t numerator, std::uint64_t denominator) { return below(denominator) < numerator; }

private:
    std::mt19937_64 engine_;
};

} // namespace netsim

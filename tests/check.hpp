#pragma once

#include <cstdlib>
#include <iostream>

// Minimal assertion helpers shared by the test executables: the first failure
// prints the expression and stops the process with a non-zero exit code.
namespace test {
inline int checks = 0;
inline void require(bool condition, const char* expression, const char* file, int line) {
    ++checks;
    if (!condition) {
        std::cerr << "FAILED " << file << ':' << line << ": " << expression << '\n';
        std::exit(1);
    }
}
} // namespace test

#define CHECK(expression) ::test::require((expression), #expression, __FILE__, __LINE__)

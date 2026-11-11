#include "core/text.hpp"

#include <cctype>

namespace netsim::text {

std::string format(std::string_view pattern, std::initializer_list<std::string_view> arguments) {
    std::string text;
    text.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if ((c == '{' || c == '}') && i + 1 < pattern.size() && pattern[i + 1] == c) {
            text += c;
            ++i;
            continue;
        }
        if (c == '{') {
            const auto close = pattern.find('}', i);
            if (close != std::string_view::npos && close > i + 1) {
                std::size_t number = 0;
                bool digits = true;
                for (std::size_t j = i + 1; j < close; ++j) {
                    if (!std::isdigit(static_cast<unsigned char>(pattern[j]))) digits = false;
                    else number = number * 10 + static_cast<std::size_t>(pattern[j] - '0');
                }
                if (digits && number < arguments.size()) {
                    text += arguments.begin()[number];
                    i = close;
                    continue;
                }
            }
        }
        text += c;
    }
    return text;
}

std::string tr(std::string_view english) { return std::string(english); }

std::string tr(std::string_view english, std::initializer_list<std::string_view> arguments) { return format(english, arguments); }

} // namespace netsim::text

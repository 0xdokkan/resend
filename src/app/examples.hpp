#pragma once

#include <span>
#include <string_view>

namespace resend {

// The scenarios of examples/, built into the app in the order a newcomer
// would open them, each with its file name and its text.
struct Example {
    std::string_view file;
    std::string_view text;
};
std::span<const Example> examples();

} // namespace resend

#pragma once

#include <span>
#include <string_view>

namespace resend {

// The labs of labs/, built into the app in the order of its library, each
// with its file name and its text.
struct LabFile {
    std::string_view file;
    std::string_view text;
};
std::span<const LabFile> lab_files();

} // namespace resend

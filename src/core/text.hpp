#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

// Every text resend writes for people: explanations, labels, status
// messages. resend speaks English only; the texts still go through tr(), so
// they stay easy to find (as @tr() in the interface) and their arguments are
// put in one way.
namespace netsim::text {

// `english` with its arguments in place of {0}, {1}...
std::string tr(std::string_view english);
std::string tr(std::string_view english, std::initializer_list<std::string_view> arguments);
// Replaces {0}, {1}... in `pattern`; {{ and }} stand for braces.
std::string format(std::string_view pattern, std::initializer_list<std::string_view> arguments);

} // namespace netsim::text

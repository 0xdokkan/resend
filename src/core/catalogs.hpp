#pragma once

#include <span>
#include <string_view>

namespace netsim::text {

// The translations of translations/<language>/LC_MESSAGES/resend.po,
// built in by cmake/embed_catalogs.cmake.
struct BuiltInCatalog {
    std::string_view language;
    std::string_view po;
};
std::span<const BuiltInCatalog> built_in_catalogs();

} // namespace netsim::text

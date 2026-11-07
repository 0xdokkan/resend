#pragma once

#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The language of every text resend writes for people: explanations,
// labels, status messages. Texts are written in English in the code and
// looked up in the catalog of the current language, the same gettext .po
// files the interface uses; a text without a translation stays in English.
namespace netsim::text {

// A catalog: English text to translation.
using Catalog = std::map<std::string, std::string, std::less<>>;
// Reads the entries of a .po file. Entries without a translation, fuzzy
// ones and the header are left out. Nothing if the file is malformed.
std::optional<Catalog> parse_po(std::string_view po);

// The languages built in, English first: language codes such as "it".
std::vector<std::string> languages();
// Selects a built-in language; "en" or an empty code selects English.
// False, and English, for a language that is not built in.
bool set_language(std::string_view code);
const std::string& language();
// The language of the user's system, e.g. "it" for it_IT.UTF-8, among
// the built-in ones; "en" otherwise.
std::string system_language();

// `english` in the current language, its arguments in place of {0}, {1}...
std::string tr(std::string_view english);
std::string tr(std::string_view english, std::initializer_list<std::string_view> arguments);
// Replaces {0}, {1}... in `pattern`; {{ and }} stand for braces.
std::string format(std::string_view pattern, std::initializer_list<std::string_view> arguments);

} // namespace netsim::text

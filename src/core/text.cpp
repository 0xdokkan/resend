#include "core/text.hpp"

#include "core/catalogs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace netsim::text {
namespace {
struct State {
    std::string language = "en";
    Catalog catalog;
};
State& state() {
    static State current;
    return current;
}

// The text of a quoted .po string, escapes resolved.
std::optional<std::string> unquote(std::string_view line) {
    if (line.size() < 2 || line.front() != '"' || line.back() != '"') return std::nullopt;
    std::string text;
    for (std::size_t i = 1; i + 1 < line.size(); ++i) {
        if (line[i] != '\\') {
            text += line[i];
            continue;
        }
        if (++i >= line.size() - 1) return std::nullopt;  // a backslash before the closing quote
        switch (line[i]) {
        case 'n': text += '\n'; break;
        case 't': text += '\t'; break;
        case '"': text += '"'; break;
        case '\\': text += '\\'; break;
        default: return std::nullopt;
        }
    }
    return text;
}

std::string_view trim(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
    return line;
}
} // namespace

std::optional<Catalog> parse_po(std::string_view po) {
    Catalog catalog;
    std::string id, translation, ignored;
    std::string* current = nullptr;
    bool fuzzy = false, has_id = false, plural = false;
    // Plural entries are for the interface's @tr(... | ... % n): the C++
    // texts choose between two entries instead.
    const auto finish = [&] {
        if (has_id && !id.empty() && !translation.empty() && !fuzzy && !plural) catalog[id] = translation;
        id.clear();
        translation.clear();
        ignored.clear();
        current = nullptr;
        fuzzy = has_id = plural = false;
    };
    while (!po.empty()) {
        const auto end = po.find('\n');
        auto line = trim(po.substr(0, end));
        po.remove_prefix(end == std::string_view::npos ? po.size() : end + 1);
        if (line.empty()) {
            finish();
            continue;
        }
        if (line.starts_with("#,")) {
            if (has_id) finish();
            fuzzy = line.find("fuzzy") != std::string_view::npos;
            continue;
        }
        if (line.front() == '#') continue;
        const auto keyword = [&](std::string_view name) {
            return line.starts_with(name) && line.size() > name.size() && line[name.size()] == ' ';
        };
        std::string* target = nullptr;
        if (keyword("msgctxt")) {
            if (has_id) {
                const bool keep = fuzzy;
                finish();
                fuzzy = keep;
            }
            line.remove_prefix(7);
            current = nullptr;
            if (!unquote(trim(line))) return std::nullopt;
            continue;
        }
        if (keyword("msgid")) {
            if (has_id) {
                const bool keep = fuzzy;
                finish();
                fuzzy = keep;
            }
            has_id = true;
            target = &id;
            line.remove_prefix(5);
        } else if (keyword("msgid_plural")) {
            if (!has_id) return std::nullopt;
            plural = true;
            target = &ignored;
            line.remove_prefix(12);
        } else if (keyword("msgstr")) {
            if (!has_id) return std::nullopt;
            target = &translation;
            line.remove_prefix(6);
        } else if (line.starts_with("msgstr[")) {
            const auto close = line.find("] ");
            if (!has_id || close == std::string_view::npos) return std::nullopt;
            plural = true;
            target = &ignored;
            line.remove_prefix(close + 1);
        } else if (line.front() == '"') {
            if (!current) return std::nullopt;
            target = current;
        } else {
            return std::nullopt;
        }
        const auto text = unquote(trim(line));
        if (!text) return std::nullopt;
        *target += *text;
        current = target;
    }
    finish();
    return catalog;
}

std::vector<std::string> languages() {
    std::vector<std::string> codes{"en"};
    for (const auto& item : built_in_catalogs()) codes.emplace_back(item.language);
    return codes;
}

bool set_language(std::string_view code) {
    auto& current = state();
    current.catalog.clear();
    current.language = "en";
    if (code.empty() || code == "en") return true;
    for (const auto& item : built_in_catalogs()) {
        if (item.language != code) continue;
        auto catalog = parse_po(item.po);
        if (!catalog) return false;
        current.catalog = std::move(*catalog);
        current.language = std::string(code);
        return true;
    }
    return false;
}

const std::string& language() { return state().language; }

std::string system_language() {
    std::string locale;
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
        for (const wchar_t* c = name; *c && *c < 128; ++c) locale += static_cast<char>(*c);
#else
    // LANGUAGE lists preferences separated by colons; the others are locales.
    for (const char* variable : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(variable);
        if (!value || !*value) continue;
        locale = value;
        locale = locale.substr(0, locale.find(':'));
        if (!locale.empty()) break;
    }
#endif
    std::string code;
    for (const char c : locale) {
        if (!std::isalpha(static_cast<unsigned char>(c))) break;
        code += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto known = languages();
    return std::find(known.begin(), known.end(), code) != known.end() ? code : "en";
}

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

std::string tr(std::string_view english) {
    const auto& catalog = state().catalog;
    const auto found = catalog.find(english);
    return found == catalog.end() ? std::string(english) : found->second;
}

std::string tr(std::string_view english, std::initializer_list<std::string_view> arguments) {
    const auto& catalog = state().catalog;
    const auto found = catalog.find(english);
    return format(found == catalog.end() ? english : std::string_view(found->second), arguments);
}

} // namespace netsim::text

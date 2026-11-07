// The exit check of milestone M4 for translations: every text written for
// people, @tr() in the interface, tr() in the C++ code and the titles and
// notes of the examples, has a translation in every built-in language, with
// the same placeholders, and no catalog keeps entries nothing uses.
#include "check.hpp"
#include "core/text.hpp"
#include "explain/explain.hpp"
#include "scenario/scenario_json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace text = netsim::text;

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::size_t skip_space(const std::string& source, std::size_t at) {
    while (at < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[at]))) ++at;
        else if (source.compare(at, 2, "//") == 0) at = source.find('\n', at);
        else break;
        if (at == std::string::npos) return source.size();
    }
    return at;
}

// The string literal at `at`, escapes resolved; `at` moves past it.
std::string literal(const std::string& source, std::size_t& at) {
    std::string value;
    for (++at; at < source.size() && source[at] != '"'; ++at) {
        if (source[at] != '\\') {
            value += source[at];
            continue;
        }
        switch (source[++at]) {
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        default: value += source[at];
        }
    }
    ++at;
    return value;
}

// The English texts of a source file: tr("…") in C++, where adjacent
// literals join, and @tr("…") in Slint. A tr() whose first argument is not
// a literal is not a text.
void collect(const fs::path& path, std::map<std::string, std::set<std::string>>& texts) {
    const auto source = read_file(path);
    const bool slint = path.extension() == ".slint";
    const std::string call = slint ? "@tr(" : "tr(";
    for (auto at = source.find(call); at != std::string::npos; at = source.find(call, at + 1)) {
        if (!slint && at > 0) {
            const auto before = static_cast<unsigned char>(source[at - 1]);
            if (std::isalnum(before) || before == '_' || before == '@') continue;
        }
        auto next = skip_space(source, at + call.size());
        if (next >= source.size() || source[next] != '"') continue;
        std::string english;
        while (next < source.size() && source[next] == '"') {
            english += literal(source, next);
            if (slint) break;
            next = skip_space(source, next);
        }
        texts[english].insert(fs::relative(path, RESEND_SOURCE_DIR).generic_string());
    }
}

std::multiset<std::string> placeholders(const std::string& value) {
    std::multiset<std::string> found;
    for (auto open = value.find('{'); open != std::string::npos; open = value.find('{', open + 1)) {
        const auto close = value.find('}', open);
        if (close == std::string::npos) break;
        const auto inside = value.substr(open + 1, close - open - 1);
        if (!inside.empty() && std::all_of(inside.begin(), inside.end(), [](unsigned char c) { return std::isdigit(c); }))
            found.insert(inside);
    }
    return found;
}
} // namespace

int main() {
    std::map<std::string, std::set<std::string>> texts;
    for (const auto* folder : {"src", "ui"}) {
        for (const auto& item : fs::recursive_directory_iterator(fs::path(RESEND_SOURCE_DIR) / folder)) {
            const auto extension = item.path().extension();
            if (extension == ".cpp" || extension == ".hpp" || extension == ".slint") collect(item.path(), texts);
        }
    }
    // The app shows the title and notes of an example through the catalog.
    int examples = 0;
    for (const auto& item : fs::directory_iterator(fs::path(RESEND_SOURCE_DIR) / "examples")) {
        if (item.path().extension() != ".json") continue;
        std::string error;
        const auto scenario = netsim::scenario_from_json(read_file(item.path()), error);
        CHECK(scenario.has_value());
        if (!scenario) continue;
        ++examples;
        const auto file = fs::relative(item.path(), RESEND_SOURCE_DIR).generic_string();
        for (const auto* value : {&scenario->title, &scenario->notes})
            if (!value->empty()) texts[*value].insert(file);
    }
    CHECK(examples >= 6);
    // A sweep that finds nothing would pass everything.
    CHECK(texts.size() > 400);
    CHECK(texts.contains("Ping succeeded · {0} → {1}") && texts.contains("Open…") && texts.contains("Destination"));

    int languages = 0;
    for (const auto& language : text::languages()) {
        if (language == "en") continue;
        ++languages;
        const auto po = fs::path(RESEND_SOURCE_DIR) / "translations" / language / "LC_MESSAGES" / "resend.po";
        const auto catalog = text::parse_po(read_file(po));
        CHECK(catalog.has_value());
        if (!catalog) continue;
        for (const auto& [english, files] : texts) {
            const auto found = catalog->find(english);
            const bool translated = found != catalog->end();
            CHECK(translated);
            if (!translated) {
                std::cout << "  " << language << ": no translation for \"" << english << "\" (" << *files.begin() << ")\n";
                continue;
            }
            const bool same = placeholders(found->second) == placeholders(english);
            CHECK(same);
            if (!same) std::cout << "  " << language << ": other placeholders in \"" << found->second << "\"\n";
        }
        for (const auto& [english, translation] : *catalog) {
            const bool used = texts.contains(english);
            CHECK(used);
            if (!used) std::cout << "  " << language << ": \"" << english << "\" is not used any more\n";
        }
        // The catalog built into the program is this file.
        CHECK(text::set_language(language));
        for (const auto& [english, translation] : *catalog) CHECK(text::tr(english) == translation);
    }
    CHECK(languages >= 1);

    // Explanations follow the language.
    CHECK(text::set_language("it"));
    CHECK(netsim::explain::describe(netsim::Problem::NoFreePort) == "Nessuna porta libera: un PC ha una porta, uno switch ne ha otto.");
    CHECK(text::set_language("en"));
    CHECK(netsim::explain::describe(netsim::Problem::NoFreePort) == "No free port: a PC has one port, a switch has eight.");

    std::cout << "Translations complete: " << texts.size() << " texts in " << languages << " language(s), " << test::checks << " checks.\n";
}

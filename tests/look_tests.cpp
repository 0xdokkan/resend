// The exit checks of milestone G1 that need no window: the themes of
// assets/palettes.json, the sprite sheets of assets/sprites/, the rule that
// keeps pixels square, and a sweep of ui/ for small text and colours that
// are not tokens.
#include "check.hpp"
#include "look/look.hpp"
#include "app/scene.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace look = resend::look;

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Pairs of text and the surface it is drawn on, as the interface uses them.
void contrast_of_every_pair() {
    const auto& palettes = look::palettes();
    CHECK(palettes.themes.size() == 3);
    CHECK(palettes.default_id == "blueberry" && palettes.find("").id == "blueberry");
    CHECK(palettes.find("picnic").id == "picnic" && !palettes.find("picnic").dark);
    CHECK(palettes.find("blueberry").credit == "Based on Sweetie 16 by GrafxKid");
    int low = 0;
    for (const auto& theme : palettes.themes) {
        const auto pair = [&](const std::string& what, look::Rgb text, look::Rgb surface) {
            const double ratio = look::contrast(text, surface);
            if (ratio < 4.5) {
                ++low;
                std::cout << "  " << theme.id << ": " << what << " has a contrast of " << ratio << ":1\n";
            }
        };
        CHECK(theme.colours().size() <= look::max_colours);
        for (const auto* ink : {"ink", "muted"})
            for (const auto* surface : {"panel", "base", "canvas", "raised", "sunken"})
                pair(std::string(ink) + " on " + surface, theme.ink(ink), theme.surface(surface));
        pair("ink on hover", theme.ink("ink"), theme.surface("hover"));
        pair("screen-ink on screen", theme.ink("screen-ink"), theme.surface("screen"));
        pair("on-accent on accent", theme.ink("on-accent"), theme.family("accent").tones[2]);
        for (const auto name : look::family_names) {
            if (name == "metal") continue;  // bodies of devices, never text
            const auto& family = theme.family(name);
            for (const auto* surface : {"panel", "canvas", "raised", "sunken"})
                pair(std::string(name) + " text on " + surface, family.text, theme.surface(surface));
            pair(std::string(name) + " chip", family.chip_ink, family.chip);
        }
    }
    CHECK(low == 0);
}

void palette_files_are_strict() {
    std::string error;
    CHECK(!look::read_palettes("{}", error) && !error.empty());
    auto text = std::string(look::palettes_text());
    // A misspelt name is refused, not ignored.
    auto misspelt = text;
    misspelt.replace(misspelt.find("\"muted\""), 7, "\"muteed\"");
    CHECK(!look::read_palettes(misspelt, error) && error.find("muted") != std::string::npos);
    auto bad = text;
    bad.replace(bad.find("#f4f4f4"), 7, "#f4f4f");
    CHECK(!look::read_palettes(bad, error));
    CHECK(look::parse_colour("#1A1c2C") == (look::Rgb{0x1a, 0x1c, 0x2c}));
    CHECK(look::format_colour({0x1a, 0x1c, 0x2c}) == "#1a1c2c");
    CHECK(std::abs(look::contrast({0, 0, 0}, {255, 255, 255}) - 21) < 1e-9);
}

void sprites_are_on_the_legend() {
    // Every built-in sheet reads without a problem.
    std::size_t total = 0;
    for (const auto& [name, text] : look::sheet_texts()) {
        std::vector<std::string> errors;
        total += look::read_sheet(name, text, errors).size();
        for (const auto& error : errors) std::cout << "  " << error << '\n';
        CHECK(errors.empty());
    }
    CHECK(total == look::sprites().size() && total >= 40);
    for (const auto* name : {"host", "switch", "router", "host-small", "arp-request", "echo-reply", "play", "pause",
                             "undo", "redo", "check", "warning", "power", "logo"})
        CHECK(look::find_sprite(name) != nullptr);
    // Every cell has a colour in every theme, and sprites are painted opaque
    // or transparent, nothing in between.
    for (const auto& theme : look::palettes().themes)
        for (const auto& sprite : look::sprites()) {
            const auto pixels = look::paint(sprite, theme);
            CHECK(pixels.size() == sprite.cells.size() * 4);
            for (std::size_t i = 0; i < sprite.cells.size(); ++i) {
                CHECK((sprite.cells[i] == '.') == (pixels[i * 4 + 3] == 0));
                CHECK(pixels[i * 4 + 3] == 0 || pixels[i * 4 + 3] == 255);
            }
        }
    // Mistakes in a sheet are found, with their line.
    std::vector<std::string> errors;
    look::read_sheet("broken", "sprite a 8 8\n........\n", errors);
    CHECK(errors.size() == 1 && errors[0].starts_with("broken:3: a: row 2"));
    errors.clear();
    look::read_sheet("broken", "sprite b 8 8\n" + std::string(8 * 9, '.'), errors);
    CHECK(errors.size() == 1);
    errors.clear();
    std::string grid;
    for (int row = 0; row < 8; ++row) grid += row == 3 ? "...Z....\n" : "........\n";
    look::read_sheet("broken", "sprite c 8 8\n" + grid, errors);
    CHECK(errors.size() == 1 && errors[0].find("'Z' is not in the legend") != std::string::npos);
    errors.clear();
    look::read_sheet("broken", "sprite d 12 8\n", errors);
    CHECK(errors.size() == 1 && errors[0].find("multiples of 8") != std::string::npos);
}

// legend.txt documents the characters; the code is what reads them. Each
// line of the form "#   1 2 3 4   metal" or "#   #   outline" must agree.
void legend_matches_the_code() {
    std::istringstream lines{std::string(look::legend_text())};
    std::string line;
    int described = 0;
    while (std::getline(lines, line)) {
        if (!line.starts_with("#")) continue;
        std::istringstream words(line.substr(1));
        std::vector<std::string> chars;
        std::string word;
        while (words >> word && word.size() == 1) chars.push_back(word);
        if (chars.empty() || word.empty() || word.size() == 1) continue;
        static const std::string_view tones[] = {"-dd", "-d", "", "-l"};
        for (std::size_t i = 0; i < chars.size(); ++i) {
            const auto expected = chars.size() == 4 ? word + std::string(tones[i]) : word;
            CHECK(look::role_of(chars[i][0]) == expected);
            ++described;
        }
    }
    CHECK(described == 5 + 8 * 4);
    CHECK(look::role_of('.').empty() && look::role_of('Z').empty());
}

// One art pixel is a whole number of physical pixels at every scale factor
// and zoom, and positions land on physical pixels.
void pixels_stay_square() {
    for (const float factor : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f}) {
        for (int step = 35; step <= 220; ++step) {
            const float zoom = static_cast<float>(step) / 100;
            const float unit = look::art_pixel(3 * zoom, factor);
            const float physical = unit * factor;
            CHECK(physical >= 1 && std::abs(physical - std::round(physical)) < 1e-4);
            // Never more than half a physical pixel from what was asked, past the minimum.
            CHECK(physical == 1 || std::abs(physical - 3 * zoom * factor) <= 0.5f + 1e-4);
            const float x = look::snap(123.456f * zoom, factor);
            CHECK(std::abs(x * factor - std::round(x * factor)) < 1e-3);
            // A sprite whose corner is snapped ends on a physical pixel too.
            const float right = x + 32 * unit;
            CHECK(std::abs(right * factor - std::round(right * factor)) < 1e-3);
        }
    }
    CHECK(look::art_pixel(0.1f, 1) == 1 && look::art_pixel(3, 0) == 3);
    // The canvas's rule, as app_look.cpp applies it: whole physical pixels,
    // the 32 px set no larger than the zoom gives it, the small set below 1×.
    for (const float factor : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f})
        for (int step = 20; step <= 400; ++step) {
            const float zoom = static_cast<float>(step) / 100;
            const auto art = look::canvas_art(zoom, factor);
            const float physical = art.unit * factor;
            CHECK(physical >= 1 && std::abs(physical - std::round(physical)) < 1e-4);
            CHECK(art.small == (std::round(zoom * factor * look::icon_room / 32) < 1));
            // Each unit is the nearest whole number of physical pixels to what is asked.
            const auto nearest = [&](float unit, float wanted) {
                const float physical = unit * factor;
                return physical >= 1 && std::abs(physical - std::round(physical)) < 1e-4 &&
                       (physical == 1 || std::abs(physical - wanted * factor) <= 0.5f + 1e-4f);
            };
            CHECK(nearest(art.unit, zoom * look::icon_room / art.size()));
            CHECK(nearest(art.port_unit, zoom) && nearest(art.packet_unit, 1.5f * zoom));
            // The drawing is never more than half an art pixel larger than its room.
            CHECK(art.size() * art.unit <= std::max(art.size() / factor, look::icon_room * zoom + art.size() * 0.5f / factor) + 1e-3f);
            const float x = look::snap(87.3f * zoom, factor);
            const float right = x + static_cast<float>(art.size()) * art.unit;
            CHECK(std::abs(right * factor - std::round(right * factor)) < 1e-3);
        }
    CHECK(!look::canvas_art(1, 1).small && look::canvas_art(1, 1).unit == 1 && look::canvas_art(1.5f, 1).unit == 2);
    CHECK(look::canvas_art(1, 1).packet_unit == 2 && look::canvas_art(0.6f, 1).packet_unit == 1 && look::canvas_art(1.5f, 1).port_unit == 2);
    CHECK(!look::canvas_art(0.6f, 1).small && look::canvas_art(0.4f, 1).small && look::canvas_art(0.4f, 1).unit == 1);
    CHECK(look::canvas_art(0.6f, 2).unit == 0.5f);
}

// No text below 12 px and no colour written in ui/ outside the tokens; the
// pixel font, whose C, 7 and 2 read as O, 1 and 8, for titles only.
void interface_uses_the_tokens() {
    const std::regex size(R"(font-size:\s*([0-9.]+)px)");
    const std::regex pixel_font(R"(font-family:[^;]*Type\.pixel\b)");
    const std::regex colour(R"((#[0-9a-fA-F]{3,8}\b)|\brgba?\s*\(|\bhsv\s*\()");
    const std::set<int> scale = {12, 14, 16, 20, 28};
    // What scrolls shows a ScrollBar of the HUD's own: the standard bar is
    // two pixels wide until pointed at, and went unseen. So nothing is a
    // ScrollView (the Scroll of widgets.slint is in its place), and a list
    // hides its bar, to carry a ScrollBar beside it.
    const std::regex scroll_view(R"(\bScrollView\s*\{)");
    const std::regex list_view(R"(\bListView\s*\{)");
    // Slint 1.18 measures a wrapped text written straight under an `if` or
    // a `for` as one line, unless that text is the element repeated: a
    // panel that holds one comes out shorter than its content, and its end
    // cannot be scrolled to. So what an `if` or a `for` repeats is a
    // component of its own whenever a text in it wraps.
    const std::regex repeated(R"(^\s*(if|for)\b.*:\s*([A-Za-z][\w-]*)\s*\{)");
    const std::regex wraps(R"(wrap:\s*(word|char)-wrap|\bNote\s*\{)");
    int files = 0, bad = 0, lists = 0, hidden_bars = 0, bars = 0;
    for (const auto& item : fs::directory_iterator(fs::path(RESEND_SOURCE_DIR) / "ui")) {
        if (item.path().extension() != ".slint") continue;
        ++files;
        std::istringstream source(read_file(item.path()));
        std::string line;
        // A line that chose the pixel font: its size is on it or on the next.
        std::string pixel_line;
        // The `if` or `for` being read, when what it repeats is not a text:
        // how deep its braces are, and where it started.
        int repeated_depth = 0, repeated_line = 0;
        for (int number = 1; std::getline(source, line); ++number) {
            const auto code = line.substr(0, line.find("//"));
            const auto where = item.path().filename().string() + ":" + std::to_string(number);
            std::smatch found;
            auto inside = code;
            if (repeated_depth == 0 && std::regex_search(code, found, repeated) && found[2] != "Text" && found[2] != "Note") {
                repeated_line = number;
                inside = code.substr(code.find('{', static_cast<std::size_t>(found.position(2))) + 1);
            }
            if (repeated_line != 0) {
                if (std::regex_search(inside, wraps)) {
                    ++bad;
                    std::cout << "  " << where << ": a text that wraps under the if or for of line " << repeated_line
                              << ": make what is repeated a component\n";
                }
                repeated_depth += static_cast<int>(std::count(code.begin(), code.end(), '{')) -
                                  static_cast<int>(std::count(code.begin(), code.end(), '}'));
                if (repeated_depth <= 0) repeated_depth = repeated_line = 0;
            }
            if (!pixel_line.empty() && pixel_line.find("Type.pixel-title") == std::string::npos &&
                code.find("Type.pixel-title") == std::string::npos) {
                ++bad;
                std::cout << "  " << where << ": the pixel font at a size other than Type.pixel-title: " << pixel_line << '\n';
            }
            pixel_line = std::regex_search(code, pixel_font) ? code : std::string();
            for (std::sregex_iterator match(code.begin(), code.end(), size), end; match != end; ++match)
                if (!scale.contains(std::stoi((*match)[1]))) {
                    ++bad;
                    std::cout << "  " << where << ": font-size " << (*match)[1] << "px is not on the scale 12, 14, 16, 20, 28\n";
                }
            if (std::regex_search(code, colour)) {
                ++bad;
                std::cout << "  " << where << ": a colour that is not a token: " << line << '\n';
            }
            if (std::regex_search(code, scroll_view)) {
                ++bad;
                std::cout << "  " << where << ": a ScrollView with the standard scroll bar: use Scroll\n";
            }
            if (item.path().filename() != "widgets.slint") {
                lists += std::regex_search(code, list_view);
                hidden_bars += code.find("vertical-scrollbar-policy: always-off") != std::string::npos;
                bars += code.find("ScrollBar {") != std::string::npos;
            }
        }
    }
    CHECK(files >= 8);
    CHECK(bad == 0);
    CHECK(lists >= 4 && hidden_bars == lists && bars >= lists);
}

} // namespace

// The app icon's files: the checksums against their published check
// values, then each PNG read back, stored block by stored block.
void icon_files_read_back() {
    using namespace resend::look;
    CHECK(crc32("123456789") == 0xcbf43926u);
    CHECK(adler32("Wikipedia") == 0x11e60398u);
    const auto u32 = [](const std::string& bytes, std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at])) << 24 |
               static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 1])) << 16 |
               static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 2])) << 8 | static_cast<std::uint8_t>(bytes[at + 3]);
    };
    const auto icons = app_icons();
    CHECK(icons.size() == 7 && icons.front().size == 16 && icons.back().size == 256);
    const auto& theme = palettes().find(palettes().default_id);
    for (const auto& [size, file] : icons) {
        CHECK(file.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
        // Chunks: each one's CRC covers its type and data.
        std::string data;
        std::size_t at = 8;
        bool ended = false;
        while (at + 12 <= file.size()) {
            const auto length = u32(file, at);
            const auto type = file.substr(at + 4, 4);
            CHECK(crc32(std::string_view(file).substr(at + 4, 4 + length)) == u32(file, at + 8 + length));
            if (type == "IHDR") CHECK(u32(file, at + 8) == static_cast<std::uint32_t>(size) && u32(file, at + 12) == static_cast<std::uint32_t>(size));
            if (type == "IDAT") data += file.substr(at + 8, length);
            if (type == "IEND") ended = true;
            at += 12 + length;
        }
        CHECK(ended && at == file.size());
        // zlib: a header, stored blocks, Adler-32 of what they hold.
        CHECK(data.size() > 6 && (static_cast<std::uint8_t>(data[0]) * 256 + static_cast<std::uint8_t>(data[1])) % 31 == 0);
        std::string raw;
        std::size_t in = 2;
        for (bool last = false; !last;) {
            last = data[in] & 1;
            CHECK((data[in] & 6) == 0);  // stored
            const auto len = static_cast<std::uint8_t>(data[in + 1]) | static_cast<std::uint8_t>(data[in + 2]) << 8;
            const auto nlen = static_cast<std::uint8_t>(data[in + 3]) | static_cast<std::uint8_t>(data[in + 4]) << 8;
            CHECK((len ^ 0xffff) == nlen);
            raw += data.substr(in + 5, static_cast<std::size_t>(len));
            in += 5 + static_cast<std::size_t>(len);
        }
        CHECK(u32(data, in) == adler32(raw) && in + 4 == data.size());
        CHECK(raw.size() == static_cast<std::size_t>(size) * (1 + static_cast<std::size_t>(size) * 4));
        // The drawing, enlarged by whole pixels: the middle pixel of the icon
        // is the painted cell under it.
        const auto* sprite = find_sprite(size < 32 ? "logo" : "app-icon");
        const int factor = size < 32 ? 1 : std::max(1, size / 32);
        const int left = (size - sprite->width * factor) / 2;
        const int x = size / 2, y = size / 2;
        const auto painted = paint(*sprite, theme);
        const auto cell = static_cast<std::size_t>((y - left) / factor * sprite->width + (x - left) / factor) * 4;
        const auto pixel = static_cast<std::size_t>(y) * (1 + static_cast<std::size_t>(size) * 4) + 1 + static_cast<std::size_t>(x) * 4;
        for (std::size_t c = 0; c < 4; ++c) CHECK(static_cast<std::uint8_t>(raw[pixel + c]) == painted[cell + c]);
    }
    // The ICO: a directory of every PNG, at the offsets it gives.
    std::vector<IconImage> images;
    for (const auto& [size, file] : icons) images.push_back({size, file});
    const auto file = ico(images);
    const auto u16le = [&](std::size_t at) { return static_cast<std::uint8_t>(file[at]) | static_cast<std::uint8_t>(file[at + 1]) << 8; };
    const auto u32le = [&](std::size_t at) { return static_cast<std::uint32_t>(u16le(at)) | static_cast<std::uint32_t>(u16le(at + 2)) << 16; };
    CHECK(u16le(0) == 0 && u16le(2) == 1 && u16le(4) == static_cast<int>(images.size()));
    for (std::size_t i = 0; i < images.size(); ++i) {
        const auto entry = 6 + 16 * i;
        CHECK(static_cast<std::uint8_t>(file[entry]) == (images[i].size == 256 ? 0 : images[i].size));
        CHECK(u32le(entry + 8) == images[i].png.size());
        CHECK(file.compare(u32le(entry + 12), images[i].png.size(), images[i].png) == 0);
    }
}

// G3's exit checks: every kind of frame and every state of a device has a
// sprite, each different from the others; and no two frames have the same
// shape once reduced to one colour.
void every_state_has_a_sprite() {
    // Every PduKind: the list of kinds
    // holds each value of the enum once, up to the last one.
    std::set<int> kinds;
    for (const auto kind : resend::pdu_kinds) kinds.insert(static_cast<int>(kind));
    CHECK(kinds.size() == resend::pdu_kinds.size());
    CHECK(*kinds.begin() == 0 && *kinds.rbegin() == static_cast<int>(resend::pdu_kinds.size()) - 1);
    CHECK(kinds.contains(static_cast<int>(netsim::PduKind::Unreachable)) && kinds.contains(static_cast<int>(netsim::PduKind::TimeExceeded)));
    std::set<std::string_view> names;
    for (const auto kind : resend::pdu_kinds) {
        const auto name = resend::packet_sprite(kind);
        CHECK(look::find_sprite(name) != nullptr);
        names.insert(name);
    }
    CHECK(names.size() == resend::pdu_kinds.size());

    // Silhouettes: which cells are drawn at all.
    std::map<std::string, std::string_view> shapes;
    for (const auto name : names) {
        const auto* sprite = look::find_sprite(name);
        std::string shape = sprite->cells;
        for (auto& cell : shape) cell = cell == '.' ? '.' : '#';
        const auto [at, fresh] = shapes.emplace(shape, name);
        if (!fresh) std::cerr << "  same shape: " << name << " and " << at->second << '\n';
        CHECK(fresh);
    }

    // Devices: six states each, all different, in every theme and both sizes;
    // the device's own colours are kept unless it is off.
    for (const auto& theme : look::palettes().themes)
        for (const auto* name : {"host", "switch", "router", "host-small", "switch-small", "router-small"}) {
            const auto* sprite = look::find_sprite(name);
            CHECK(sprite != nullptr && sprite->cells.find('o') != std::string::npos);
            std::set<std::vector<std::uint8_t>> painted;
            for (const auto state : look::device_states) painted.insert(look::paint(*sprite, theme, state));
            CHECK(painted.size() == look::device_states.size());
            CHECK(look::paint(*sprite, theme, look::DeviceState::On) == look::paint(*sprite, theme));
            // Off: nothing but metal, outline, screen, well and ink's metal.
            for (const char cell : sprite->cells) {
                const char off = look::in_state(cell, look::DeviceState::Off);
                CHECK(off == '.' || off == '#' || off == ':' || off == ',' || std::string_view("1234").find(off) != std::string_view::npos);
            }
        }
    // Port lights: four states, all different.
    const auto* port = look::find_sprite("port");
    CHECK(port != nullptr);
    for (const auto& theme : look::palettes().themes) {
        std::set<std::vector<std::uint8_t>> painted;
        for (const auto state : look::port_states) painted.insert(look::paint(*port, theme, state));
        CHECK(painted.size() == look::port_states.size());
    }
    CHECK(look::state_name(look::DeviceState::Receiving) == "receiving" && look::state_name(look::PortState::Down) == "down");
    // The grid's tile: opaque, its edges in the grid's colour, the rest the canvas's.
    const auto& blueberry = look::palettes().find("blueberry");
    const auto tile = look::grid_tile(blueberry, 28);
    const auto at = [&](int x, int y) { return look::Rgb{tile[(y * 28 + x) * 4], tile[(y * 28 + x) * 4 + 1], tile[(y * 28 + x) * 4 + 2]}; };
    CHECK(tile.size() == 28u * 28u * 4u);
    for (std::size_t i = 3; i < tile.size(); i += 4) CHECK(tile[i] == 255);
    CHECK(at(5, 5) == blueberry.surface("canvas") && at(10, 0) == blueberry.surface("grid") && at(0, 10) == blueberry.surface("grid"));
    CHECK(at(1, 0) == blueberry.surface("edge") && at(27, 0) == blueberry.surface("edge"));
}

// The GSUB feature tags of a TrueType font, in the order of its list.
std::vector<std::string> gsub_features(const std::string& font) {
    const auto u16 = [&](std::size_t at) { return static_cast<std::size_t>(static_cast<unsigned char>(font[at]) << 8 | static_cast<unsigned char>(font[at + 1])); };
    const auto u32 = [&](std::size_t at) { return u16(at) << 16 | u16(at + 2); };
    std::vector<std::string> tags;
    for (std::size_t i = 0; i < u16(4); ++i) {
        const auto entry = 12 + 16 * i;
        if (font.compare(entry, 4, "GSUB") != 0) continue;
        const auto gsub = u32(entry + 8);
        const auto list = gsub + u16(gsub + 6);
        for (std::size_t f = 0; f < u16(list); ++f) tags.push_back(font.substr(list + 2 + 6 * f, 4));
    }
    return tags;
}

void pixel_font_has_no_default_ligatures() {
    // Pixelify Sans's "fi" reads as another letter at the interface's sizes:
    // scripts/pixel-font-ligatures.py turns the default ligatures off.
    for (const auto* name : {"PixelifySans-Regular.ttf", "PixelifySans-SemiBold.ttf"}) {
        std::ifstream file(std::filesystem::path(RESEND_SOURCE_DIR) / "assets" / "fonts" / name, std::ios::binary);
        const std::string font{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto tags = gsub_features(font);
        CHECK(tags.size() > 5);
        CHECK(std::find(tags.begin(), tags.end(), "liga") == tags.end());
        CHECK(std::find(tags.begin(), tags.end(), "hlig") != tags.end());
        CHECK(std::is_sorted(tags.begin(), tags.end()));
    }
}

int main() {
    pixel_font_has_no_default_ligatures();
    contrast_of_every_pair();
    palette_files_are_strict();
    sprites_are_on_the_legend();
    legend_matches_the_code();
    pixels_stay_square();
    interface_uses_the_tokens();
    icon_files_read_back();
    every_state_has_a_sprite();
    std::cout << "Look tests passed: " << test::checks << " checks.\n";
}

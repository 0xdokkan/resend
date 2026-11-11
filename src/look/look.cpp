#include "look/look.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace resend::look {
namespace json = netsim::json;

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

double channel(std::uint8_t value) {
    const double v = value / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

template <std::size_t N>
std::size_t index_of(const std::array<std::string_view, N>& names, std::string_view name) {
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) throw std::out_of_range("no colour named " + std::string(name));
    return static_cast<std::size_t>(found - names.begin());
}

// The characters of each family's tones, darkest first, as legend.txt lists them.
struct FamilyChars {
    std::string_view family;
    std::string_view chars;
};
constexpr FamilyChars family_chars[] = {
    {"metal", "1234"}, {"ok", "abcd"}, {"link", "efgh"}, {"ask", "ijkl"},
    {"route", "mnpq"}, {"accent", "rstu"}, {"drop", "vwxy"}, {"guard", "ABCD"},
};
constexpr std::string_view tone_suffixes[] = {"-dd", "-d", "", "-l"};

bool read_colour(const json::Value* value, Rgb& out) {
    const auto* text = value ? value->string() : nullptr;
    const auto colour = text ? parse_colour(*text) : std::nullopt;
    if (colour) out = *colour;
    return colour.has_value();
}

std::optional<Theme> read_theme(const json::Value& item, std::string& error) {
    Theme theme;
    const auto* id = item.find("id");
    const auto* title = item.find("title");
    const auto* dark = item.find("dark");
    if (!id || !id->string() || !title || !title->string() || !dark || !dark->boolean()) {
        error = "a theme needs an id, a title and \"dark\"";
        return std::nullopt;
    }
    theme.id = *id->string();
    theme.title = *title->string();
    theme.dark = *dark->boolean();
    if (const auto* credit = item.find("credit"); credit && credit->string()) theme.credit = *credit->string();
    const auto* surfaces = item.find("surfaces");
    const auto* inks = item.find("inks");
    const auto* families = item.find("families");
    if (!surfaces || !surfaces->members() || !inks || !inks->members() || !families || !families->members()) {
        error = theme.id + ": surfaces, inks and families are objects";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < surface_names.size(); ++i)
        if (!read_colour(surfaces->find(surface_names[i]), theme.surfaces[i])) {
            error = theme.id + ": surface \"" + std::string(surface_names[i]) + "\" is missing or not #rrggbb";
            return std::nullopt;
        }
    for (std::size_t i = 0; i < ink_names.size(); ++i)
        if (!read_colour(inks->find(ink_names[i]), theme.inks[i])) {
            error = theme.id + ": ink \"" + std::string(ink_names[i]) + "\" is missing or not #rrggbb";
            return std::nullopt;
        }
    for (std::size_t i = 0; i < family_names.size(); ++i) {
        const auto* tones = families->find(family_names[i]);
        const auto* items = tones ? tones->array_items() : nullptr;
        auto& family = theme.families[i];
        bool ok = items && items->size() == 4;
        for (std::size_t t = 0; ok && t < 4; ++t) ok = read_colour(&(*items)[t], family.tones[t]);
        if (!ok) {
            error = theme.id + ": family \"" + std::string(family_names[i]) + "\" needs four #rrggbb tones";
            return std::nullopt;
        }
        family.text = theme.dark ? family.tones[2] : family.tones[0];
        family.chip = theme.dark ? family.tones[0] : family.tones[3];
        family.chip_ink = theme.dark ? family.tones[3] : family.tones[0];
    }
    // Members the format does not know are refused, as in the scenario files.
    for (const auto& [name, value] : *surfaces->members())
        if (std::find(surface_names.begin(), surface_names.end(), name) == surface_names.end()) {
            error = theme.id + ": unknown surface \"" + name + "\"";
            return std::nullopt;
        }
    for (const auto& [name, value] : *inks->members())
        if (std::find(ink_names.begin(), ink_names.end(), name) == ink_names.end()) {
            error = theme.id + ": unknown ink \"" + name + "\"";
            return std::nullopt;
        }
    for (const auto& [name, value] : *families->members())
        if (std::find(family_names.begin(), family_names.end(), name) == family_names.end()) {
            error = theme.id + ": unknown family \"" + name + "\"";
            return std::nullopt;
        }
    return theme;
}

} // namespace

std::optional<Rgb> parse_colour(std::string_view text) {
    if (text.size() != 7 || text[0] != '#') return std::nullopt;
    int value[6];
    for (int i = 0; i < 6; ++i)
        if ((value[i] = hex_digit(text[static_cast<std::size_t>(i) + 1])) < 0) return std::nullopt;
    return Rgb{static_cast<std::uint8_t>(value[0] * 16 + value[1]), static_cast<std::uint8_t>(value[2] * 16 + value[3]),
               static_cast<std::uint8_t>(value[4] * 16 + value[5])};
}

std::string format_colour(Rgb colour) {
    char text[8];
    std::snprintf(text, sizeof text, "#%02x%02x%02x", colour.r, colour.g, colour.b);
    return text;
}

double luminance(Rgb colour) {
    return 0.2126 * channel(colour.r) + 0.7152 * channel(colour.g) + 0.0722 * channel(colour.b);
}

double contrast(Rgb a, Rgb b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Rgb Theme::surface(std::string_view name) const { return surfaces[index_of(surface_names, name)]; }
Rgb Theme::ink(std::string_view name) const { return inks[index_of(ink_names, name)]; }
const Family& Theme::family(std::string_view name) const { return families[index_of(family_names, name)]; }

std::vector<Rgb> Theme::colours() const {
    std::vector<Rgb> all(surfaces.begin(), surfaces.end());
    all.insert(all.end(), inks.begin(), inks.end());
    for (const auto& family : families) all.insert(all.end(), family.tones.begin(), family.tones.end());
    std::vector<Rgb> distinct;
    for (const auto& colour : all)
        if (std::find(distinct.begin(), distinct.end(), colour) == distinct.end()) distinct.push_back(colour);
    return distinct;
}

const Theme& Palettes::find(std::string_view id) const {
    for (const auto& theme : themes)
        if (theme.id == id) return theme;
    for (const auto& theme : themes)
        if (theme.id == default_id) return theme;
    return themes.front();
}

std::optional<Palettes> read_palettes(std::string_view text, std::string& error) {
    json::Error parse_error;
    const auto root = json::parse(text, parse_error);
    if (!root) {
        error = "line " + std::to_string(parse_error.line) + ": " + parse_error.message;
        return std::nullopt;
    }
    const auto* format = root->find("format");
    const auto* version = root->find("version");
    if (!format || !format->string() || *format->string() != "resend-palettes" || !version || !version->integer() ||
        *version->integer() != 1) {
        error = "not a resend-palettes file of version 1";
        return std::nullopt;
    }
    const auto* themes = root->find("themes");
    const auto* fallback = root->find("default");
    if (!themes || !themes->array_items() || themes->array_items()->empty() || !fallback || !fallback->string()) {
        error = "a palettes file needs themes and a default";
        return std::nullopt;
    }
    Palettes palettes;
    palettes.default_id = *fallback->string();
    for (const auto& item : *themes->array_items()) {
        auto theme = read_theme(item, error);
        if (!theme) return std::nullopt;
        for (const auto& other : palettes.themes)
            if (other.id == theme->id) {
                error = "two themes named " + theme->id;
                return std::nullopt;
            }
        palettes.themes.push_back(std::move(*theme));
    }
    if (std::none_of(palettes.themes.begin(), palettes.themes.end(), [&](const Theme& t) { return t.id == palettes.default_id; })) {
        error = "the default theme " + palettes.default_id + " is not in the file";
        return std::nullopt;
    }
    return palettes;
}

const Palettes& palettes() {
    static const Palettes built_in = [] {
        std::string error;
        auto read = read_palettes(palettes_text(), error);
        if (!read) throw std::runtime_error("assets/palettes.json: " + error);
        return std::move(*read);
    }();
    return built_in;
}

std::string role_of(char cell) {
    switch (cell) {
    case '#': return "outline";
    case ':': return "screen";
    case ',': return "well";
    case '*': return "ink";
    case 'o': return "status";
    default: break;
    }
    for (const auto& [family, chars] : family_chars)
        if (const auto at = chars.find(cell); at != std::string_view::npos)
            return std::string(family) + std::string(tone_suffixes[at]);
    return {};
}

std::optional<Rgb> colour_of(char cell, const Theme& theme) {
    switch (cell) {
    case '#': return theme.surface("outline");
    case ':': return theme.surface("screen");
    case ',': return theme.surface("sunken");
    case '*': return theme.ink("ink");
    case 'o': return theme.family("ok").tones[1];  // as a device that is on
    default: break;
    }
    for (const auto& [family, chars] : family_chars)
        if (const auto at = chars.find(cell); at != std::string_view::npos) return theme.family(family).tones[at];
    return std::nullopt;
}

std::vector<Sprite> read_sheet(std::string_view sheet, std::string_view text, std::vector<std::string>& errors) {
    std::vector<Sprite> sprites;
    std::vector<std::string_view> lines;
    for (std::size_t at = 0; at <= text.size();) {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        at = end + 1;
    }
    const auto fail = [&](std::size_t line, const std::string& message) {
        errors.push_back(std::string(sheet) + ":" + std::to_string(line + 1) + ": " + message);
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = lines[i];
        if (line.empty() || line.front() == '#') continue;
        Sprite sprite;
        sprite.sheet = std::string(sheet);
        char name[64] = {};
        int width = 0, height = 0;
        const std::string copy(line);
        if (std::sscanf(copy.c_str(), "sprite %63s %d %d", name, &width, &height) != 3) {
            fail(i, "expected \"sprite NAME WIDTH HEIGHT\"");
            continue;
        }
        sprite.name = name;
        sprite.width = width;
        sprite.height = height;
        if (width <= 0 || height <= 0 || width % 8 || height % 8 || width > 64 || height > 64) {
            fail(i, sprite.name + ": sizes are multiples of 8 up to 64");
            continue;
        }
        for (int row = 0; row < height; ++row) {
            const auto at = i + 1 + static_cast<std::size_t>(row);
            const auto cells = at < lines.size() ? lines[at] : std::string_view{};
            if (cells.size() != static_cast<std::size_t>(width)) {
                fail(at, sprite.name + ": row " + std::to_string(row + 1) + " is not " + std::to_string(width) + " cells wide");
                break;
            }
            for (const char cell : cells)
                if (cell != '.' && role_of(cell).empty()) fail(at, sprite.name + ": '" + std::string(1, cell) + "' is not in the legend");
            sprite.cells += cells;
        }
        i += static_cast<std::size_t>(height);
        if (sprite.cells.size() != static_cast<std::size_t>(width * height)) continue;
        if (std::any_of(sprites.begin(), sprites.end(), [&](const Sprite& s) { return s.name == sprite.name; })) {
            fail(i, "two sprites named " + sprite.name);
            continue;
        }
        sprites.push_back(std::move(sprite));
    }
    return sprites;
}

const std::vector<Sprite>& sprites() {
    static const std::vector<Sprite> all = [] {
        std::vector<Sprite> result;
        std::vector<std::string> errors;
        for (const auto& [name, text] : sheet_texts()) {
            auto sheet = read_sheet(name, text, errors);
            result.insert(result.end(), std::make_move_iterator(sheet.begin()), std::make_move_iterator(sheet.end()));
        }
        if (!errors.empty()) throw std::runtime_error("assets/sprites/" + errors.front());
        return result;
    }();
    return all;
}

const Sprite* find_sprite(std::string_view name) {
    for (const auto& sprite : sprites())
        if (sprite.name == name) return &sprite;
    return nullptr;
}

std::vector<std::uint8_t> paint(const Sprite& sprite, const Theme& theme) {
    std::vector<std::uint8_t> pixels(sprite.cells.size() * 4, 0);
    for (std::size_t i = 0; i < sprite.cells.size(); ++i)
        if (const auto colour = colour_of(sprite.cells[i], theme)) {
            pixels[i * 4] = colour->r;
            pixels[i * 4 + 1] = colour->g;
            pixels[i * 4 + 2] = colour->b;
            pixels[i * 4 + 3] = 255;
        }
    return pixels;
}

std::string_view state_name(DeviceState state) {
    switch (state) {
    case DeviceState::Off: return "off";
    case DeviceState::On: return "on";
    case DeviceState::Selected: return "selected";
    case DeviceState::Sending: return "sending";
    case DeviceState::Receiving: return "receiving";
    case DeviceState::Error: return "error";
    }
    return "on";
}

std::string_view state_name(PortState state) {
    switch (state) {
    case PortState::Free: return "free";
    case PortState::Connected: return "connected";
    case PortState::Down: return "down";
    case PortState::Active: return "active";
    }
    return "free";
}

namespace {
// A family's tone in the metal family: 0 for dd up to 3 for l; -1 when the
// cell is not a family's.
int tone_of(char cell) {
    for (const auto& [family, chars] : family_chars)
        if (const auto at = chars.find(cell); at != std::string_view::npos) return static_cast<int>(at);
    return -1;
}
} // namespace

char in_state(char cell, DeviceState state) {
    if (cell == 'o') {
        switch (state) {
        case DeviceState::Off: return '1';        // metal dd: dark
        case DeviceState::On: return 'b';         // ok d: lit, quietly
        case DeviceState::Selected: return 'c';   // ok base
        case DeviceState::Sending: return 'd';    // ok l: bright
        case DeviceState::Receiving: return 'h';  // link l
        case DeviceState::Error: return 'x';      // drop base
        }
    }
    switch (state) {
    case DeviceState::Off:
        if (const int tone = tone_of(cell); tone >= 0) return "1234"[tone];
        if (cell == '*') return '3';
        return cell;
    case DeviceState::Selected: return cell == '#' ? 's' : cell;  // an accent outline
    case DeviceState::Sending: return cell == ':' ? 'a' : cell;   // the screen lights up
    case DeviceState::Receiving: return cell == ':' ? 'e' : cell;
    case DeviceState::Error: return cell == ':' ? 'v' : cell;
    case DeviceState::On: return cell;
    }
    return cell;
}

char in_state(char cell, PortState state) {
    if (cell != 'o') return cell;
    switch (state) {
    case PortState::Free: return ',';       // an empty socket
    case PortState::Connected: return 'g';  // link base
    case PortState::Down: return 'k';       // ask base
    case PortState::Active: return 'd';     // ok l
    }
    return cell;
}

namespace {
template <typename State>
std::vector<std::uint8_t> paint_in(const Sprite& sprite, const Theme& theme, State state) {
    Sprite changed = sprite;
    for (auto& cell : changed.cells) cell = in_state(cell, state);
    return paint(changed, theme);
}
} // namespace

std::vector<std::uint8_t> paint(const Sprite& sprite, const Theme& theme, DeviceState state) { return paint_in(sprite, theme, state); }
std::vector<std::uint8_t> paint(const Sprite& sprite, const Theme& theme, PortState state) { return paint_in(sprite, theme, state); }

std::vector<std::uint8_t> grid_tile(const Theme& theme, int size) {
    size = std::max(size, 4);
    // Opaque, on the canvas's colour: the renderer copies it instead of blending.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 255);
    const auto ground = theme.surface("canvas");
    for (std::size_t at = 0; at < pixels.size(); at += 4) {
        pixels[at] = ground.r;
        pixels[at + 1] = ground.g;
        pixels[at + 2] = ground.b;
    }
    const auto put = [&](int x, int y, Rgb colour) {
        const auto at = static_cast<std::size_t>((y % size) * size + (x % size)) * 4;
        pixels[at] = colour.r;
        pixels[at + 1] = colour.g;
        pixels[at + 2] = colour.b;
        pixels[at + 3] = 255;
    };
    const auto line = theme.surface("grid"), stud = theme.surface("edge");
    for (int i = 0; i < size; ++i) {
        put(i, 0, line);
        put(0, i, line);
    }
    // A plus of three pixels each way where the lines cross.
    for (int i = -1; i <= 1; ++i) {
        put((i + size) % size, 0, stud);
        put(0, (i + size) % size, stud);
    }
    return pixels;
}

float art_pixel(float wanted, float scale_factor) {
    if (!(scale_factor > 0)) scale_factor = 1;
    return std::max(1.0f, std::round(wanted * scale_factor)) / scale_factor;
}

CanvasArt canvas_art(float zoom, float scale_factor) {
    if (!(scale_factor > 0)) scale_factor = 1;
    CanvasArt art;
    const auto nearest = [&](float wanted) { return std::max(1.0f, std::round(wanted * scale_factor)) / scale_factor; };
    art.packet_unit = nearest(1.5f * zoom);
    art.port_unit = nearest(zoom);
    if (std::round(zoom * scale_factor * icon_room / 32) >= 1) {
        art.unit = nearest(zoom * icon_room / 32);
        return art;
    }
    art.small = true;
    art.unit = nearest(zoom * icon_room / 16);
    return art;
}

float snap(float logical, float scale_factor) {
    if (!(scale_factor > 0)) scale_factor = 1;
    return std::round(logical * scale_factor) / scale_factor;
}

} // namespace resend::look
